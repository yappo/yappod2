#include "test_mutation.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_fs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static size_t encode_varint_pair(unsigned char *buf, int a, int b) {
  int vals[2];
  size_t out = 0U;
  size_t i;

  vals[0] = a;
  vals[1] = b;
  for (i = 0; i < 2U; i++) {
    unsigned int v = (unsigned int)vals[i];
    for (;;) {
      unsigned char b8 = (unsigned char)(v & 0x7FU);
      v >>= 7;
      if (v != 0U) {
        buf[out++] = (unsigned char)(b8 | 0x80U);
      } else {
        buf[out++] = b8;
        break;
      }
    }
  }
  return out;
}

int ytest_mutate_malformed_postings(const char *index_dir) {
  char keywordnum_path[PATH_MAX];
  char index_path[PATH_MAX];
  char size_path[PATH_MAX];
  char data_path[PATH_MAX];
  FILE *f_keyword = NULL;
  FILE *f_idx = NULL;
  FILE *f_size = NULL;
  FILE *f_data = NULL;
  int keyword_num;
  int kid;
  unsigned char enc[16];
  int enc_len;

  if (ytest_path_join(keywordnum_path, sizeof(keywordnum_path), index_dir, "keywordnum") != 0 ||
      ytest_path_join(index_path, sizeof(index_path), index_dir, "pos/0_index") != 0 ||
      ytest_path_join(size_path, sizeof(size_path), index_dir, "pos/0_size") != 0 ||
      ytest_path_join(data_path, sizeof(data_path), index_dir, "pos/0") != 0) {
    return -1;
  }

  f_keyword = fopen(keywordnum_path, "rb");
  if (f_keyword == NULL) {
    return -1;
  }
  if (fread(&keyword_num, sizeof(keyword_num), 1U, f_keyword) != 1U) {
    fclose(f_keyword);
    return -1;
  }
  fclose(f_keyword);

  if (keyword_num <= 0) {
    return 0;
  }

  f_idx = fopen(index_path, "r+b");
  f_size = fopen(size_path, "r+b");
  f_data = fopen(data_path, "r+b");
  if (f_idx == NULL || f_size == NULL || f_data == NULL) {
    if (f_idx != NULL) {
      fclose(f_idx);
    }
    if (f_size != NULL) {
      fclose(f_size);
    }
    if (f_data != NULL) {
      fclose(f_data);
    }
    return -1;
  }

  enc_len = (int)encode_varint_pair(enc, 1, 1000000);

  for (kid = 1; kid <= keyword_num; kid++) {
    int idx;

    if (fseek(f_idx, (long)(4 * kid), SEEK_SET) != 0) {
      break;
    }
    if (fread(&idx, sizeof(idx), 1U, f_idx) != 1U) {
      break;
    }
    if (idx < 0) {
      continue;
    }

    if (fseek(f_data, (long)idx, SEEK_SET) != 0 ||
        fwrite(enc, 1U, (size_t)enc_len, f_data) != (size_t)enc_len ||
        fseek(f_size, (long)(4 * kid), SEEK_SET) != 0 ||
        fwrite(&enc_len, sizeof(enc_len), 1U, f_size) != 1U) {
      fclose(f_idx);
      fclose(f_size);
      fclose(f_data);
      return -1;
    }
  }

  fclose(f_idx);
  fclose(f_size);
  fclose(f_data);
  return 0;
}

static int write_at_index(const char *path, long offset, const void *data, size_t data_len) {
  FILE *fp = fopen(path, "r+b");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, offset, SEEK_SET) != 0 || fwrite(data, 1U, data_len, fp) != data_len) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

