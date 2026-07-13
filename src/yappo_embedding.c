#include "yappo_embedding.h"

#include <curl/curl.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <yyjson.h>
#include <unicode/utf8.h>

#define YAP_EMBEDDING_MAX_INPUTS 1000000U
#define YAP_EMBEDDING_MAX_RESPONSE_BYTES (256U * 1024U * 1024U)
#define YAP_EMBEDDING_MAX_VECTOR_VALUES (YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES / sizeof(float))

typedef struct {
  char *data;
  size_t len;
} HTTP_BUFFER;

typedef struct {
  char *id;
  size_t id_len;
  float *values;
} PRECOMPUTED_ENTRY;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_global_status = CURLE_FAILED_INIT;

static void initialize_curl(void) { curl_global_status = curl_global_init(CURL_GLOBAL_DEFAULT); }

const char *YAP_Embedding_status_string(YAP_EMBEDDING_STATUS status) {
  switch (status) {
    case YAP_EMBEDDING_OK: return "ok";
    case YAP_EMBEDDING_INVALID_ARGUMENT: return "invalid argument";
    case YAP_EMBEDDING_IO_ERROR: return "I/O error";
    case YAP_EMBEDDING_HTTP_ERROR: return "HTTP error";
    case YAP_EMBEDDING_INVALID_RESPONSE: return "invalid response";
    case YAP_EMBEDDING_DIMENSION_MISMATCH: return "dimension mismatch";
    case YAP_EMBEDDING_NON_FINITE: return "non-finite embedding value";
    case YAP_EMBEDDING_ALLOCATION_FAILED: return "allocation failed";
    case YAP_EMBEDDING_MISSING_ID: return "missing embedding ID";
    case YAP_EMBEDDING_DUPLICATE_ID: return "duplicate embedding ID";
    default: return "unknown status";
  }
}

void YAP_Embedding_result_init(YAP_EMBEDDING_RESULT *result) {
  if (result != NULL) memset(result, 0, sizeof(*result));
}

void YAP_Embedding_result_free(YAP_EMBEDDING_RESULT *result) {
  if (result == NULL) return;
  free(result->values);
  memset(result, 0, sizeof(*result));
}

void YAP_Embedding_http_config_init(YAP_EMBEDDING_HTTP_CONFIG *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->batch_size = 64U;
  config->timeout_ms = 30000U;
  config->max_retries = 2U;
  config->retry_backoff_ms = 100U;
}

static int input_validate(const YAP_EMBEDDING_INPUT *inputs, size_t count, int require_text) {
  size_t i;
  if (inputs == NULL || count == 0U || count > YAP_EMBEDDING_MAX_INPUTS) return 0;
  for (i = 0U; i < count; i++) {
    int32_t offset;
    UChar32 codepoint;
    if (inputs[i].id.data == NULL || inputs[i].id.len == 0U ||
        inputs[i].id.len > YAP_V2_MAX_IDENTIFIER_BYTES ||
        (require_text && (inputs[i].text.data == NULL || inputs[i].text.len == 0U)) ||
        inputs[i].text.len > YAP_V2_MAX_METADATA_BYTES ||
        memchr(inputs[i].id.data, '\0', inputs[i].id.len) != NULL ||
        (inputs[i].text.len > 0U && (inputs[i].text.data == NULL ||
         memchr(inputs[i].text.data, '\0', inputs[i].text.len) != NULL)) ||
        inputs[i].id.len > INT32_MAX || inputs[i].text.len > INT32_MAX) return 0;
    offset = 0;
    while (offset < (int32_t)inputs[i].id.len) {
      U8_NEXT(inputs[i].id.data, offset, (int32_t)inputs[i].id.len, codepoint);
      if (codepoint < 0) return 0;
    }
    offset = 0;
    while (offset < (int32_t)inputs[i].text.len) {
      U8_NEXT(inputs[i].text.data, offset, (int32_t)inputs[i].text.len, codepoint);
      if (codepoint < 0) return 0;
    }
  }
  return 1;
}

