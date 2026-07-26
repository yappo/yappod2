#include "test_fs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int ytest_path_join(char *out, size_t out_size, const char *lhs, const char *rhs) {
  int n;

  if (out == NULL || out_size == 0 || lhs == NULL || rhs == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (lhs[0] == '\0') {
    n = snprintf(out, out_size, "%s", rhs);
  } else if (rhs[0] == '\0') {
    n = snprintf(out, out_size, "%s", lhs);
  } else if (lhs[strlen(lhs) - 1] == '/') {
    n = snprintf(out, out_size, "%s%s", lhs, rhs);
  } else {
    n = snprintf(out, out_size, "%s/%s", lhs, rhs);
  }

  if (n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int ytest_mkdir_p(const char *path, mode_t mode) {
  char buf[PATH_MAX];
  size_t len;
  size_t i;

  if (path == NULL || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  len = strlen(path);
  if (len >= sizeof(buf)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  memcpy(buf, path, len + 1);
  if (len > 1 && buf[len - 1] == '/') {
    buf[len - 1] = '\0';
    len--;
  }

  for (i = 1; i < len; i++) {
    if (buf[i] != '/') {
      continue;
    }
    buf[i] = '\0';
    if (mkdir(buf, mode) != 0 && errno != EEXIST) {
      return -1;
    }
    buf[i] = '/';
  }

  if (mkdir(buf, mode) != 0 && errno != EEXIST) {
    return -1;
  }

  return 0;
}

static int rm_rf_impl(const char *path) {
  struct stat st;

  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) {
      return 0;
    }
    return -1;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir = NULL;
    struct dirent *ent = NULL;

    dir = opendir(path);
    if (dir == NULL) {
      return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
      char child[PATH_MAX];

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      if (ytest_path_join(child, sizeof(child), path, ent->d_name) != 0) {
        closedir(dir);
        return -1;
      }
      if (rm_rf_impl(child) != 0) {
        int saved = errno;
        closedir(dir);
        errno = saved;
        return -1;
      }
    }
    closedir(dir);
    return rmdir(path);
  }

  return unlink(path);
}

int ytest_rm_rf(const char *path) {
  if (path == NULL || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  return rm_rf_impl(path);
}

int ytest_truncate_file(const char *path, off_t size) {
  if (path == NULL || size < 0) {
    errno = EINVAL;
    return -1;
  }
  return truncate(path, size);
}

int ytest_read_file(const char *path, char **data, size_t *size_out) {
  FILE *fp = NULL;
  long sz;
  char *buf = NULL;

  if (path == NULL || data == NULL || size_out == NULL) {
    errno = EINVAL;
    return -1;
  }

  *data = NULL;
  *size_out = 0;

  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }

  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  sz = ftell(fp);
  if (sz < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }

  buf = (char *)malloc((size_t)sz + 1U);
  if (buf == NULL) {
    fclose(fp);
    return -1;
  }

  if (sz > 0 && fread(buf, 1U, (size_t)sz, fp) != (size_t)sz) {
    int saved = errno;
    free(buf);
    fclose(fp);
    errno = (saved != 0) ? saved : EIO;
    return -1;
  }
  buf[(size_t)sz] = '\0';

  fclose(fp);
  *data = buf;
  *size_out = (size_t)sz;
  return 0;
}

int ytest_write_file(const char *path, const void *data, size_t size) {
  FILE *fp = NULL;

  if (path == NULL || (data == NULL && size > 0U)) {
    errno = EINVAL;
    return -1;
  }

  fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }

  if (size > 0U && fwrite(data, 1U, size, fp) != size) {
    int saved = errno;
    fclose(fp);
    errno = (saved != 0) ? saved : EIO;
    return -1;
  }

  if (fclose(fp) != 0) {
    return -1;
  }
  return 0;
}
