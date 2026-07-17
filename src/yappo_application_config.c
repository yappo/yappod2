#include "yappo_application_config.h"

#include "yappo_config_v2.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <toml.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *format, ...) {
  va_list args;
  if (error == NULL || size == 0U) return;
  va_start(args, format);
  (void)vsnprintf(error, size, format, args);
  va_end(args);
}

static int key_allowed(const toml_table_t *table, const char *const *allowed) {
  int index;
  for (index = 0;; index++) {
    const char *key = toml_key_in(table, index);
    size_t i;
    int found = 0;
    if (key == NULL) break;
    for (i = 0U; allowed[i] != NULL; i++) {
      if (strcmp(key, allowed[i]) == 0) { found = 1; break; }
    }
    if (!found) return 0;
  }
  return 1;
}

static int root_key_allowed(toml_table_t *root) {
  static const char *const scalar_keys[] = {
    "schema_version", "format_version", "collection_id", NULL
  };
  int index;
  for (index = 0;; index++) {
    const char *key = toml_key_in(root, index);
    size_t i;
    int scalar = 0;
    if (key == NULL) break;
    if (toml_table_in(root, key) != NULL) continue;
    for (i = 0U; scalar_keys[i] != NULL; i++)
      if (strcmp(key, scalar_keys[i]) == 0) { scalar = 1; break; }
    if (!scalar) return 0;
  }
  return 1;
}