static size_t curl_write(char *data, size_t size, size_t count, void *context) {
  HTTP_BUFFER *buffer = context;
  size_t bytes;
  char *next;
  if (size != 0U && count > SIZE_MAX / size) return 0U;
  bytes = size * count;
  if (bytes > YAP_EMBEDDING_MAX_RESPONSE_BYTES - buffer->len) return 0U;
  next = realloc(buffer->data, buffer->len + bytes + 1U);
  if (next == NULL) return 0U;
  buffer->data = next;
  memcpy(buffer->data + buffer->len, data, bytes);
  buffer->len += bytes;
  buffer->data[buffer->len] = '\0';
  return bytes;
}

static int curl_transport(void *context, const char *endpoint, const char *authorization,
                          const char *request_body, size_t request_bytes, uint32_t timeout_ms,
                          char **response_body, size_t *response_bytes, long *http_status) {
  CURL *curl;
  CURLcode code;
  struct curl_slist *headers = NULL;
  HTTP_BUFFER buffer = {0};
  char authorization_header[2048];
  (void)context;
  if (pthread_once(&curl_once, initialize_curl) != 0 || curl_global_status != CURLE_OK)
    return YAP_EMBEDDING_IO_ERROR;
  curl = curl_easy_init();
  if (curl == NULL) return YAP_EMBEDDING_IO_ERROR;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (authorization != NULL && authorization[0] != '\0') {
    int length = snprintf(authorization_header, sizeof(authorization_header),
                          "Authorization: Bearer %s", authorization);
    if (length < 0 || (size_t)length >= sizeof(authorization_header)) {
      curl_slist_free_all(headers); curl_easy_cleanup(curl); return YAP_EMBEDDING_INVALID_ARGUMENT;
    }
    headers = curl_slist_append(headers, authorization_header);
  }
  if (headers == NULL || curl_easy_setopt(curl, CURLOPT_URL, endpoint) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_bytes) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) != CURLE_OK) {
    curl_slist_free_all(headers); curl_easy_cleanup(curl); free(buffer.data);
    return YAP_EMBEDDING_IO_ERROR;
  }
  code = curl_easy_perform(curl);
  if (code == CURLE_OK) code = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
  curl_slist_free_all(headers); curl_easy_cleanup(curl);
  if (code != CURLE_OK) { free(buffer.data); return YAP_EMBEDDING_IO_ERROR; }
  if (buffer.data == NULL) {
    buffer.data = malloc(1U);
    if (buffer.data == NULL) return YAP_EMBEDDING_ALLOCATION_FAILED;
    buffer.data[0] = '\0';
  }
  *response_body = buffer.data; *response_bytes = buffer.len;
  return YAP_EMBEDDING_OK;
}

static void default_sleep(void *context, uint32_t milliseconds) {
  struct timespec requested;
  (void)context;
  requested.tv_sec = (time_t)(milliseconds / 1000U);
  requested.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
  while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {}
}

static int make_request(const char *model_id, const YAP_EMBEDDING_INPUT *inputs, size_t count,
                        char **body, size_t *body_bytes) {
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root, *array;
  yyjson_write_err error;
  size_t i;
  if (document == NULL) return YAP_EMBEDDING_ALLOCATION_FAILED;
  root = yyjson_mut_obj(document); array = yyjson_mut_arr(document);
  if (root == NULL || array == NULL) {
    yyjson_mut_doc_free(document); return YAP_EMBEDDING_ALLOCATION_FAILED;
  }
  yyjson_mut_doc_set_root(document, root);
  if (!yyjson_mut_obj_add_str(document, root, "model", model_id) ||
      !yyjson_mut_obj_add_val(document, root, "input", array)) {
    yyjson_mut_doc_free(document); return YAP_EMBEDDING_ALLOCATION_FAILED;
  }
  for (i = 0U; i < count; i++) {
    yyjson_mut_val *value = yyjson_mut_strncpy(document, (const char *)inputs[i].text.data,
                                                inputs[i].text.len);
    if (value == NULL || !yyjson_mut_arr_append(array, value)) {
      yyjson_mut_doc_free(document); return YAP_EMBEDDING_ALLOCATION_FAILED;
    }
  }
  *body = yyjson_mut_write_opts(document, YYJSON_WRITE_NOFLAG, NULL, body_bytes, &error);
  yyjson_mut_doc_free(document);
  return *body == NULL ? YAP_EMBEDDING_ALLOCATION_FAILED : YAP_EMBEDDING_OK;
}

