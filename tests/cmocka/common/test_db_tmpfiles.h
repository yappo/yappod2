#ifndef YTEST_DB_TMPFILES_H
#define YTEST_DB_TMPFILES_H

#include <stdio.h>

#include "yappo_db.h"

int ytest_write_exact(FILE *fp, const void *buf, size_t len);
void ytest_close_if_open(FILE **fp);

int ytest_setup_filedata_db(YAPPO_DB_FILES *db, const unsigned char *payload, int payload_len);
void ytest_cleanup_filedata_db(YAPPO_DB_FILES *db);

int ytest_setup_pos_db(YAPPO_DB_FILES *db, int pos_size, int pos_index,
                       const unsigned char *payload, int payload_len);
void ytest_cleanup_pos_db(YAPPO_DB_FILES *db);

#endif
