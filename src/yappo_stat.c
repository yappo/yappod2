/*
 *stat helpers
 */

#include "yappo_stat.h"

#include <errno.h>
#include <stddef.h>

int __YAP_stat(char *filename, int line, const char *path, struct stat *st) {
  (void)filename;
  (void)line;

  if (path == NULL || st == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (stat(path, st) != 0) {
    return -1;
  }
  return 0;
}

int __YAP_is_reg(char *filename, int line, const char *path) {
  struct stat st;
  if (__YAP_stat(filename, line, path, &st) != 0) {
    return 0;
  }
  return S_ISREG(st.st_mode) ? 1 : 0;
}

int __YAP_is_dir(char *filename, int line, const char *path) {
  struct stat st;
  if (__YAP_stat(filename, line, path, &st) != 0) {
    return 0;
  }
  return S_ISDIR(st.st_mode) ? 1 : 0;
}
