#ifndef YTEST_FS_H
#define YTEST_FS_H

#include <stddef.h>
#include <sys/types.h>

int ytest_path_join(char *out, size_t out_size, const char *lhs, const char *rhs);
int ytest_mkdir_p(const char *path, mode_t mode);
int ytest_rm_rf(const char *path);
int ytest_truncate_file(const char *path, off_t size);
int ytest_read_file(const char *path, char **data, size_t *size_out);
int ytest_write_file(const char *path, const void *data, size_t size);

#endif
