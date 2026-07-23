#include "yappo_core_http_v2.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "yappo_http_v2.h"

typedef struct {
  unsigned char *data;
  size_t bytes;
  int failed;
} response_buffer_t;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_global_status = CURLE_FAILED_INIT;

static void initialize_curl(void) {
  curl_global_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

void YAP_V2_core_http_request_init(YAP_V2_CORE_HTTP_REQUEST *request) {
  if (request != NULL) memset(request, 0, sizeof(*request));
}

void YAP_V2_core_http_request_free(YAP_V2_CORE_HTTP_REQUEST *request) {
  if (request == NULL) return;
  free(request->body);
  YAP_V2_core_http_request_init(request);
}

void YAP_V2_core_http_response_init(YAP_V2_CORE_HTTP_RESPONSE *response) {
  if (response != NULL) memset(response, 0, sizeof(*response));
}

void YAP_V2_core_http_response_free(YAP_V2_CORE_HTTP_RESPONSE *response) {
  if (response == NULL) return;
  free(response->body);
  YAP_V2_core_http_response_init(response);
}

static int parse_content_length(const char *value, size_t *length) {
  size_t parsed = 0U;
  const unsigned char *cursor = (const unsigned char *)value;
  if (*cursor == '\0') return -1;
  while (*cursor != '\0') {
    size_t digit;
    if (*cursor < '0' || *cursor > '9') return -1;
    digit = (size_t)(*cursor - '0');
    if (parsed > (SIZE_MAX - digit) / 10U) return -1;
    parsed = parsed * 10U + digit;
    cursor++;
  }
  *length = parsed;
  return 0;
}

static int header_name_valid(const char *name) {
  const unsigned char *cursor = (const unsigned char *)name;
  if (*cursor == '\0') return 0;
  for (; *cursor != '\0'; cursor++) {
    if ((*cursor >= '0' && *cursor <= '9') ||
        (*cursor >= 'A' && *cursor <= 'Z') ||
        (*cursor >= 'a' && *cursor <= 'z') ||
        strchr("!#$%&'*+-.^_`|~", *cursor) != NULL)
      continue;
    return 0;
  }
  return 1;
}

static char *trim_value(char *value) {
  char *end;
  while (*value == ' ' || *value == '\t') value++;
  end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *end = '\0';
  return value;
}

int YAP_V2_core_http_parse_head(const unsigned char *input, size_t input_bytes,
                                YAP_V2_CORE_HTTP_REQUEST *request) {
  char *copy = NULL, *cursor, *line_end;
  char version[32], trailing;
  int have_host = 0, have_type = 0, have_authorization = 0;
  if (input == NULL || request == NULL || input_bytes < 4U ||
      input_bytes > YAP_V2_CORE_HTTP_MAX_HEADER_BYTES)
    return YAP_V2_CORE_HTTP_INVALID_ARGUMENT;
  if (input[input_bytes - 4U] != '\r' || input[input_bytes - 3U] != '\n' ||
      input[input_bytes - 2U] != '\r' || input[input_bytes - 1U] != '\n')
    return YAP_V2_CORE_HTTP_INVALID;
  copy = malloc(input_bytes + 1U);
  if (copy == NULL) return YAP_V2_CORE_HTTP_NO_MEMORY;
  memcpy(copy, input, input_bytes);
  copy[input_bytes] = '\0';
  YAP_V2_core_http_request_free(request);
  cursor = copy;
  line_end = strstr(cursor, "\r\n");
  if (line_end == NULL || (size_t)(line_end - cursor) > YAP_V2_CORE_HTTP_MAX_LINE_BYTES) {
    free(copy);
    return YAP_V2_CORE_HTTP_INVALID;
  }
  *line_end = '\0';
  if (sscanf(cursor, "%15s %255s %31s %c", request->method, request->target,
             version, &trailing) != 3 || strcmp(version, "HTTP/1.1") != 0) {
    free(copy);
    return YAP_V2_CORE_HTTP_INVALID;
  }
  cursor = line_end + 2;
  while (*cursor != '\0') {
    char *colon, *value;
    line_end = strstr(cursor, "\r\n");
    if (line_end == NULL || (size_t)(line_end - cursor) > YAP_V2_CORE_HTTP_MAX_LINE_BYTES) {
      free(copy);
      return YAP_V2_CORE_HTTP_INVALID;
    }
    if (line_end == cursor) break;
    *line_end = '\0';
    colon = strchr(cursor, ':');
    if (colon == NULL || colon == cursor) {
      free(copy);
      return YAP_V2_CORE_HTTP_INVALID;
    }
    *colon = '\0';
    if (!header_name_valid(cursor)) {
      free(copy);
      return YAP_V2_CORE_HTTP_INVALID;
    }
    value = trim_value(colon + 1);
    if (strcasecmp(cursor, "Host") == 0) {
      if (have_host || *value == '\0') {
        free(copy);
        return YAP_V2_CORE_HTTP_INVALID;
      }
      have_host = 1;
    } else if (strcasecmp(cursor, "Content-Type") == 0) {
      if (have_type) {
        free(copy);
        return YAP_V2_CORE_HTTP_INVALID;
      }
      have_type = 1;
      request->json_content_type =
        strncasecmp(value, "application/json", 16U) == 0 &&
        (value[16] == '\0' || value[16] == ';');
    } else if (strcasecmp(cursor, "Content-Length") == 0) {
      if (request->have_content_length ||
          parse_content_length(value, &request->content_length) != 0) {
        free(copy);
        return YAP_V2_CORE_HTTP_INVALID;
      }
      request->have_content_length = 1;
    } else if (strcasecmp(cursor, "Transfer-Encoding") == 0) {
      free(copy);
      return YAP_V2_CORE_HTTP_INVALID;
    } else if (strcasecmp(cursor, "Authorization") == 0) {
      size_t value_bytes = strlen(value);
      if (have_authorization || value_bytes == 0U ||
          value_bytes > YAP_V2_AUTHORIZATION_MAX_BYTES) {
        free(copy);
        return YAP_V2_CORE_HTTP_INVALID;
      }
      memcpy(request->authorization, value, value_bytes + 1U);
      have_authorization = 1;
    }
    cursor = line_end + 2;
  }
  free(copy);
  return have_host ? YAP_V2_CORE_HTTP_OK : YAP_V2_CORE_HTTP_INVALID;
}

int YAP_V2_core_http_read_request(FILE *stream, size_t max_body_bytes,
                                  YAP_V2_CORE_HTTP_REQUEST *request) {
  unsigned char *head;
  size_t used = 0U;
  int status;
  if (stream == NULL || request == NULL || max_body_bytes == 0U)
    return YAP_V2_CORE_HTTP_INVALID_ARGUMENT;
  head = malloc(YAP_V2_CORE_HTTP_MAX_HEADER_BYTES);
  if (head == NULL) return YAP_V2_CORE_HTTP_NO_MEMORY;
  while (used < YAP_V2_CORE_HTTP_MAX_HEADER_BYTES) {
    int byte = fgetc(stream);
    if (byte == EOF) {
      free(head);
      if (used == 0U && feof(stream)) return YAP_V2_CORE_HTTP_EOF;
      return YAP_V2_CORE_HTTP_IO_ERROR;
    }
    head[used++] = (unsigned char)byte;
    if (used >= 4U && memcmp(head + used - 4U, "\r\n\r\n", 4U) == 0) break;
  }
  if (used == YAP_V2_CORE_HTTP_MAX_HEADER_BYTES &&
      memcmp(head + used - 4U, "\r\n\r\n", 4U) != 0) {
    free(head);
    return YAP_V2_CORE_HTTP_TOO_LARGE;
  }
  status = YAP_V2_core_http_parse_head(head, used, request);
  free(head);
  if (status != YAP_V2_CORE_HTTP_OK) return status;
  if (request->have_content_length && request->content_length > max_body_bytes)
    return YAP_V2_CORE_HTTP_TOO_LARGE;
  if (!request->have_content_length || request->content_length == 0U)
    return YAP_V2_CORE_HTTP_OK;
  request->body = malloc(request->content_length);
  if (request->body == NULL) return YAP_V2_CORE_HTTP_NO_MEMORY;
  if (fread(request->body, 1U, request->content_length, stream) != request->content_length) {
    YAP_V2_core_http_request_free(request);
    return YAP_V2_CORE_HTTP_IO_ERROR;
  }
  request->body_bytes = request->content_length;
  return YAP_V2_CORE_HTTP_OK;
}

static const char *reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    default: return "Service Unavailable";
  }
}

