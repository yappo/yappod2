/*
 *stat helpers
 */
#ifndef __YAPPO_STAT_H__
#define __YAPPO_STAT_H__

#include <sys/stat.h>

int __YAP_stat(char *filename, int line, const char *path, struct stat *st);
#define YAP_stat(path, st) (__YAP_stat(__FILE__, __LINE__, path, st))

int __YAP_is_reg(char *filename, int line, const char *path);
#define YAP_is_reg(path) (__YAP_is_reg(__FILE__, __LINE__, path))

int __YAP_is_dir(char *filename, int line, const char *path);
#define YAP_is_dir(path) (__YAP_is_dir(__FILE__, __LINE__, path))

#endif
