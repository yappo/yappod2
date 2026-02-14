/*
 *stdio helpers
 */

#include "yappo_io.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int __YAP_fseek_set(char *filename, int line, FILE *fp, long offset) {
  if (fp == NULL) {
    fprintf(stderr, "YAP_fseek_set: null file pointer: %s:%d\n", filename, line);
    return -1;
  }
  if (fseek(fp, offset, SEEK_SET) != 0) {
    fprintf(stderr, "YAP_fseek_set: fseek failed: %s:%d: %s\n", filename, line, strerror(errno));
    return -1;
  }
  return 0;
}

int __YAP_fseek_cur(char *filename, int line, FILE *fp, long offset) {
  if (fp == NULL) {
    fprintf(stderr, "YAP_fseek_cur: null file pointer: %s:%d\n", filename, line);
    return -1;
  }
  if (fseek(fp, offset, SEEK_CUR) != 0) {
    fprintf(stderr, "YAP_fseek_cur: fseek failed: %s:%d: %s\n", filename, line, strerror(errno));
    return -1;
  }
  return 0;
}

int __YAP_fseek_end(char *filename, int line, FILE *fp, long offset) {
  if (fp == NULL) {
    fprintf(stderr, "YAP_fseek_end: null file pointer: %s:%d\n", filename, line);
    return -1;
  }
  if (fseek(fp, offset, SEEK_END) != 0) {
    fprintf(stderr, "YAP_fseek_end: fseek failed: %s:%d: %s\n", filename, line, strerror(errno));
    return -1;
  }
  return 0;
}

int __YAP_ftell_int(char *filename, int line, FILE *fp, int *offset_out) {
  long pos;

  if (fp == NULL) {
    fprintf(stderr, "YAP_ftell_int: null file pointer: %s:%d\n", filename, line);
    return -1;
  }
  if (offset_out == NULL) {
    fprintf(stderr, "YAP_ftell_int: null output pointer: %s:%d\n", filename, line);
    return -1;
  }

  errno = 0;
  pos = ftell(fp);
  if (pos < 0) {
    fprintf(stderr, "YAP_ftell_int: ftell failed: %s:%d: %s\n", filename, line, strerror(errno));
    return -1;
  }
  if (pos > INT_MAX) {
    fprintf(stderr, "YAP_ftell_int: offset overflow: %s:%d: pos=%ld\n", filename, line, pos);
    return -1;
  }

  *offset_out = (int)pos;
  return 0;
}

int __YAP_fread_exact(char *filename, int line, FILE *fp, void *ptr, size_t size, size_t nmemb) {
  size_t got;

  if (fp == NULL) {
    fprintf(stderr, "YAP_fread_exact: null file pointer: %s:%d\n", filename, line);
    return -1;
  }

  got = fread(ptr, size, nmemb, fp);
  if (got != nmemb) {
    if (ferror(fp)) {
      fprintf(stderr, "YAP_fread_exact: fread failed: %s:%d: %s\n", filename, line,
              strerror(errno));
    } else {
      fprintf(stderr, "YAP_fread_exact: short read: %s:%d: expected=%lu got=%lu\n", filename, line,
              (unsigned long)nmemb, (unsigned long)got);
    }
    return -1;
  }
  return 0;
}

size_t __YAP_fread_try(char *filename, int line, FILE *fp, void *ptr, size_t size, size_t nmemb) {
  size_t got;

  if (fp == NULL) {
    fprintf(stderr, "YAP_fread_try: null file pointer: %s:%d\n", filename, line);
    return 0;
  }

  got = fread(ptr, size, nmemb, fp);
  if (ferror(fp)) {
    fprintf(stderr, "YAP_fread_try: fread failed: %s:%d: %s\n", filename, line, strerror(errno));
    return 0;
  }

  return got;
}

int __YAP_fwrite_exact(char *filename, int line, FILE *fp, const void *ptr, size_t size,
                       size_t nmemb) {
  size_t put;

  if (fp == NULL) {
    fprintf(stderr, "YAP_fwrite_exact: null file pointer: %s:%d\n", filename, line);
    return -1;
  }

  put = fwrite(ptr, size, nmemb, fp);
  if (put != nmemb) {
    fprintf(stderr, "YAP_fwrite_exact: fwrite failed: %s:%d: %s\n", filename, line,
            strerror(errno));
    return -1;
  }
  return 0;
}
