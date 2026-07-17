#include "yappo_build_v2.h"
#include "yappo_ingest.h"
#include "yappo_http_v2.h"
#include "yappo_update_v2.h"
#include "yappo_application_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *output) {
  fputs("Usage:\n"
        "  yappo_makeindex prepare --config CONFIG --input INPUT --output OUTPUT "
        "[--input-format ndjson|tsv]\n"
        "  yappo_makeindex build --config CONFIG --input documents.ndjson\n"
        "  yappo_makeindex update --config CONFIG --input operations.ndjson\n"
        "  yappo_makeindex verify --config CONFIG\n",
        output);
}

static int verify_main(int argc, char **argv) {
  const char *config_path = NULL, *index_option = NULL;
  YAP_APPLICATION_CONFIG application;
  YAP_V2_HTTP_RUNTIME runtime;
  YAP_V2_OPERATIONAL_STATE state;
  int i, status;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
    else if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) index_option = argv[++i];
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      fputs("Usage: yappo_makeindex verify --config CONFIG\n", stdout);
      return EXIT_SUCCESS;
    } else {
      fprintf(stderr, "Unknown verify option: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }
  if (config_path == NULL || index_option != NULL) {
    fputs("verify requires --config CONFIG\n", stderr);
    return EXIT_FAILURE;
  }
  {
    char error[256] = {0};
    if (YAP_application_config_load(config_path, &application, error, sizeof(error)) != YAP_V2_OK) {
      fprintf(stderr, "Config error: %s\n", error); return EXIT_FAILURE;
    }
  }
  YAP_V2_http_runtime_init(&runtime);
  status = YAP_V2_http_runtime_open(&runtime, application.index_directory);
  if (status != YAP_V2_OK) {
    fprintf(stderr, "verification failed: %s\n", YAP_V2_status_string(status));
    return EXIT_FAILURE;
  }
  status = YAP_V2_http_runtime_state(&runtime, &state);
  if (status == YAP_V2_OK)
    printf("verified generation=%llu segments=%zu\n",
           (unsigned long long)state.generation, state.segment_count);
  YAP_V2_http_runtime_close(&runtime);
  return status == YAP_V2_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  if (strcmp(argv[1], "prepare") == 0) return YAP_V2_prepare_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "build") == 0) return YAP_V2_build_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "update") == 0) return YAP_V2_update_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "verify") == 0) return verify_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage(stdout);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "Unknown command: %s\n", argv[1]);
  usage(stderr);
  return EXIT_FAILURE;
}