static int only_response_keys(yyjson_val *object) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  if (!yyjson_is_obj(object)) return 0;
  iterator = yyjson_obj_iter_with(object);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key);
    if (strcmp(name, "data") != 0 && strcmp(name, "model") != 0 && strcmp(name, "usage") != 0 &&
        strcmp(name, "object") != 0) return 0;
  }
  return 1;
}

static int only_data_keys(yyjson_val *object) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  if (!yyjson_is_obj(object)) return 0;
  iterator = yyjson_obj_iter_with(object);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key);
    if (strcmp(name, "index") != 0 && strcmp(name, "embedding") != 0 &&
        strcmp(name, "object") != 0) return 0;
  }
  return 1;
}

static int parse_response(const char *body, size_t body_bytes, size_t count, size_t dimensions,
                          float *output) {
  yyjson_doc *document;
  yyjson_val *root, *data, *item;
  yyjson_arr_iter iterator;
  unsigned char *seen;
  int status = YAP_EMBEDDING_INVALID_RESPONSE;
  document = yyjson_read(body, body_bytes, YYJSON_READ_NOFLAG);
  if (document == NULL) return status;
  root = yyjson_doc_get_root(document); data = yyjson_obj_get(root, "data");
  if (!only_response_keys(root) || !yyjson_is_arr(data) || yyjson_arr_size(data) != count) goto done;
  seen = calloc(count, 1U);
  if (seen == NULL) { status = YAP_EMBEDDING_ALLOCATION_FAILED; goto done; }
  yyjson_arr_iter_init(data, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    yyjson_val *index = yyjson_obj_get(item, "index");
    yyjson_val *embedding = yyjson_obj_get(item, "embedding");
    yyjson_arr_iter values;
    yyjson_val *value;
    size_t position = 0U, ordinal;
    if (!only_data_keys(item) || yyjson_obj_size(item) < 2U || !yyjson_is_uint(index) ||
        yyjson_get_uint(index) >= count || !yyjson_is_arr(embedding) ||
        yyjson_arr_size(embedding) != dimensions) {
      if (yyjson_is_arr(embedding) && yyjson_arr_size(embedding) != dimensions)
        status = YAP_EMBEDDING_DIMENSION_MISMATCH;
      goto parse_done;
    }
    ordinal = (size_t)yyjson_get_uint(index);
    if (seen[ordinal]) goto parse_done;
    seen[ordinal] = 1U;
    yyjson_arr_iter_init(embedding, &values);
    while ((value = yyjson_arr_iter_next(&values)) != NULL) {
      double number;
      if (!yyjson_is_num(value)) goto parse_done;
      number = yyjson_get_num(value);
      if (!isfinite(number) || number > (double)FLT_MAX || number < -(double)FLT_MAX) {
        status = YAP_EMBEDDING_NON_FINITE; goto parse_done;
      }
      output[ordinal * dimensions + position++] = (float)number;
    }
  }
  status = YAP_EMBEDDING_OK;
parse_done:
  free(seen);
done:
  yyjson_doc_free(document);
  return status;
}

static int retryable(long http_status, int transport_status) {
  return transport_status != YAP_EMBEDDING_OK || http_status == 408L || http_status == 429L ||
         http_status >= 500L;
}

