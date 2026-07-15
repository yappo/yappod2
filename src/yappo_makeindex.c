#include "yappo_build_v2.h"
#include "yappo_ingest.h"
#include "yappo_http_v2.h"
#include "yappo_update_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *output) {
  fputs("Usage:\n"
        "  yappo_makeindex prepare --config CONFIG --input INPUT --output OUTPUT "
        "[--input-format ndjson|tsv]\n"
        "  yappo_makeindex build --config CONFIG --input documents.ndjson "
        "--index INDEX_DIR\n"
        "  yappo_makeindex update --input operations.ndjson --index INDEX_DIR\n"
        "  yappo_makeindex verify --index INDEX_DIR\n",
        output);
}

static int verify_main(int argc, char **argv) {
  const char *index_dir = NULL;
  YAP_V2_HTTP_RUNTIME runtime;
  YAP_V2_OPERATIONAL_STATE state;
  int i, status;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) index_dir = argv[++i];
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      fputs("Usage: yappo_makeindex verify --index INDEX_DIR\n", stdout);
      return EXIT_SUCCESS;
    } else {
      fprintf(stderr, "Unknown verify option: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }
  if (index_dir == NULL) {
    fputs("verify requires --index INDEX_DIR\n", stderr);
    return EXIT_FAILURE;
  }
  YAP_V2_http_runtime_init(&runtime);
  status = YAP_V2_http_runtime_open(&runtime, index_dir);
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
