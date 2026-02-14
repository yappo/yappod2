#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_fs.h"

/* mkdtemp prototype can be hidden under strict feature levels on some toolchains. */
char *mkdtemp(char *template);

#ifndef YTEST_ROOT_DIR
#define YTEST_ROOT_DIR "."
#endif

#ifndef YTEST_BUILD_DIR
#define YTEST_BUILD_DIR "./build"
#endif

int ytest_env_init(ytest_env_t *env) {
  char tmp_base[PATH_MAX];
  char tmpl[PATH_MAX];
  int n;

  if (env == NULL) {
    errno = EINVAL;
    return -1;
  }

  memset(env, 0, sizeof(*env));
  if (snprintf(env->root_dir, sizeof(env->root_dir), "%s", YTEST_ROOT_DIR) >=
      (int)sizeof(env->root_dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (snprintf(env->build_dir, sizeof(env->build_dir), "%s", YTEST_BUILD_DIR) >=
      (int)sizeof(env->build_dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (snprintf(env->fixture_path, sizeof(env->fixture_path), "%s/tests/fixtures/index.txt",
               env->root_dir) >= (int)sizeof(env->fixture_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (snprintf(env->fixture_malformed_path, sizeof(env->fixture_malformed_path),
               "%s/tests/fixtures/index_malformed.txt", env->root_dir) >=
      (int)sizeof(env->fixture_malformed_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

#if defined(__APPLE__) && defined(_CS_DARWIN_USER_TEMP_DIR)
  {
    size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, tmp_base, sizeof(tmp_base));
    if (len == 0 || len >= sizeof(tmp_base)) {
      errno = ENOENT;
      return -1;
    }
  }
#elif defined(P_tmpdir)
  n = snprintf(tmp_base, sizeof(tmp_base), "%s", P_tmpdir);
  if (n < 0 || n >= (int)sizeof(tmp_base)) {
    errno = ENAMETOOLONG;
    return -1;
  }
#else
  errno = ENOENT;
  return -1;
#endif

  if (tmp_base[strlen(tmp_base) - 1] == '/') {
    n = snprintf(tmpl, sizeof(tmpl), "%syappod2-test-XXXXXX", tmp_base);
  } else {
    n = snprintf(tmpl, sizeof(tmpl), "%s/yappod2-test-XXXXXX", tmp_base);
  }
  if (n < 0 || n >= (int)sizeof(tmpl)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  if (mkdtemp(tmpl) == NULL) {
    return -1;
  }

  if (snprintf(env->tmp_root, sizeof(env->tmp_root), "%s", tmpl) >=
      (int)sizeof(env->tmp_root)) {
    ytest_rm_rf(tmpl);
    errno = ENAMETOOLONG;
    return -1;
  }

  return 0;
}

void ytest_env_destroy(ytest_env_t *env) {
  if (env == NULL) {
    return;
  }
  if (env->tmp_root[0] != '\0') {
    ytest_rm_rf(env->tmp_root);
  }
  memset(env, 0, sizeof(*env));
}