int YAP_Embedding_http(const YAP_EMBEDDING_HTTP_CONFIG *config,
                       const YAP_EMBEDDING_INPUT *inputs, size_t input_count,
                       YAP_EMBEDDING_RESULT *result) {
  YAP_EMBEDDING_TRANSPORT transport;
  YAP_EMBEDDING_SLEEP sleep_fn;
  float *values;
  size_t offset;
  int status = YAP_EMBEDDING_OK;
  if (config == NULL || result == NULL || config->endpoint == NULL || config->endpoint[0] == '\0' ||
      config->model_id == NULL || config->model_id[0] == '\0' || config->dimensions == 0U ||
      config->dimensions > YAP_V2_MAX_VECTOR_DIMENSIONS || config->batch_size == 0U ||
      config->batch_size > 1024U || config->timeout_ms == 0U || config->max_retries > 10U ||
      strlen(config->endpoint) > 2048U || strlen(config->model_id) > YAP_V2_MAX_MODEL_ID_BYTES ||
      (config->api_key != NULL && strlen(config->api_key) > 1900U) ||
      !input_validate(inputs, input_count, 1) ||
      input_count > SIZE_MAX / config->dimensions ||
      input_count * config->dimensions > SIZE_MAX / sizeof(*values) ||
      input_count * config->dimensions > YAP_EMBEDDING_MAX_VECTOR_VALUES)
    return YAP_EMBEDDING_INVALID_ARGUMENT;
  values = calloc(input_count * config->dimensions, sizeof(*values));
  if (values == NULL) return YAP_EMBEDDING_ALLOCATION_FAILED;
  transport = config->transport == NULL ? curl_transport : config->transport;
  sleep_fn = config->sleep == NULL ? default_sleep : config->sleep;
  for (offset = 0U; offset < input_count; offset += config->batch_size) {
    size_t count = input_count - offset < config->batch_size ? input_count - offset : config->batch_size;
    char *request = NULL;
    size_t request_bytes = 0U;
    uint32_t attempt;
    status = make_request(config->model_id, inputs + offset, count, &request, &request_bytes);
    if (status != YAP_EMBEDDING_OK) break;
    for (attempt = 0U; attempt <= config->max_retries; attempt++) {
      char *response = NULL;
      size_t response_bytes = 0U;
      long http_status = 0L;
      int transport_status = transport(config->transport_context, config->endpoint, config->api_key,
                                       request, request_bytes, config->timeout_ms, &response,
                                       &response_bytes, &http_status);
      if (transport_status == YAP_EMBEDDING_OK && http_status >= 200L && http_status < 300L) {
        status = parse_response(response, response_bytes, count, config->dimensions,
                                values + offset * config->dimensions);
        free(response);
        break;
      }
      free(response);
      status = transport_status == YAP_EMBEDDING_OK ? YAP_EMBEDDING_HTTP_ERROR : transport_status;
      if (attempt == config->max_retries || !retryable(http_status, transport_status)) break;
      {
        uint64_t delay = attempt >= 32U ? UINT64_MAX :
                         (uint64_t)config->retry_backoff_ms << attempt;
        sleep_fn(config->sleep_context, delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay);
      }
    }
    free(request);
    if (status != YAP_EMBEDDING_OK) break;
  }
  if (status != YAP_EMBEDDING_OK) { free(values); return status; }
  YAP_Embedding_result_free(result);
  result->values = values; result->input_count = input_count; result->dimensions = config->dimensions;
  return YAP_EMBEDDING_OK;
}

static void precomputed_free(PRECOMPUTED_ENTRY *entries, size_t count) {
  size_t i;
  for (i = 0U; i < count; i++) { free(entries[i].id); free(entries[i].values); }
  free(entries);
}

static int parse_precomputed_line(const char *line, size_t length, size_t dimensions,
                                  PRECOMPUTED_ENTRY *entry) {
  yyjson_doc *document = yyjson_read(line, length, YYJSON_READ_NOFLAG);
  yyjson_val *root, *id, *embedding, *value;
  yyjson_arr_iter iterator;
  size_t i = 0U;
  int status = YAP_EMBEDDING_INVALID_RESPONSE;
  if (document == NULL) return status;
  root = yyjson_doc_get_root(document); id = yyjson_obj_get(root, "id");
  embedding = yyjson_obj_get(root, "embedding");
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 2U || !yyjson_is_str(id) ||
      yyjson_get_len(id) == 0U || yyjson_get_len(id) > YAP_V2_MAX_IDENTIFIER_BYTES ||
      !yyjson_is_arr(embedding) || yyjson_arr_size(embedding) != dimensions) {
    if (yyjson_is_arr(embedding) && yyjson_arr_size(embedding) != dimensions)
      status = YAP_EMBEDDING_DIMENSION_MISMATCH;
    goto done;
  }
  entry->id = malloc(yyjson_get_len(id) + 1U);
  entry->values = malloc(dimensions * sizeof(*entry->values));
  if (entry->id == NULL || entry->values == NULL) { status = YAP_EMBEDDING_ALLOCATION_FAILED; goto done; }
  memcpy(entry->id, yyjson_get_str(id), yyjson_get_len(id)); entry->id[yyjson_get_len(id)] = '\0';
  entry->id_len = yyjson_get_len(id);
  yyjson_arr_iter_init(embedding, &iterator);
  while ((value = yyjson_arr_iter_next(&iterator)) != NULL) {
    double number;
    if (!yyjson_is_num(value)) goto done;
    number = yyjson_get_num(value);
    if (!isfinite(number) || number > (double)FLT_MAX || number < -(double)FLT_MAX) {
      status = YAP_EMBEDDING_NON_FINITE; goto done;
    }
    entry->values[i++] = (float)number;
  }
  status = YAP_EMBEDDING_OK;
