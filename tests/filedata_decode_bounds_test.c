#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "yappo_db.h"
#include "yappo_index_filedata.h"

static int fail(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  return 1;
}

static int write_exact(FILE *fp, const void *buf, size_t len) {
  return fwrite(buf, 1, len, fp) == len ? 0 : -1;
}

static void close_if_open(FILE **fp) {
  if (*fp != NULL) {
    fclose(*fp);
    *fp = NULL;
  }
}

static void cleanup_db(YAPPO_DB_FILES *db) {
  close_if_open(&db->filedata_size_file);
  close_if_open(&db->filedata_index_file);
  close_if_open(&db->filedata_file);
}

static int setup_db_with_payload(YAPPO_DB_FILES *db, const unsigned char *payload, int payload_len) {
  int index = 0;

  memset(db, 0, sizeof(*db));
  db->total_filenum = 0;

  db->filedata_size_file = tmpfile();
  db->filedata_index_file = tmpfile();
  db->filedata_file = tmpfile();
  if (db->filedata_size_file == NULL || db->filedata_index_file == NULL || db->filedata_file == NULL) {
    cleanup_db(db);
    return -1;
  }

  if (write_exact(db->filedata_size_file, &payload_len, sizeof(payload_len)) != 0 ||
      write_exact(db->filedata_index_file, &index, sizeof(index)) != 0 ||
      (payload_len > 0 && write_exact(db->filedata_file, payload, (size_t)payload_len) != 0) ||
      fseek(db->filedata_size_file, 0L, SEEK_SET) != 0 ||
      fseek(db->filedata_index_file, 0L, SEEK_SET) != 0 ||
      fseek(db->filedata_file, 0L, SEEK_SET) != 0) {
    cleanup_db(db);
    return -1;
  }

  return 0;
}

static int expect_get_failed(const unsigned char *payload, int payload_len) {
  YAPPO_DB_FILES db;
  FILEDATA filedata;
  int rc;

  if (setup_db_with_payload(&db, payload, payload_len) != 0) {
    return fail("failed to setup test db");
  }
  memset(&filedata, 0, sizeof(filedata));
  rc = YAP_Index_Filedata_get(&db, 0, &filedata);
  YAP_Index_Filedata_free(&filedata);
  cleanup_db(&db);
  if (rc == 0) {
    return fail("expected decode failure");
  }
  return 0;
}

static int test_truncated_payload(void) {
  unsigned char payload[1] = {0};
  return expect_get_failed(payload, 1);
}

static int test_invalid_string_length(void) {
  unsigned char payload[sizeof(size_t)];
  size_t bad_len = 1024;
  memcpy(payload, &bad_len, sizeof(bad_len));
  return expect_get_failed(payload, (int)sizeof(payload));
}

static int test_negative_other_length(void) {
  unsigned char payload[256];
  unsigned char *p = payload;
  size_t zero = 0;
  int size = 10;
  int keyword_num = 3;
  time_t lastmod = 123;
  int domainid = 7;
  int other_len = -1;
  int payload_len;

  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &size, sizeof(size));
  p += sizeof(size);
  memcpy(p, &keyword_num, sizeof(keyword_num));
  p += sizeof(keyword_num);
  memcpy(p, &lastmod, sizeof(lastmod));
  p += sizeof(lastmod);
  memcpy(p, &domainid, sizeof(domainid));
  p += sizeof(domainid);
  memcpy(p, &other_len, sizeof(other_len));
  p += sizeof(other_len);
  payload_len = (int)(p - payload);

  return expect_get_failed(payload, payload_len);
}

static int test_valid_payload(void) {
  YAPPO_DB_FILES db;
  FILEDATA filedata;
  unsigned char payload[256];
  unsigned char *p = payload;
  size_t url_len = 1;
  size_t title_len = 1;
  size_t comment_len = 1;
  int size = 20;
  int keyword_num = 5;
  time_t lastmod = 456;
  int domainid = 8;
  int other_len = 2;
  const unsigned char other[2] = {'x', 'y'};
  int payload_len;
  int rc;

  memcpy(p, &url_len, sizeof(url_len));
  p += sizeof(url_len);
  *p++ = 'u';
  memcpy(p, &title_len, sizeof(title_len));
  p += sizeof(title_len);
  *p++ = 't';
  memcpy(p, &comment_len, sizeof(comment_len));
  p += sizeof(comment_len);
  *p++ = 'c';
  memcpy(p, &size, sizeof(size));
  p += sizeof(size);
  memcpy(p, &keyword_num, sizeof(keyword_num));
  p += sizeof(keyword_num);
  memcpy(p, &lastmod, sizeof(lastmod));
  p += sizeof(lastmod);
  memcpy(p, &domainid, sizeof(domainid));
  p += sizeof(domainid);
  memcpy(p, &other_len, sizeof(other_len));
  p += sizeof(other_len);
  memcpy(p, other, sizeof(other));
  p += sizeof(other);
  payload_len = (int)(p - payload);

  if (setup_db_with_payload(&db, payload, payload_len) != 0) {
    return fail("failed to setup valid test db");
  }

  memset(&filedata, 0, sizeof(filedata));
  rc = YAP_Index_Filedata_get(&db, 0, &filedata);
  if (rc != 0) {
    cleanup_db(&db);
    return fail("valid payload decode failed");
  }
  if (strcmp(filedata.url, "u") != 0 || strcmp(filedata.title, "t") != 0 ||
      strcmp(filedata.comment, "c") != 0 || filedata.size != size ||
      filedata.keyword_num != keyword_num || filedata.lastmod != lastmod ||
      filedata.domainid != domainid || filedata.other_len != other_len ||
      filedata.other[0] != 'x' || filedata.other[1] != 'y') {
    YAP_Index_Filedata_free(&filedata);
    cleanup_db(&db);
    return fail("decoded payload mismatch");
  }

  YAP_Index_Filedata_free(&filedata);
  cleanup_db(&db);
  return 0;
}

int main(void) {
  if (test_truncated_payload() != 0) {
    return 1;
  }
  if (test_invalid_string_length() != 0) {
    return 1;
  }
  if (test_negative_other_length() != 0) {
    return 1;
  }
  if (test_valid_payload() != 0) {
    return 1;
  }
  return 0;
}
