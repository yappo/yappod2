#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yappo_db.h"
#include "yappo_index_pos.h"
#include "yappo_limits.h"

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
  close_if_open(&db->pos_size_file);
  close_if_open(&db->pos_index_file);
  close_if_open(&db->pos_file);
}

static int setup_db(YAPPO_DB_FILES *db, int pos_size, int pos_index, const unsigned char *payload,
                    int payload_len) {
  int zero = 0;

  memset(db, 0, sizeof(*db));
  db->pos_num = 1;

  db->pos_size_file = tmpfile();
  db->pos_index_file = tmpfile();
  db->pos_file = tmpfile();
  if (db->pos_size_file == NULL || db->pos_index_file == NULL || db->pos_file == NULL) {
    cleanup_db(db);
    return -1;
  }

  if (write_exact(db->pos_size_file, &zero, sizeof(zero)) != 0 ||
      write_exact(db->pos_size_file, &pos_size, sizeof(pos_size)) != 0 ||
      write_exact(db->pos_index_file, &zero, sizeof(zero)) != 0 ||
      write_exact(db->pos_index_file, &pos_index, sizeof(pos_index)) != 0 ||
      (payload_len > 0 && write_exact(db->pos_file, payload, (size_t)payload_len) != 0) ||
      fseek(db->pos_size_file, 0L, SEEK_SET) != 0 || fseek(db->pos_index_file, 0L, SEEK_SET) != 0 ||
      fseek(db->pos_file, 0L, SEEK_SET) != 0) {
    cleanup_db(db);
    return -1;
  }

  return 0;
}

static int expect_get_failed(int pos_size, int pos_index, const unsigned char *payload,
                             int payload_len) {
  YAPPO_DB_FILES db;
  unsigned char *postings = NULL;
  int postings_len = 0;
  int rc;

  if (setup_db(&db, pos_size, pos_index, payload, payload_len) != 0) {
    return fail("failed to setup test db");
  }

  rc = YAP_Index_Pos_get(&db, 1, &postings, &postings_len);
  free(postings);
  cleanup_db(&db);
  if (rc == 0) {
    return fail("expected YAP_Index_Pos_get failure");
  }
  return 0;
}

static int test_negative_size(void) {
  unsigned char payload[1] = {0x01};
  return expect_get_failed(-1, 0, payload, (int)sizeof(payload));
}

static int test_truncated_postings_data(void) {
  unsigned char payload[1] = {0x01};
  return expect_get_failed(4, 0, payload, (int)sizeof(payload));
}

static int test_negative_index(void) {
  unsigned char payload[4] = {0x01, 0x02, 0x03, 0x04};
  return expect_get_failed(4, -1, payload, (int)sizeof(payload));
}

static int test_oversized_postings_size(void) {
  unsigned char payload[1] = {0x00};
  return expect_get_failed(YAP_MAX_POSTINGS_BLOB_SIZE + 1, 0, payload, 0);
}

static int test_valid_postings(void) {
  YAPPO_DB_FILES db;
  unsigned char payload[4] = {0x10, 0x20, 0x30, 0x40};
  unsigned char *postings = NULL;
  int postings_len = 0;
  int rc;

  if (setup_db(&db, (int)sizeof(payload), 0, payload, (int)sizeof(payload)) != 0) {
    return fail("failed to setup valid db");
  }

  rc = YAP_Index_Pos_get(&db, 1, &postings, &postings_len);
  if (rc != 0) {
    cleanup_db(&db);
    return fail("valid postings should be decoded");
  }

  if (postings_len != (int)sizeof(payload) ||
      memcmp(postings, payload, (size_t)postings_len) != 0) {
    free(postings);
    cleanup_db(&db);
    return fail("decoded postings mismatch");
  }

  free(postings);
  cleanup_db(&db);
  return 0;
}

int main(void) {
  if (test_negative_size() != 0) {
    return 1;
  }
  if (test_truncated_postings_data() != 0) {
    return 1;
  }
  if (test_negative_index() != 0) {
    return 1;
  }
  if (test_oversized_postings_size() != 0) {
    return 1;
  }
  if (test_valid_postings() != 0) {
    return 1;
  }
  return 0;
}