done:
  if (status != YAP_EMBEDDING_OK) { free(entry->id); free(entry->values); memset(entry, 0, sizeof(*entry)); }
  yyjson_doc_free(document);
  return status;
}

int YAP_Embedding_precomputed_read(const char *path, const YAP_EMBEDDING_INPUT *inputs,
                                   size_t input_count, size_t dimensions,
                                   YAP_EMBEDDING_RESULT *result) {
  FILE *file;
  char *line = NULL;
  size_t capacity = 0U, count = 0U, entries_capacity = 0U, i, j;
  ssize_t length;
  PRECOMPUTED_ENTRY *entries = NULL;
  float *values = NULL;
  int status = YAP_EMBEDDING_OK;
  if (path == NULL || result == NULL || dimensions == 0U ||
      dimensions > YAP_V2_MAX_VECTOR_DIMENSIONS || !input_validate(inputs, input_count, 0) ||
      input_count > SIZE_MAX / dimensions || input_count * dimensions > SIZE_MAX / sizeof(*values) ||
      input_count * dimensions > YAP_EMBEDDING_MAX_VECTOR_VALUES)
    return YAP_EMBEDDING_INVALID_ARGUMENT;
  file = fopen(path, "r");
  if (file == NULL) return YAP_EMBEDDING_IO_ERROR;
  while ((length = getline(&line, &capacity, file)) >= 0) {
    PRECOMPUTED_ENTRY *next;
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) length--;
    if (length == 0) { status = YAP_EMBEDDING_INVALID_RESPONSE; break; }
    if (count == entries_capacity) {
      size_t next_capacity = entries_capacity == 0U ? 64U : entries_capacity * 2U;
      if (next_capacity < entries_capacity || next_capacity > YAP_EMBEDDING_MAX_INPUTS) {
        status = YAP_EMBEDDING_ALLOCATION_FAILED; break;
      }
      next = realloc(entries, next_capacity * sizeof(*entries));
      if (next == NULL) { status = YAP_EMBEDDING_ALLOCATION_FAILED; break; }
      entries = next; memset(entries + entries_capacity, 0,
                             (next_capacity - entries_capacity) * sizeof(*entries));
      entries_capacity = next_capacity;
    }
    status = parse_precomputed_line(line, (size_t)length, dimensions, &entries[count]);
    if (status != YAP_EMBEDDING_OK) break;
    count++;
    for (i = 0U; i + 1U < count; i++) if (entries[i].id_len == entries[count - 1U].id_len &&
        memcmp(entries[i].id, entries[count - 1U].id, entries[count - 1U].id_len) == 0) {
      status = YAP_EMBEDDING_DUPLICATE_ID; break;
    }
    if (status != YAP_EMBEDDING_OK) break;
  }
  if (ferror(file)) status = YAP_EMBEDDING_IO_ERROR;
  free(line); fclose(file);
  if (status != YAP_EMBEDDING_OK) { precomputed_free(entries, count); return status; }
  values = malloc(input_count * dimensions * sizeof(*values));
  if (values == NULL) { precomputed_free(entries, count); return YAP_EMBEDDING_ALLOCATION_FAILED; }
  for (i = 0U; i < input_count; i++) {
    size_t found = SIZE_MAX;
    for (j = 0U; j < count; j++) if (inputs[i].id.len == entries[j].id_len &&
        memcmp(inputs[i].id.data, entries[j].id, entries[j].id_len) == 0) {
      if (found != SIZE_MAX) { status = YAP_EMBEDDING_DUPLICATE_ID; break; }
      found = j;
    }
    if (status != YAP_EMBEDDING_OK) break;
    if (found == SIZE_MAX) { status = YAP_EMBEDDING_MISSING_ID; break; }
    memcpy(values + i * dimensions, entries[found].values, dimensions * sizeof(*values));
  }
  precomputed_free(entries, count);
  if (status != YAP_EMBEDDING_OK) { free(values); return status; }
  YAP_Embedding_result_free(result);
  result->values = values; result->input_count = input_count; result->dimensions = dimensions;
  return YAP_EMBEDDING_OK;
}
