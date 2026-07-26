#ifndef YTEST_HTTP_H
#define YTEST_HTTP_H

#include <stddef.h>

int ytest_http_send_bytes(int port, const unsigned char *request, size_t request_len,
                          int shutdown_write, unsigned char **response, size_t *response_len);
int ytest_http_send_text(int port, const char *request, char **response_text);
int ytest_http_send_hex(int port, const char *hex_request, unsigned char **response,
                        size_t *response_len);
int ytest_http_text_contains(int port, const char *request, const char *needle);
char *ytest_http_build_request_with_repeat(const char *prefix, char repeat_char, size_t repeat_len,
                                           const char *suffix);

#endif
