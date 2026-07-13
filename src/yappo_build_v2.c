#include "yappo_build_v2.h"

#include "yappo_config_v2.h"
#include "yappo_ingest.h"
#include "yappo_manifest_v2.h"
#include "yappo_update_v2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int copy_file(const char *source, const char *destination) {
  unsigned char buffer[65536];
  int input = -1, output = -1, status = -1;
  input = open(source, O_RDONLY);
  if (input < 0) return -1;
  output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (output < 0) goto done;
  for (;;) {
    ssize_t count = read(input, buffer, sizeof(buffer));
    size_t offset = 0U;
    if (count < 0) { if (errno == EINTR) continue; goto done; }
    if (count == 0) break;
    while (offset < (size_t)count) {
      ssize_t written = write(output, buffer + offset, (size_t)count - offset);
      if (written < 0) { if (errno == EINTR) continue; goto done; }
      if (written == 0) goto done;
      offset += (size_t)written;
    }
  }
  if (fsync(output) != 0) goto done;
  status = 0;
done:
  if (input >= 0 && close(input) != 0) status = -1;
  if (output >= 0 && close(output) != 0) status = -1;
  return status;
}

static int remove_tree(const char *path) {
  struct stat info;
  DIR *directory;
  struct dirent *entry;
  int status = 0;
  if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) return unlink(path);
  directory = opendir(path);
  if (directory == NULL) return -1;
  while ((entry = readdir(directory)) != NULL) {
    char child[4096];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (join_path(child, sizeof(child), path, entry->d_name) != 0 || remove_tree(child) != 0) {
      status = -1;
      break;
    }
  }
  if (closedir(directory) != 0) status = -1;
  if (status == 0 && rmdir(path) != 0) status = -1;
  return status;
}

static int sync_parent(const char *path) {
  char *copy = strdup(path), *slash;
  int descriptor, status;
  if (copy == NULL) return -1;
  slash = strrchr(copy, '/');
  if (slash == NULL) (void)strcpy(copy, ".");
  else if (slash == copy) slash[1] = '\0';
  else *slash = '\0';
  descriptor = open(copy, O_RDONLY);
  free(copy);
  if (descriptor < 0) return -1;
  status = fsync(descriptor);
  if (close(descriptor) != 0) status = -1;
  return status;
}

static void free_operations(YAP_V2_INGEST_OPERATION *operations, size_t count) {
  size_t i;
  for (i = 0U; i < count; i++) YAP_V2_ingest_operation_free(&operations[i]);
  free(operations);
}

static int publish_batch(const char *index_dir, YAP_V2_INGEST_OPERATION *operations,
                         size_t count, uint64_t *generation, size_t *accepted,
                         char *error, size_t error_size) {
  YAP_V2_UPDATE_RESULT result;
  int status = YAP_V2_build_apply(index_dir, operations, count, &result, error, error_size);
  if (status == YAP_V2_OK) {
    *generation = result.generation;
    *accepted += result.accepted;
  }
  return status;
}

static int ingest_stream(const char *input_path, const char *index_dir,
                         uint64_t *generation, size_t *accepted,
                         char *error, size_t error_size) {
  FILE *input = fopen(input_path, "rb");
  YAP_V2_INGEST_OPERATION *operations;
  char *line = NULL;
  size_t capacity = 0U, count = 0U, line_number = 0U;
  int status = YAP_V2_OK;
  ssize_t length;
  if (input == NULL) {
    (void)snprintf(error, error_size, "cannot open input: %s", strerror(errno));
    return YAP_V2_IO_ERROR;
  }
  operations = calloc(YAP_V2_BUILD_BATCH_OPERATIONS, sizeof(*operations));
  if (operations == NULL) {
    (void)snprintf(error, error_size, "cannot allocate build batch");
    (void)fclose(input);
    return YAP_V2_ALLOCATION_FAILED;
  }
  while ((length = getline(&line, &capacity, input)) >= 0) {
    size_t bytes = (size_t)length;
    line_number++;
    while (bytes > 0U && (line[bytes - 1U] == '\n' || line[bytes - 1U] == '\r')) bytes--;
    if (bytes == 0U) {
      (void)snprintf(error, error_size, "empty record at line %zu", line_number);
      status = YAP_V2_INVALID_FORMAT;
      break;
    }
    status = YAP_V2_ingest_parse_ndjson(line, bytes, &operations[count], error, error_size);
    if (status != YAP_V2_OK) break;
    if (operations[count].kind != YAP_V2_INGEST_UPSERT) {
      (void)snprintf(error, error_size, "build accepts upsert records only (line %zu)",
                     line_number);
      status = YAP_V2_INVALID_FORMAT;
      break;
    }
    count++;
    if (count == YAP_V2_BUILD_BATCH_OPERATIONS) {
      status = publish_batch(index_dir, operations, count, generation, accepted,
                             error, error_size);
      free_operations(operations, count);
      operations = NULL;
      count = 0U;
      if (status != YAP_V2_OK) break;
      operations = calloc(YAP_V2_BUILD_BATCH_OPERATIONS, sizeof(*operations));
      if (operations == NULL) { status = YAP_V2_ALLOCATION_FAILED; break; }
    }
  }
  if (status == YAP_V2_OK && ferror(input)) status = YAP_V2_IO_ERROR;
  if (status == YAP_V2_OK && count != 0U)
    status = publish_batch(index_dir, operations, count, generation, accepted,
                           error, error_size);
  if (status == YAP_V2_OK && *accepted == 0U) {
    (void)snprintf(error, error_size, "build input is empty");
    status = YAP_V2_INVALID_FORMAT;
  }
  free_operations(operations, count);
  free(line);
  if (fclose(input) != 0 && status == YAP_V2_OK) status = YAP_V2_IO_ERROR;
  return status;
}

