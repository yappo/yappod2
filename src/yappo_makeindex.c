#include "yappo_build_v2.h"
#include "yappo_ingest.h"
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
        "  yappo_makeindex update --input operations.ndjson --index INDEX_DIR\n",
        output);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  if (strcmp(argv[1], "prepare") == 0) return YAP_V2_prepare_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "build") == 0) return YAP_V2_build_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "update") == 0) return YAP_V2_update_main(argc - 1, argv + 1);
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage(stdout);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "Unknown command: %s\n", argv[1]);
  usage(stderr);
  return EXIT_FAILURE;
}
