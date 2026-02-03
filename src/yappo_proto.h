/*
 *protocol I/O helpers between front and core
 */
#ifndef YAPPO_PROTO_H
#define YAPPO_PROTO_H

#include <stdio.h>
#include "yappo_search.h"

int YAP_Proto_send_query(FILE *socket, const char *dict, int max_size, const char *op,
                         const char *keyword);
int YAP_Proto_recv_query(FILE *socket, int max_payload, char **dict, int *max_size, char **op,
                         char **keyword);
int YAP_Proto_send_shutdown(FILE *socket);

int YAP_Proto_send_result(FILE *socket, const SEARCH_RESULT *result);
SEARCH_RESULT *YAP_Proto_recv_result(FILE *socket);

#endif
