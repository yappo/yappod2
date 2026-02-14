#ifndef YTEST_ENV_H
#define YTEST_ENV_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  char root_dir[PATH_MAX];
  char build_dir[PATH_MAX];
  char fixture_path[PATH_MAX];
  char fixture_malformed_path[PATH_MAX];
  char tmp_root[PATH_MAX];
} ytest_env_t;

int ytest_env_init(ytest_env_t *env);
void ytest_env_destroy(ytest_env_t *env);

#endif
