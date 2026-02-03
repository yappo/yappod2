/*
 *stdio helpers
 */
#ifndef __YAPPO_IO_H__
#define __YAPPO_IO_H__

#include <stdio.h>
#include <stddef.h>

/*
 * 以降の I/O ヘルパーは Doxygen 形式で記述しています。
 * C には JavaDoc そのものはありませんが、Doxygen が広く使われます。
 */

/**
 * @brief ファイル位置を先頭基準(SEEK_SET)で移動します。
 *
 * @param fp 対象ファイル
 * @param offset 先頭からのオフセット
 * @return 0:成功 / -1:失敗
 */
int __YAP_fseek_set(char *filename, int line, FILE *fp, long offset);
/** @brief 呼び出し位置情報付きの `__YAP_fseek_set` ラッパー。通常はこちらを使います。 */
#define YAP_fseek_set(fp, offset) (__YAP_fseek_set(__FILE__, __LINE__, fp, offset))

/**
 * @brief ファイル位置を現在位置基準(SEEK_CUR)で移動します。
 *
 * @param fp 対象ファイル
 * @param offset 現在位置からのオフセット
 * @return 0:成功 / -1:失敗
 */
int __YAP_fseek_cur(char *filename, int line, FILE *fp, long offset);
/** @brief 呼び出し位置情報付きの `__YAP_fseek_cur` ラッパー。通常はこちらを使います。 */
#define YAP_fseek_cur(fp, offset) (__YAP_fseek_cur(__FILE__, __LINE__, fp, offset))

/**
 * @brief ファイル位置を末尾基準(SEEK_END)で移動します。
 *
 * @param fp 対象ファイル
 * @param offset 末尾からのオフセット
 * @return 0:成功 / -1:失敗
 */
int __YAP_fseek_end(char *filename, int line, FILE *fp, long offset);
/** @brief 呼び出し位置情報付きの `__YAP_fseek_end` ラッパー。通常はこちらを使います。 */
#define YAP_fseek_end(fp, offset) (__YAP_fseek_end(__FILE__, __LINE__, fp, offset))

/**
 * @brief 指定要素数を「必ず」読み込みます。
 *
 * @details
 * `nmemb` 個未満しか読めなかった場合（EOF を含む）も失敗扱いです。
 * 「ここは必ず存在するはず」というデータに使ってください。
 *
 * @param fp 対象ファイル
 * @param ptr 読み込み先
 * @param size 要素サイズ
 * @param nmemb 要素数
 * @return 0:成功 / -1:失敗
 */
int __YAP_fread_exact(char *filename, int line, FILE *fp, void *ptr, size_t size, size_t nmemb);
/** @brief 呼び出し位置情報付きの `__YAP_fread_exact` ラッパー。通常はこちらを使います。 */
#define YAP_fread_exact(fp, ptr, size, nmemb)                                                      \
  (__YAP_fread_exact(__FILE__, __LINE__, fp, ptr, size, nmemb))

/**
 * @brief 指定要素数を上限に、読めた分だけ読み込みます。
 *
 * @details
 * EOF による短読込は正常系として扱います。
 * 「未登録なら読めなくてもよい」データに使ってください。
 *
 * @param fp 対象ファイル
 * @param ptr 読み込み先
 * @param size 要素サイズ
 * @param nmemb 最大要素数
 * @return 実際に読めた要素数（0 を含む）
 */
size_t __YAP_fread_try(char *filename, int line, FILE *fp, void *ptr, size_t size, size_t nmemb);
/** @brief 呼び出し位置情報付きの `__YAP_fread_try` ラッパー。通常はこちらを使います。 */
#define YAP_fread_try(fp, ptr, size, nmemb)                                                        \
  (__YAP_fread_try(__FILE__, __LINE__, fp, ptr, size, nmemb))

/**
 * @brief 指定要素数を「必ず」書き込みます。
 *
 * @param fp 対象ファイル
 * @param ptr 書き込み元
 * @param size 要素サイズ
 * @param nmemb 要素数
 * @return 0:成功 / -1:失敗
 */
int __YAP_fwrite_exact(char *filename, int line, FILE *fp, const void *ptr, size_t size,
                       size_t nmemb);
/** @brief 呼び出し位置情報付きの `__YAP_fwrite_exact` ラッパー。通常はこちらを使います。 */
#define YAP_fwrite_exact(fp, ptr, size, nmemb)                                                     \
  (__YAP_fwrite_exact(__FILE__, __LINE__, fp, ptr, size, nmemb))

#endif