static int read_string(toml_table_t *table, const char *key, char *output,
                       size_t capacity, int required, char *error, size_t error_size) {
  toml_datum_t value = toml_string_in(table, key);
  size_t length;
  if (!value.ok) {
    if (required || toml_key_exists(table, key)) {
      set_error(error, error_size, "%s must be a string", key);
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  length = strlen(value.u.s);
  if (length == 0U || length >= capacity) {
    free(value.u.s);
    set_error(error, error_size, "%s is empty or too long", key);
    return YAP_V2_OUT_OF_RANGE;
  }
  memcpy(output, value.u.s, length + 1U);
  free(value.u.s);
  return YAP_V2_OK;
}

static int read_uint32(toml_table_t *table, const char *key, uint32_t *output,
                       uint32_t minimum, uint32_t maximum, int required,
                       char *error, size_t error_size) {
  toml_datum_t value = toml_int_in(table, key);
  if (!value.ok) {
    if (required || toml_key_exists(table, key)) {
      set_error(error, error_size, "%s must be an integer", key);
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  if (value.u.i < (int64_t)minimum || value.u.i > (int64_t)maximum) {
    set_error(error, error_size, "%s is out of range", key);
    return YAP_V2_OUT_OF_RANGE;
  }
  *output = (uint32_t)value.u.i;
  return YAP_V2_OK;
}

static int normalize_path(const char *base, const char *value, char *output,
                          size_t capacity, char *error, size_t error_size) {
  char joined[YAP_APPLICATION_PATH_BYTES * 2U];
  char scratch[YAP_APPLICATION_PATH_BYTES * 2U];
  char *parts[1024];
  char *cursor, *token;
  size_t count = 0U, used = 1U, i;
  int written;
  if (value[0] == '/') written = snprintf(joined, sizeof(joined), "%s", value);
  else written = snprintf(joined, sizeof(joined), "%s/%s", base, value);
  if (written < 0 || (size_t)written >= sizeof(joined)) {
    set_error(error, error_size, "resolved path is too long");
    return YAP_V2_OUT_OF_RANGE;
  }
  memcpy(scratch, joined, (size_t)written + 1U);
  cursor = scratch;
  while ((token = strsep(&cursor, "/")) != NULL) {
    if (token[0] == '\0' || strcmp(token, ".") == 0) continue;
    if (strcmp(token, "..") == 0) { if (count > 0U) count--; continue; }
    if (count == sizeof(parts) / sizeof(parts[0])) return YAP_V2_OUT_OF_RANGE;
    parts[count++] = token;
  }
  if (capacity < 2U) return YAP_V2_OUT_OF_RANGE;
  output[0] = '/'; output[1] = '\0';
  for (i = 0U; i < count; i++) {
    size_t length = strlen(parts[i]);
    if (used + length + (i + 1U < count ? 1U : 0U) >= capacity) {
      set_error(error, error_size, "resolved path is too long");
      return YAP_V2_OUT_OF_RANGE;
    }
    memcpy(output + used, parts[i], length); used += length;
    if (i + 1U < count) output[used++] = '/';
    output[used] = '\0';
  }
  return YAP_V2_OK;
}

static int read_filterable_fields(toml_table_t *metadata, YAP_V2_CONFIG *config,
                                  char *error, size_t error_size) {
  toml_array_t *array;
  int count, i;
  if (metadata == NULL) return YAP_V2_OK;
  array = toml_array_in(metadata, "filterable_fields");
  if (array == NULL) {
    if (toml_key_exists(metadata, "filterable_fields")) {
      set_error(error, error_size, "filterable_fields must be an array of strings");
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  count = toml_array_nelem(array);
  if (count < 0 || count > (int)YAP_V2_MAX_FILTER_FIELDS) return YAP_V2_OUT_OF_RANGE;
  for (i = 0; i < count; i++) {
    toml_datum_t value = toml_string_at(array, i);
    size_t length;
    if (!value.ok) return YAP_V2_INVALID_FORMAT;
    length = strlen(value.u.s);
    if (length == 0U || length > YAP_V2_MAX_FILTER_FIELD_BYTES) {
      free(value.u.s); return YAP_V2_OUT_OF_RANGE;
    }
    memcpy(config->filterable_fields[i], value.u.s, length + 1U);
    free(value.u.s);
  }
  config->filterable_field_count = (size_t)count;
  for (i = 0; i < count; i++) {
    int j;
    for (j = i + 1; j < count; j++) {
      if (strcmp(config->filterable_fields[i], config->filterable_fields[j]) > 0) {
        char swap[YAP_V2_MAX_FILTER_FIELD_BYTES + 1U];
        memcpy(swap, config->filterable_fields[i], sizeof(swap));
        memcpy(config->filterable_fields[i], config->filterable_fields[j], sizeof(swap));
        memcpy(config->filterable_fields[j], swap, sizeof(swap));
      }
    }
    if (i > 0 && strcmp(config->filterable_fields[i - 1], config->filterable_fields[i]) == 0)
      return YAP_V2_DUPLICATE;
  }
  return YAP_V2_OK;
}

void YAP_application_config_init(YAP_APPLICATION_CONFIG *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->schema_version = 1U;
  YAP_V2_config_init(&config->index_config);
  (void)strcpy(config->core_host, "127.0.0.1");
  config->core_port = 18401U;
  (void)strcpy(config->front_host, "127.0.0.1");
  config->front_port = 18400U;
  YAP_V2_runtime_policy_init(&config->runtime_policy);
}

int YAP_application_config_load(const char *path, YAP_APPLICATION_CONFIG *config,
                                char *error, size_t error_size) {
  static const char *const index_keys[] = {"directory", NULL};
  static const char *const tokenizer_keys[] = {"id", NULL};
  static const char *const chunking_keys[] = {"max_chars", "overlap_chars", NULL};
  static const char *const vector_keys[] = {"enabled", "model_id", "dimensions", "metric", NULL};
  static const char *const metadata_keys[] = {"filterable_fields", NULL};
  static const char *const daemon_keys[] = {"run_directory", "core_host", "core_port",
    "front_host", "front_port", "max_inflight", "max_inflight_bytes",
    "request_timeout_ms", "write_token", NULL};
  FILE *file;
  toml_table_t *root = NULL, *index, *tokenizer, *chunking, *vector, *metadata, *daemon;
  toml_datum_t enabled, metric, token;
  char parse_error[256] = {0}, canonical[YAP_APPLICATION_PATH_BYTES];
  char base[YAP_APPLICATION_PATH_BYTES], path_value[YAP_APPLICATION_PATH_BYTES];
  uint32_t value;
  int status = YAP_V2_INVALID_FORMAT;
  char *slash;
  if (path == NULL || config == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (error != NULL && error_size > 0U) error[0] = '\0';
  if (realpath(path, canonical) == NULL) {
    set_error(error, error_size, "cannot resolve %s: %s", path, strerror(errno));
    return YAP_V2_IO_ERROR;
  }
  file = fopen(canonical, "r");
  if (file == NULL) return YAP_V2_IO_ERROR;
  root = toml_parse_file(file, parse_error, (int)sizeof(parse_error));
  (void)fclose(file);
  if (root == NULL) { set_error(error, error_size, "invalid TOML: %s", parse_error); return YAP_V2_INVALID_FORMAT; }
  if (!root_key_allowed(root)) { set_error(error, error_size, "config contains an unknown top-level scalar or array"); goto done; }
  index = toml_table_in(root, "index"); tokenizer = toml_table_in(root, "tokenizer");
  chunking = toml_table_in(root, "chunking"); vector = toml_table_in(root, "vector");
  metadata = toml_table_in(root, "metadata"); daemon = toml_table_in(root, "daemon");
  if (index == NULL || tokenizer == NULL || chunking == NULL || vector == NULL || daemon == NULL) {
    set_error(error, error_size, "required application config table is missing"); goto done;
  }
  if (!key_allowed(index, index_keys) || !key_allowed(tokenizer, tokenizer_keys) ||
      !key_allowed(chunking, chunking_keys) || !key_allowed(vector, vector_keys) ||
      (metadata != NULL && !key_allowed(metadata, metadata_keys)) || !key_allowed(daemon, daemon_keys)) {
    set_error(error, error_size, "application config contains an unknown key"); goto done;
  }
  YAP_application_config_init(config);
  value = config->schema_version;
  status = read_uint32(root, "schema_version", &value, 1U, 1U, 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->schema_version = value;
  value = config->index_config.format_version;
  status = read_uint32(root, "format_version", &value, YAP_V2_FORMAT_VERSION,
                       YAP_V2_FORMAT_VERSION, 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->index_config.format_version = value;
  status = read_string(tokenizer, "id", config->index_config.tokenizer_id,
                       sizeof(config->index_config.tokenizer_id), 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_uint32(chunking, "max_chars", &config->index_config.chunk_max_chars,
                       1U, UINT32_MAX, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_uint32(chunking, "overlap_chars", &config->index_config.chunk_overlap_chars,
                       0U, UINT32_MAX, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  enabled = toml_bool_in(vector, "enabled");
  if (!enabled.ok) { set_error(error, error_size, "vector.enabled must be a boolean"); status = YAP_V2_INVALID_FORMAT; goto done; }
  if (enabled.u.b) {
    status = read_string(vector, "model_id", config->index_config.vector_model_id,
                         sizeof(config->index_config.vector_model_id), 1, error, error_size);
    if (status != YAP_V2_OK) goto done;
    status = read_uint32(vector, "dimensions", &config->index_config.vector_dimensions,
                         1U, UINT32_MAX, 1, error, error_size);
    if (status != YAP_V2_OK) goto done;
    metric = toml_string_in(vector, "metric");
    if (!metric.ok) { set_error(error, error_size, "vector.metric must be a string"); status = YAP_V2_INVALID_FORMAT; goto done; }
    if (strcmp(metric.u.s, "cosine") == 0) config->index_config.vector_metric = YAP_V2_VECTOR_COSINE;
    else if (strcmp(metric.u.s, "dot") == 0) config->index_config.vector_metric = YAP_V2_VECTOR_DOT;
    else if (strcmp(metric.u.s, "l2") == 0) config->index_config.vector_metric = YAP_V2_VECTOR_L2;
    else { free(metric.u.s); set_error(error, error_size, "vector.metric is invalid"); status = YAP_V2_INVALID_FORMAT; goto done; }
    free(metric.u.s);
  } else {
    toml_datum_t model = toml_string_in(vector, "model_id");
    toml_datum_t dimensions = toml_int_in(vector, "dimensions");
    if ((model.ok && model.u.s[0] != '\0') || (dimensions.ok && dimensions.u.i != 0)) {
      if (model.ok) free(model.u.s);
      set_error(error, error_size, "disabled vector configuration must be empty");
      status = YAP_V2_INVALID_FORMAT;
      goto done;
    }
    if (model.ok) free(model.u.s);
  }
  status = read_filterable_fields(metadata, &config->index_config, error, error_size);
  if (status != YAP_V2_OK || YAP_V2_config_validate(&config->index_config) != YAP_V2_OK) {
    if (status == YAP_V2_OK) status = YAP_V2_INVALID_FORMAT;
    goto done;
  }
  memcpy(base, canonical, strlen(canonical) + 1U); slash = strrchr(base, '/');
  if (slash == NULL) { set_error(error, error_size, "config path has no directory"); status = YAP_V2_INVALID_FORMAT; goto done; }
  if (slash == base) slash[1] = '\0'; else *slash = '\0';
  status = read_string(index, "directory", path_value, sizeof(path_value), 1, error, error_size);
  if (status == YAP_V2_OK) status = normalize_path(base, path_value, config->index_directory,
                                                   sizeof(config->index_directory), error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_string(daemon, "run_directory", path_value, sizeof(path_value), 1, error, error_size);
  if (status == YAP_V2_OK) status = normalize_path(base, path_value, config->run_directory,
                                                   sizeof(config->run_directory), error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_string(daemon, "core_host", config->core_host, sizeof(config->core_host), 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  value = config->core_port;
  status = read_uint32(daemon, "core_port", &value, 1U, 65535U, 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->core_port = (uint16_t)value;
  status = read_string(daemon, "front_host", config->front_host, sizeof(config->front_host), 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  value = config->front_port;
  status = read_uint32(daemon, "front_port", &value, 1U, 65535U, 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->front_port = (uint16_t)value;
  value = (uint32_t)config->runtime_policy.max_inflight;
  status = read_uint32(daemon, "max_inflight", &value, 1U, 1024U, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->runtime_policy.max_inflight = value;
  value = (uint32_t)config->runtime_policy.max_inflight_bytes;
  status = read_uint32(daemon, "max_inflight_bytes", &value, 1U, 1024U * 1024U * 1024U, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->runtime_policy.max_inflight_bytes = value;
  value = config->runtime_policy.request_timeout_ms;
  status = read_uint32(daemon, "request_timeout_ms", &value, 1U, 60000U, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->runtime_policy.request_timeout_ms = value;
  token = toml_string_in(daemon, "write_token");
  if (!token.ok && toml_key_exists(daemon, "write_token")) { set_error(error, error_size, "daemon.write_token must be a string"); status = YAP_V2_INVALID_FORMAT; goto done; }
  if (token.ok) {
    size_t i, length = strlen(token.u.s);
    if (length < 16U || length > YAP_V2_WRITE_TOKEN_MAX_BYTES) { free(token.u.s); status = YAP_V2_INVALID_FORMAT; set_error(error, error_size, "daemon.write_token must contain 16 to 255 bytes"); goto done; }
    for (i = 0U; i < length; i++) if ((unsigned char)token.u.s[i] <= 0x20U || (unsigned char)token.u.s[i] == 0x7fU) { free(token.u.s); status = YAP_V2_INVALID_FORMAT; set_error(error, error_size, "daemon.write_token contains whitespace or control bytes"); goto done; }
    memcpy(config->runtime_policy.write_token, token.u.s, length + 1U);
    config->runtime_policy.write_token_bytes = length; free(token.u.s);
  }
  status = YAP_V2_OK;
done:
  toml_free(root);
  return status;
}
