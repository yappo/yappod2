#include "test_db_tmpfiles.h"

#include <string.h>

int ytest_write_exact(FILE *fp, const void *buf, size_t len) {
  if (fp == NULL || (buf == NULL && len > 0U)) {
    return -1;
  }
  return fwrite(buf, 1U, len, fp) == len ? 0 : -1;
}

void ytest_close_if_open(FILE **fp) {
  if (fp != NULL && *fp != NULL) {
    fclose(*fp);
    *fp = NULL;
  }
}

void ytest_cleanup_filedata_db(YAPPO_DB_FILES *db) {
  if (db == NULL) {
    return;
  }
  ytest_close_if_open(&db->filedata_size_file);
  ytest_close_if_open(&db->filedata_index_file);
  ytest_close_if_open(&db->filedata_file);
}

int ytest_setup_filedata_db(YAPPO_DB_FILES *db, const unsigned char *payload, int payload_len) {
  int index = 0;

  if (db == NULL || payload_len < 0) {
    return -1;
  }

  memset(db, 0, sizeof(*db));
  db->total_filenum = 0;

  db->filedata_size_file = tmpfile();
  db->filedata_index_file = tmpfile();
  db->filedata_file = tmpfile();
  if (db->filedata_size_file == NULL || db->filedata_index_file == NULL || db->filedata_file == NULL) {
    ytest_cleanup_filedata_db(db);
    return -1;
  }

  if (ytest_write_exact(db->filedata_size_file, &payload_len, sizeof(payload_len)) != 0 ||
      ytest_write_exact(db->filedata_index_file, &index, sizeof(index)) != 0 ||
      (payload_len > 0 && payload != NULL &&
       ytest_write_exact(db->filedata_file, payload, (size_t)payload_len) != 0) ||
      fseek(db->filedata_size_file, 0L, SEEK_SET) != 0 ||
      fseek(db->filedata_index_file, 0L, SEEK_SET) != 0 ||
      fseek(db->filedata_file, 0L, SEEK_SET) != 0) {
    ytest_cleanup_filedata_db(db);
    return -1;
  }

  return 0;
}

void ytest_cleanup_pos_db(YAPPO_DB_FILES *db) {
  if (db == NULL) {
    return;
  }
  ytest_close_if_open(&db->pos_size_file);
  ytest_close_if_open(&db->pos_index_file);
  ytest_close_if_open(&db->pos_file);
}

int ytest_setup_pos_db(YAPPO_DB_FILES *db, int pos_size, int pos_index,
                       const unsigned char *payload, int payload_len) {
  int zero = 0;

  if (db == NULL || payload_len < 0) {
    return -1;
  }

  memset(db, 0, sizeof(*db));
  db->pos_num = 1;

  db->pos_size_file = tmpfile();
  db->pos_index_file = tmpfile();
  db->pos_file = tmpfile();
  if (db->pos_size_file == NULL || db->pos_index_file == NULL || db->pos_file == NULL) {
    ytest_cleanup_pos_db(db);
    return -1;
  }

  if (ytest_write_exact(db->pos_size_file, &zero, sizeof(zero)) != 0 ||
      ytest_write_exact(db->pos_size_file, &pos_size, sizeof(pos_size)) != 0 ||
      ytest_write_exact(db->pos_index_file, &zero, sizeof(zero)) != 0 ||
      ytest_write_exact(db->pos_index_file, &pos_index, sizeof(pos_index)) != 0 ||
      (payload_len > 0 && payload != NULL &&
       ytest_write_exact(db->pos_file, payload, (size_t)payload_len) != 0) ||
      fseek(db->pos_size_file, 0L, SEEK_SET) != 0 ||
      fseek(db->pos_index_file, 0L, SEEK_SET) != 0 ||
      fseek(db->pos_file, 0L, SEEK_SET) != 0) {
    ytest_cleanup_pos_db(db);
    return -1;
  }

  return 0;
}
