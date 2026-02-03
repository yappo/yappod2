/*
 *socket/file-stream helpers
 */
#ifndef __YAPPO_NET_H__
#define __YAPPO_NET_H__

#include <stdio.h>
#include <sys/socket.h>

int YAP_Net_accept_stream(int listen_socket, struct sockaddr *addr, socklen_t *addrlen,
                          FILE **stream_out, int *fd_out, const char *label, int thread_id);
void YAP_Net_close_stream(FILE **stream_io, int *fd_io);
int YAP_Net_write_all(int fd, const void *buf, size_t len, const char *label, int thread_id);

#endif