int ytest_mutate_extreme_score_record(const char *index_dir, int fileindex) {
  char score_path[PATH_MAX];
  char size_path[PATH_MAX];
  char urllen_path[PATH_MAX];
  char filekeywordnum_path[PATH_MAX];
  double nan_value = NAN;
  int zero = 0;

  if (fileindex < 0) {
    return -1;
  }

  if (ytest_path_join(score_path, sizeof(score_path), index_dir, "score") != 0 ||
      ytest_path_join(size_path, sizeof(size_path), index_dir, "size") != 0 ||
      ytest_path_join(urllen_path, sizeof(urllen_path), index_dir, "urllen") != 0 ||
      ytest_path_join(filekeywordnum_path, sizeof(filekeywordnum_path), index_dir, "filekeywordnum") !=
        0) {
    return -1;
  }

  if (write_at_index(score_path, (long)(sizeof(double) * (size_t)fileindex), &nan_value,
                     sizeof(nan_value)) != 0 ||
      write_at_index(size_path, (long)(sizeof(int) * (size_t)fileindex), &zero, sizeof(zero)) !=
        0 ||
      write_at_index(urllen_path, (long)(sizeof(int) * (size_t)fileindex), &zero, sizeof(zero)) !=
        0 ||
      write_at_index(filekeywordnum_path, (long)(sizeof(int) * (size_t)fileindex), &zero,
                     sizeof(zero)) != 0) {
    return -1;
  }

  return 0;
}

int ytest_mutate_filedata_missing_strings(const char *index_dir, int fileindex) {
  char size_path[PATH_MAX];
  char index_path[PATH_MAX];
  char data_path[PATH_MAX];
  FILE *f_index = NULL;
  FILE *f_data = NULL;
  FILE *f_size = NULL;
  int data_index;
  int payload_len;
  unsigned char payload[128];
  unsigned char *p = payload;
  size_t zero_len = 0;
  size_t comment_len = 1;
  int one = 1;
  long long lastmod = 0;
  int zero = 0;

  if (fileindex < 0) {
    return -1;
  }

  if (ytest_path_join(size_path, sizeof(size_path), index_dir, "filedata_size") != 0 ||
      ytest_path_join(index_path, sizeof(index_path), index_dir, "filedata_index") != 0 ||
      ytest_path_join(data_path, sizeof(data_path), index_dir, "filedata") != 0) {
    return -1;
  }

  f_index = fopen(index_path, "r+b");
  f_data = fopen(data_path, "r+b");
  f_size = fopen(size_path, "r+b");
  if (f_index == NULL || f_data == NULL || f_size == NULL) {
    if (f_index != NULL) {
      fclose(f_index);
    }
    if (f_data != NULL) {
      fclose(f_data);
    }
    if (f_size != NULL) {
      fclose(f_size);
    }
    return -1;
  }

  if (fseek(f_index, (long)(4 * fileindex), SEEK_SET) != 0 ||
      fread(&data_index, sizeof(data_index), 1U, f_index) != 1U) {
    fclose(f_index);
    fclose(f_data);
    fclose(f_size);
    return -1;
  }

  memcpy(p, &zero_len, sizeof(zero_len));
  p += sizeof(zero_len);
  memcpy(p, &zero_len, sizeof(zero_len));
  p += sizeof(zero_len);
  memcpy(p, &comment_len, sizeof(comment_len));
  p += sizeof(comment_len);
  *p++ = 'x';
  memcpy(p, &one, sizeof(one));
  p += sizeof(one);
  memcpy(p, &one, sizeof(one));
  p += sizeof(one);
  memcpy(p, &lastmod, sizeof(lastmod));
  p += sizeof(lastmod);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  payload_len = (int)(p - payload);

  if (fseek(f_data, (long)data_index, SEEK_SET) != 0 ||
      fwrite(payload, 1U, (size_t)payload_len, f_data) != (size_t)payload_len ||
      fseek(f_size, (long)(4 * fileindex), SEEK_SET) != 0 ||
      fwrite(&payload_len, sizeof(payload_len), 1U, f_size) != 1U) {
    fclose(f_index);
    fclose(f_data);
    fclose(f_size);
    return -1;
  }

  fclose(f_index);
  fclose(f_data);
  fclose(f_size);
  return 0;
}
