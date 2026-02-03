/*
 *socket/file-stream helpers
 */
#include "yappo_net.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static void yap_net_log_errno(const char *label, int thread_id, const char *op) {
  fprintf(stderr, "ERROR: %s thread %d %s failed: %s\n", label, thread_id, op, strerror(errno));
}

int YAP_Net_accept_stream(int listen_socket, struct sockaddr *addr, socklen_t *addrlen,
                          FILE **stream_out, int *fd_out, const char *label, int thread_id) {
  int fd;
  FILE *stream;

  *stream_out = NULL;
  *fd_out = -1;

  fd = accept(listen_socket, addr, addrlen);
  if (fd == -1) {
    yap_net_log_errno(label, thread_id, "accept");
    return -1;
  }

  stream = fdopen(fd, "r+");
  if (stream == NULL) {
    yap_net_log_errno(label, thread_id, "fdopen");
    close(fd);
    return -1;
  }

  *stream_out = stream;
  *fd_out = fd;
  return 0;
}

void YAP_Net_close_stream(FILE **stream_io, int *fd_io) {
  if (stream_io != NULL && *stream_io != NULL) {
    fclose(*stream_io);
    *stream_io = NULL;
    if (fd_io != NULL) {
      *fd_io = -1;
    }
    return;
  }

  if (fd_io != NULL && *fd_io >= 0) {
    close(*fd_io);
    *fd_io = -1;
  }
}

int YAP_Net_write_all(int fd, const void *buf, size_t len, const char *label, int thread_id) {
  const char *p;
  size_t left;

  p = (const char *)buf;
  left = len;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n < 0) {
      yap_net_log_errno(label, thread_id, "write");
      return -1;
    }
    if (n == 0) {
      fprintf(stderr, "ERROR: %s thread %d write returned 0\n", label, thread_id);
      return -1;
    }
    p += n;
    left -= (size_t)n;
  }
  return 0;
}
