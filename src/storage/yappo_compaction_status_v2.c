#include "storage/yappo_compaction_status_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define YAP_V2_COMPACTION_STATUS_FILE "compaction.state"

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

const char *YAP_V2_compaction_state_name(YAP_V2_COMPACTION_STATE state) {
  switch (state) {
    case YAP_V2_COMPACTION_IDLE: return "idle";
    case YAP_V2_COMPACTION_RUNNING: return "running";
    case YAP_V2_COMPACTION_SUCCEEDED: return "succeeded";
    case YAP_V2_COMPACTION_FAILED: return "failed";
    case YAP_V2_COMPACTION_INTERRUPTED: return "interrupted";
    default: return "unknown";
  }
}

int YAP_V2_compaction_status_write(const char *index_dir, YAP_V2_COMPACTION_STATE state,
                                   uint64_t generation) {
  char path[4096], temporary[4096]; FILE *file = NULL; int fd = -1, status = YAP_V2_IO_ERROR;
  long process_id; time_t now; int written;
  if (index_dir == NULL || (state != YAP_V2_COMPACTION_RUNNING &&
      state != YAP_V2_COMPACTION_SUCCEEDED && state != YAP_V2_COMPACTION_FAILED))
    return YAP_V2_INVALID_ARGUMENT;
  if (join_path(path, sizeof(path), index_dir, YAP_V2_COMPACTION_STATUS_FILE) != 0)
    return YAP_V2_OUT_OF_RANGE;
  written = snprintf(temporary, sizeof(temporary), "%s/.compaction-state-XXXXXX", index_dir);
  if (written < 0 || (size_t)written >= sizeof(temporary)) return YAP_V2_OUT_OF_RANGE;
  fd = mkstemp(temporary); if (fd < 0) return YAP_V2_IO_ERROR;
  if (fchmod(fd, 0600) != 0 || (file = fdopen(fd, "wb")) == NULL) goto done;
  fd = -1; process_id = state == YAP_V2_COMPACTION_RUNNING ? (long)getpid() : 0L;
  now = time(NULL); if (now < 0 ||
      fprintf(file, "YAP2-COMPACTION\t%s\t%ld\t%llu\t%lld\n",
              YAP_V2_compaction_state_name(state), process_id,
              (unsigned long long)generation, (long long)now) < 0 ||
      fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
    file = NULL; goto done;
  }
  file = NULL;
  if (rename(temporary, path) != 0) goto done;
  {
    int directory = open(index_dir, O_RDONLY);
    if (directory < 0) goto done;
    if (fsync(directory) != 0) { (void)close(directory); goto done; }
    if (close(directory) != 0) goto done;
  }
  status = YAP_V2_OK;
done:
  if (file != NULL) (void)fclose(file); else if (fd >= 0) (void)close(fd);
  if (status != YAP_V2_OK) (void)unlink(temporary);
  return status;
}
