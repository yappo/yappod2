#ifndef YAPPO_WRITER_LOCK_V2_H
#define YAPPO_WRITER_LOCK_V2_H

typedef struct {
  int fd;
} YAP_V2_WRITER_LOCK;

void YAP_V2_writer_lock_init(YAP_V2_WRITER_LOCK *lock);
int YAP_V2_writer_lock_acquire(YAP_V2_WRITER_LOCK *lock, const char *index_dir);
void YAP_V2_writer_lock_release(YAP_V2_WRITER_LOCK *lock);

#endif