int YAP_V2_core_http_write_response(FILE *stream, int status, const char *content_type,
                                    const char *allow, int accept_query,
                                    const void *body, size_t body_bytes) {
  if (stream == NULL || content_type == NULL ||
      (body_bytes != 0U && body == NULL) ||
      body_bytes > YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES)
    return YAP_V2_CORE_HTTP_INVALID_ARGUMENT;
  if (fprintf(stream,
              "HTTP/1.1 %d %s\r\nServer: Yappo Search Core/2.0\r\n"
              "Content-Type: %s\r\nContent-Length: %zu\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n",
              status, reason_phrase(status), content_type, body_bytes) < 0)
    return YAP_V2_CORE_HTTP_IO_ERROR;
  if (allow != NULL && fprintf(stream, "Allow: %s\r\n", allow) < 0)
    return YAP_V2_CORE_HTTP_IO_ERROR;
  if (accept_query && fputs("Accept-Query: application/json\r\n", stream) == EOF)
    return YAP_V2_CORE_HTTP_IO_ERROR;
  if (fputs("\r\n", stream) == EOF ||
      (body_bytes != 0U && fwrite(body, 1U, body_bytes, stream) != body_bytes) ||
      fflush(stream) != 0)
    return YAP_V2_CORE_HTTP_IO_ERROR;
  return YAP_V2_CORE_HTTP_OK;
}

