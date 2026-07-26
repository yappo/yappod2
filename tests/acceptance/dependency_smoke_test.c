#include <stddef.h>
#include <string.h>

#include <curl/curl.h>
#include <event2/event.h>
#include <toml.h>
#include <unicode/uversion.h>
#include <usearch.h>
#include <yyjson.h>

static int check_icu(void) {
  UVersionInfo version;

  u_getVersion(version);
  return version[0] == 0U ? -1 : 0;
}

static int check_curl(void) {
  const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);

  return version == NULL || version->version == NULL ? -1 : 0;
}

static int check_libevent(void) {
  const char *version = event_get_version();

  return version == NULL || version[0] == '\0' ? -1 : 0;
}

static int check_yyjson(void) {
  static const char input[] = "{\"ready\":true}";
  yyjson_doc *document;
  yyjson_val *root;
  yyjson_val *ready;
  int status = -1;

  document = yyjson_read(input, sizeof(input) - 1U, 0U);
  if (document == NULL) return -1;

  root = yyjson_doc_get_root(document);
  ready = yyjson_obj_get(root, "ready");
  if (yyjson_is_true(ready)) status = 0;
  yyjson_doc_free(document);
  return status;
}

static int check_tomlc99(void) {
  char input[] = "ready = true\n";
  char error[128];
  toml_table_t *table;
  toml_datum_t ready;
  int status = -1;

  table = toml_parse(input, error, (int)sizeof(error));
  if (table == NULL) return -1;

  ready = toml_bool_in(table, "ready");
  if (ready.ok && ready.u.b) status = 0;
  toml_free(table);
  return status;
}

static int check_usearch(void) {
  usearch_init_options_t options;
  usearch_error_t error = NULL;
  usearch_index_t index;

  memset(&options, 0, sizeof(options));
  options.metric_kind = usearch_metric_cos_k;
  options.quantization = usearch_scalar_f32_k;
  options.dimensions = 2U;
  options.connectivity = 16U;
  options.expansion_add = 32U;
  options.expansion_search = 16U;

  index = usearch_init(&options, &error);
  if (index == NULL || error != NULL) return -1;
  if (strcmp(usearch_version(), "2.24.0") != 0) {
    usearch_free(index, &error);
    return -1;
  }

  usearch_free(index, &error);
  return error == NULL ? 0 : -1;
}

int main(void) {
  if (check_icu() != 0 || check_curl() != 0 || check_libevent() != 0 ||
      check_yyjson() != 0 || check_tomlc99() != 0 || check_usearch() != 0) {
    return 1;
  }
  return 0;
}
