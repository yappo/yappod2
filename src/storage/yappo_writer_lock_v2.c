#include "storage/yappo_writer_lock_v2.h"

#include "common/yappo_types_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

void YAP_V2_writer_lock_init(YAP_V2_WRITER_LOCK *lock) {
  if (lock != NULL) lock->fd = -1;
}

static int lock_acquire(YAP_V2_WRITER_LOCK *lock, const char *index_dir,
                        const char *name) {
  char path[4096]; int written;
  if (lock == NULL || lock->fd >= 0 || index_dir == NULL) return YAP_V2_INVALID_ARGUMENT;
  written = snprintf(path, sizeof(path), "%s/%s", index_dir, name);
  if (written < 0 || (size_t)written >= sizeof(path)) return YAP_V2_OUT_OF_RANGE;
  lock->fd = open(path, O_CREAT | O_RDWR, 0600);
  if (lock->fd < 0 || flock(lock->fd, LOCK_EX) != 0) {
    if (lock->fd >= 0) (void)close(lock->fd);
    lock->fd = -1; return YAP_V2_IO_ERROR;
  }
  return YAP_V2_OK;
}

int YAP_V2_writer_lock_acquire(YAP_V2_WRITER_LOCK *lock,
                               const char *index_dir) {
  return lock_acquire(lock, index_dir, "writer.lock");
}

int YAP_V2_compaction_lock_acquire(YAP_V2_WRITER_LOCK *lock,
                                   const char *index_dir) {
  return lock_acquire(lock, index_dir, "compaction.lock");
}

void YAP_V2_writer_lock_release(YAP_V2_WRITER_LOCK *lock) {
  if (lock == NULL || lock->fd < 0) return;
  (void)flock(lock->fd, LOCK_UN); (void)close(lock->fd); lock->fd = -1;
}