static size_t receive_response(void *contents, size_t size, size_t count, void *opaque) {
  response_buffer_t *buffer = opaque;
  size_t bytes;
  unsigned char *next;
  if (size != 0U && count > SIZE_MAX / size) {
    buffer->failed = 1;
    return 0U;
  }
  bytes = size * count;
  if (bytes > YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES - buffer->bytes) {
    buffer->failed = 1;
    return 0U;
  }
  next = realloc(buffer->data, buffer->bytes + bytes + 1U);
  if (next == NULL) {
    buffer->failed = 1;
    return 0U;
  }
  buffer->data = next;
  if (bytes != 0U) memcpy(buffer->data + buffer->bytes, contents, bytes);
  buffer->bytes += bytes;
  buffer->data[buffer->bytes] = '\0';
  return bytes;
}

static int append_header(struct curl_slist **headers, const char *value) {
  struct curl_slist *next = curl_slist_append(*headers, value);
  if (next == NULL) return -1;
  *headers = next;
  return 0;
}

static int build_url(const char *host, int port, const char *target,
                     char *url, size_t capacity) {
  int written;
  if (strchr(host, ':') != NULL && host[0] != '[')
    written = snprintf(url, capacity, "http://[%s]:%d%s", host, port, target);
  else
    written = snprintf(url, capacity, "http://%s:%d%s", host, port, target);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

int YAP_V2_core_http_client_request(const char *host, int port, uint32_t timeout_ms,
                                    const char *method, const char *target,
                                    const char *authorization, const void *body,
                                    size_t body_bytes, YAP_V2_CORE_HTTP_RESPONSE *response) {
  CURL *curl = NULL;
  CURLcode code;
  struct curl_slist *headers = NULL;
  response_buffer_t buffer = {0};
  char url[768], authorization_header[YAP_V2_AUTHORIZATION_MAX_BYTES + 32U];
  char *content_type = NULL;
  long http_status = 0L;
  int result = YAP_V2_CORE_HTTP_IO_ERROR;
  if (host == NULL || port < 1 || port > 65535 || timeout_ms == 0U ||
      method == NULL || target == NULL || response == NULL ||
      (body_bytes != 0U && body == NULL) || body_bytes > YAP_V2_HTTP_MAX_BODY_BYTES ||
      build_url(host, port, target, url, sizeof(url)) != 0)
    return YAP_V2_CORE_HTTP_INVALID_ARGUMENT;
  YAP_V2_core_http_response_free(response);
  if (pthread_once(&curl_once, initialize_curl) != 0 || curl_global_status != CURLE_OK)
    return YAP_V2_CORE_HTTP_IO_ERROR;
  curl = curl_easy_init();
  if (curl == NULL) return YAP_V2_CORE_HTTP_IO_ERROR;
  if (append_header(&headers, "Accept: application/json") != 0 ||
      append_header(&headers, "Connection: close") != 0 ||
      append_header(&headers, "Expect:") != 0)
    goto done;
  if (body_bytes != 0U && append_header(&headers, "Content-Type: application/json") != 0)
    goto done;
  if (authorization != NULL && authorization[0] != '\0') {
    int written = snprintf(authorization_header, sizeof(authorization_header),
                           "Authorization: %s", authorization);
    if (written < 0 || (size_t)written >= sizeof(authorization_header) ||
        append_header(&headers, authorization_header) != 0)
      goto done;
  }
  if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) != CURLE_OK)
    goto done;
  if (body_bytes != 0U &&
      (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
       curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_bytes) != CURLE_OK))
    goto done;
  code = curl_easy_perform(curl);
  if (code != CURLE_OK || buffer.failed ||
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK ||
      curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type) != CURLE_OK ||
      http_status < 100L || http_status > 599L || content_type == NULL ||
      strncasecmp(content_type, "application/json", 16U) != 0 ||
      (content_type[16] != '\0' && content_type[16] != ';') ||
      buffer.data == NULL)
    goto done;
  response->status = (int)http_status;
  response->body = buffer.data;
  response->body_bytes = buffer.bytes;
  buffer.data = NULL;
  result = YAP_V2_CORE_HTTP_OK;
done:
  free(buffer.data);
  curl_slist_free_all(headers);
  if (curl != NULL) curl_easy_cleanup(curl);
  return result;
}