static int build_index(const char *config_path, const char *input_path,
                       const char *index_dir, uint64_t *generation, size_t *accepted,
                       char *error, size_t error_size) {
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  struct stat info;
  char *temporary;
  char copied_config[4096], manifest_path[4096], segments_path[4096];
  size_t length;
  int status = YAP_V2_IO_ERROR;
  status = YAP_V2_config_load(config_path, &config, error, error_size);
  if (status != YAP_V2_OK) return status;
  if (lstat(index_dir, &info) == 0 || errno != ENOENT) {
    (void)snprintf(error, error_size, "index path already exists");
    return YAP_V2_CONFLICT;
  }
  length = strlen(index_dir);
  if (length > SIZE_MAX - 16U) return YAP_V2_OUT_OF_RANGE;
  temporary = malloc(length + 16U);
  if (temporary == NULL) return YAP_V2_ALLOCATION_FAILED;
  (void)snprintf(temporary, length + 16U, "%s.tmp.XXXXXX", index_dir);
  if (mkdtemp(temporary) == NULL ||
      join_path(copied_config, sizeof(copied_config), temporary, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), temporary, "manifest.json") != 0 ||
      join_path(segments_path, sizeof(segments_path), temporary, "segments") != 0 ||
      copy_file(config_path, copied_config) != 0 || mkdir(segments_path, 0700) != 0) {
    (void)snprintf(error, error_size, "cannot initialize index: %s", strerror(errno));
    goto done;
  }
  YAP_V2_manifest_init(&manifest);
  manifest.generation = 1U;
  status = YAP_V2_config_fingerprint(&config, manifest.config_fingerprint);
  if (status == YAP_V2_OK) status = YAP_V2_manifest_save_atomic(manifest_path, &manifest);
  YAP_V2_manifest_free(&manifest);
  if (status == YAP_V2_OK)
    status = ingest_stream(input_path, temporary, generation, accepted, error, error_size);
  if (status == YAP_V2_OK && lstat(index_dir, &info) == 0) {
    (void)snprintf(error, error_size, "index path appeared during build");
    status = YAP_V2_CONFLICT;
  }
  else if (status == YAP_V2_OK && errno != ENOENT) status = YAP_V2_IO_ERROR;
  if (status == YAP_V2_OK && (rename(temporary, index_dir) != 0 || sync_parent(index_dir) != 0)) {
    (void)snprintf(error, error_size, "cannot publish index: %s", strerror(errno));
    status = YAP_V2_IO_ERROR;
  }
done:
  if (status != YAP_V2_OK) (void)remove_tree(temporary);
  free(temporary);
  return status;
}

int YAP_V2_build_main(int argc, char **argv) {
  const char *config_path = NULL, *input_path = NULL, *index_dir = NULL;
  uint64_t generation = 0U;
  size_t accepted = 0U;
  char error[256] = {0};
  int i, status;
  for (i = 1; i < argc; i++) {
    const char **target;
    if (strcmp(argv[i], "--config") == 0) target = &config_path;
    else if (strcmp(argv[i], "--input") == 0) target = &input_path;
    else if (strcmp(argv[i], "--index") == 0) target = &index_dir;
    else { fprintf(stderr, "Unknown build option: %s\n", argv[i]); return EXIT_FAILURE; }
    if (++i >= argc) { fputs("Missing build option value\n", stderr); return EXIT_FAILURE; }
    *target = argv[i];
  }
  if (config_path == NULL || input_path == NULL || index_dir == NULL) {
    fputs("Usage: yappo_makeindex build --config CONFIG --input documents.ndjson "
          "--index INDEX_DIR\n", stderr);
    return EXIT_FAILURE;
  }
  status = build_index(config_path, input_path, index_dir, &generation, &accepted,
                       error, sizeof(error));
  if (status != YAP_V2_OK) {
    fprintf(stderr, "Build failed: %s (%s)\n", error, YAP_V2_status_string(status));
    return EXIT_FAILURE;
  }
  printf("{\"generation\":%llu,\"accepted\":%zu}\n",
         (unsigned long long)generation, accepted);
  return EXIT_SUCCESS;
}
