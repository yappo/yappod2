#include "test_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_localhost(int port) {
  int fd;
  struct sockaddr_in addr;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    close(fd);
    errno = EINVAL;
    return -1;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int append_buf(unsigned char **response, size_t *response_len, const unsigned char *buf,
                      size_t len) {
  unsigned char *p;

  p = (unsigned char *)realloc(*response, *response_len + len + 1U);
  if (p == NULL) {
    return -1;
  }
  *response = p;
  if (len > 0U) {
    memcpy((*response) + *response_len, buf, len);
  }
  *response_len += len;
  (*response)[*response_len] = '\0';
  return 0;
}

int ytest_http_send_bytes(int port, const unsigned char *request, size_t request_len,
                          int shutdown_write, unsigned char **response, size_t *response_len) {
  int fd = -1;
  unsigned char buf[4096];

  if (request == NULL || response == NULL || response_len == NULL) {
    errno = EINVAL;
    return -1;
  }

  *response = NULL;
  *response_len = 0U;

  fd = connect_localhost(port);
  if (fd < 0) {
    return -1;
  }

  if (request_len > 0U) {
    size_t off = 0U;
    while (off < request_len) {
      ssize_t w = send(fd, request + off, request_len - off, 0);
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        close(fd);
        return -1;
      }
      off += (size_t)w;
    }
  }

  if (shutdown_write) {
    shutdown(fd, SHUT_WR);
  }

  for (;;) {
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    if (r == 0) {
      break;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(*response);
      *response = NULL;
      *response_len = 0U;
      close(fd);
      return -1;
    }
    if (append_buf(response, response_len, buf, (size_t)r) != 0) {
      free(*response);
      *response = NULL;
      *response_len = 0U;
      close(fd);
      return -1;
    }
  }

  close(fd);
  return 0;
}

int ytest_http_send_text(int port, const char *request, char **response_text) {
  unsigned char *response = NULL;
  size_t response_len = 0U;
  int rc;

  if (request == NULL || response_text == NULL) {
    errno = EINVAL;
    return -1;
  }

  *response_text = NULL;
  rc = ytest_http_send_bytes(port, (const unsigned char *)request, strlen(request), 1, &response,
                             &response_len);
  if (rc != 0) {
    return -1;
  }

  *response_text = (char *)response;
  (void)response_len;
  return 0;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

int ytest_http_send_hex(int port, const char *hex_request, unsigned char **response,
                        size_t *response_len) {
  size_t hex_len;
  size_t req_len;
  unsigned char *req;
  size_t i;
  int rc;

  if (hex_request == NULL || response == NULL || response_len == NULL) {
    errno = EINVAL;
    return -1;
  }

  hex_len = strlen(hex_request);
  if ((hex_len % 2U) != 0U) {
    errno = EINVAL;
    return -1;
  }

  req_len = hex_len / 2U;
  req = (unsigned char *)malloc(req_len > 0U ? req_len : 1U);
  if (req == NULL) {
    return -1;
  }

  for (i = 0; i < req_len; i++) {
    int hi = hex_nibble(hex_request[2U * i]);
    int lo = hex_nibble(hex_request[2U * i + 1U]);
    if (hi < 0 || lo < 0) {
      free(req);
      errno = EINVAL;
      return -1;
    }
    req[i] = (unsigned char)((hi << 4) | lo);
  }

  rc = ytest_http_send_bytes(port, req, req_len, 0, response, response_len);
  free(req);
  return rc;
}

int ytest_http_text_contains(int port, const char *request, const char *needle) {
  char *response_text = NULL;
  int ok;

  if (request == NULL || needle == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (ytest_http_send_text(port, request, &response_text) != 0) {
    return -1;
  }

  ok = (response_text != NULL && strstr(response_text, needle) != NULL);
  free(response_text);
  if (!ok) {
    errno = ENOENT;
    return -1;
  }
  return 0;
}

char *ytest_http_build_request_with_repeat(const char *prefix, char repeat_char, size_t repeat_len,
                                           const char *suffix) {
  size_t prefix_len;
  size_t suffix_len;
  char *buf;

  if (prefix == NULL || suffix == NULL) {
    errno = EINVAL;
    return NULL;
  }

  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  buf = (char *)malloc(prefix_len + repeat_len + suffix_len + 1U);
  if (buf == NULL) {
    return NULL;
  }

  memcpy(buf, prefix, prefix_len);
  if (repeat_len > 0U) {
    memset(buf + prefix_len, (unsigned char)repeat_char, repeat_len);
  }
  memcpy(buf + prefix_len + repeat_len, suffix, suffix_len);
  buf[prefix_len + repeat_len + suffix_len] = '\0';

  return buf;
}
