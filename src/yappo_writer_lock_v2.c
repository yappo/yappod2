#include "yappo_writer_lock_v2.h"

#include "yappo_index_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

void YAP_V2_writer_lock_init(YAP_V2_WRITER_LOCK *lock) {
  if (lock != NULL) lock->fd = -1;
}

int YAP_V2_writer_lock_acquire(YAP_V2_WRITER_LOCK *lock, const char *index_dir) {
  char path[4096]; int written;
  if (lock == NULL || lock->fd >= 0 || index_dir == NULL) return YAP_V2_INVALID_ARGUMENT;
  written = snprintf(path, sizeof(path), "%s/writer.lock", index_dir);
  if (written < 0 || (size_t)written >= sizeof(path)) return YAP_V2_OUT_OF_RANGE;
  lock->fd = open(path, O_CREAT | O_RDWR, 0600);
  if (lock->fd < 0 || flock(lock->fd, LOCK_EX) != 0) {
    if (lock->fd >= 0) (void)close(lock->fd);
    lock->fd = -1; return YAP_V2_IO_ERROR;
  }
  return YAP_V2_OK;
}

void YAP_V2_writer_lock_release(YAP_V2_WRITER_LOCK *lock) {
  if (lock == NULL || lock->fd < 0) return;
  (void)flock(lock->fd, LOCK_UN); (void)close(lock->fd); lock->fd = -1;
}
