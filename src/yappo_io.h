/*
 *stdio helpers
 */
#ifndef __YAPPO_IO_H__
#define __YAPPO_IO_H__

#include <stdio.h>
#include <stddef.h>

int __YAP_fseek_set(char *filename, int line, FILE *fp, long offset);
#define YAP_fseek_set(fp, offset) (__YAP_fseek_set(__FILE__, __LINE__, fp, offset))

int __YAP_fseek_cur(char *filename, int line, FILE *fp, long offset);
#define YAP_fseek_cur(fp, offset) (__YAP_fseek_cur(__FILE__, __LINE__, fp, offset))

int __YAP_fseek_end(char *filename, int line, FILE *fp, long offset);
#define YAP_fseek_end(fp, offset) (__YAP_fseek_end(__FILE__, __LINE__, fp, offset))

int __YAP_fread_exact(char *filename, int line, FILE *fp, void *ptr, size_t size, size_t nmemb);
#define YAP_fread_exact(fp, ptr, size, nmemb) (__YAP_fread_exact(__FILE__, __LINE__, fp, ptr, size, nmemb))

int __YAP_fwrite_exact(char *filename, int line, FILE *fp, const void *ptr, size_t size, size_t nmemb);
#define YAP_fwrite_exact(fp, ptr, size, nmemb) (__YAP_fwrite_exact(__FILE__, __LINE__, fp, ptr, size, nmemb))

#endif
