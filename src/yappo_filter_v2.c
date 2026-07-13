#include "yappo_filter_v2.h"

#include "yappo_lexical_v2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#define MAX_FILTER_DEPTH 32U
#define MAX_FILTER_NODES 1024U

static int only_keys(yyjson_val *object, const char *const *keys) {
  yyjson_obj_iter iterator; yyjson_val *key;
  if (!yyjson_is_obj(object)) return 0;
  iterator = yyjson_obj_iter_with(object);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key); size_t i; int found = 0;
    for (i = 0U; keys[i] != NULL; i++) if (strcmp(name, keys[i]) == 0) { found = 1; break; }
    if (!found) return 0;
  }
  return 1;
}

static int field_valid(yyjson_val *object, const YAP_V2_METADATA_INDEX *metadata) {
  yyjson_val *field = yyjson_obj_get(object, "field"); uint32_t ordinal;
  return yyjson_is_str(field) && yyjson_get_len(field) <= YAP_V2_MAX_FILTER_FIELD_BYTES &&
         YAP_V2_metadata_field_ordinal(metadata, yyjson_get_str(field), &ordinal) == YAP_V2_OK;
}

static int scalar(yyjson_val *value) {
  return yyjson_is_null(value) || yyjson_is_bool(value) || yyjson_is_num(value) || yyjson_is_str(value);
}

static int validate_node(yyjson_val *node, const YAP_V2_METADATA_INDEX *metadata, size_t depth,
                         size_t *nodes) {
  yyjson_obj_iter iterator; yyjson_val *key, *body; const char *op;
  static const char *const eq_keys[] = {"field", "value", NULL};
  static const char *const in_keys[] = {"field", "values", NULL};
  static const char *const range_keys[] = {"field", "gt", "gte", "lt", "lte", NULL};
  static const char *const exists_keys[] = {"field", NULL};
  if (depth > MAX_FILTER_DEPTH || ++*nodes > MAX_FILTER_NODES || !yyjson_is_obj(node) || yyjson_obj_size(node) != 1U) return 0;
  iterator = yyjson_obj_iter_with(node); key = yyjson_obj_iter_next(&iterator); op = yyjson_get_str(key); body = yyjson_obj_iter_get_val(key);
  if (strcmp(op, "eq") == 0) return only_keys(body, eq_keys) && yyjson_obj_size(body) == 2U && field_valid(body, metadata) && scalar(yyjson_obj_get(body, "value"));
  if (strcmp(op, "exists") == 0) return only_keys(body, exists_keys) && yyjson_obj_size(body) == 1U && field_valid(body, metadata);
  if (strcmp(op, "in") == 0) {
    yyjson_val *values; yyjson_arr_iter items; yyjson_val *item;
    if (!only_keys(body, in_keys) || yyjson_obj_size(body) != 2U || !field_valid(body, metadata) || !yyjson_is_arr(values = yyjson_obj_get(body, "values")) || yyjson_arr_size(values) == 0U) return 0;
    yyjson_arr_iter_init(values, &items); while ((item = yyjson_arr_iter_next(&items)) != NULL) if (!scalar(item)) return 0; return 1;
  }
  if (strcmp(op, "range") == 0) {
    size_t bounds = 0U; const char *names[] = {"gt", "gte", "lt", "lte", NULL}; size_t i;
    if (!only_keys(body, range_keys) || !field_valid(body, metadata)) return 0;
    for (i = 0U; names[i] != NULL; i++) { yyjson_val *value = yyjson_obj_get(body, names[i]); if (value != NULL) { if (!yyjson_is_num(value)) return 0; bounds++; } }
    return bounds > 0U;
  }
  if (strcmp(op, "not") == 0) return validate_node(body, metadata, depth + 1U, nodes);
  if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
    yyjson_arr_iter items; yyjson_val *item;
    if (!yyjson_is_arr(body) || yyjson_arr_size(body) == 0U) return 0;
    yyjson_arr_iter_init(body, &items); while ((item = yyjson_arr_iter_next(&items)) != NULL) if (!validate_node(item, metadata, depth + 1U, nodes)) return 0; return 1;
  }
  return 0;
}

static int literal_equal(const YAP_V2_METADATA_ENTRY *entry, yyjson_val *literal) {
  if (yyjson_is_null(literal)) return entry->type == YAP_V2_METADATA_NULL;
  if (yyjson_is_bool(literal)) return entry->type == YAP_V2_METADATA_BOOL && entry->value.len == 1U && entry->value.data[0] == (yyjson_is_true(literal) ? 1U : 0U);
  if (yyjson_is_str(literal)) return entry->type == YAP_V2_METADATA_STRING && entry->value.len == yyjson_get_len(literal) && memcmp(entry->value.data, yyjson_get_str(literal), entry->value.len) == 0;
  if (yyjson_is_num(literal) && entry->type == YAP_V2_METADATA_NUMBER) {
    char buffer[65]; double stored, wanted = yyjson_get_num(literal);
    if (entry->value.len >= sizeof(buffer)) return 0;
    memcpy(buffer, entry->value.data, entry->value.len);
    buffer[entry->value.len] = '\0'; stored = strtod(buffer, NULL);
    return stored == wanted;
  }
  return 0;
}

static int field_entry(const YAP_V2_METADATA_INDEX *metadata, uint32_t field, uint64_t document,
                       size_t *cursor, const YAP_V2_METADATA_ENTRY **entry) {
  size_t i;
  for (i = *cursor; i < metadata->entry_count; i++) if (metadata->entries[i].field_ordinal == field && metadata->entries[i].document_ordinal == document) { *cursor = i + 1U; *entry = &metadata->entries[i]; return 1; }
  return 0;
}

static int eval_node(yyjson_val *node, const YAP_V2_METADATA_INDEX *metadata, uint64_t document) {
  yyjson_obj_iter iterator = yyjson_obj_iter_with(node); yyjson_val *key = yyjson_obj_iter_next(&iterator), *body = yyjson_obj_iter_get_val(key);
  const char *op = yyjson_get_str(key);
  if (strcmp(op, "not") == 0) return !eval_node(body, metadata, document);
  if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
    yyjson_arr_iter items; yyjson_val *item; int is_and = strcmp(op, "and") == 0;
    yyjson_arr_iter_init(body, &items); while ((item = yyjson_arr_iter_next(&items)) != NULL) { int value = eval_node(item, metadata, document); if ((is_and && !value) || (!is_and && value)) return !is_and; } return is_and;
  } else {
    yyjson_val *field_value = yyjson_obj_get(body, "field"); uint32_t field; size_t cursor = 0U; const YAP_V2_METADATA_ENTRY *entry;
    YAP_V2_metadata_field_ordinal(metadata, yyjson_get_str(field_value), &field);
    if (strcmp(op, "exists") == 0) return field_entry(metadata, field, document, &cursor, &entry);
    while (field_entry(metadata, field, document, &cursor, &entry)) {
      if (strcmp(op, "eq") == 0 && literal_equal(entry, yyjson_obj_get(body, "value"))) return 1;
      if (strcmp(op, "in") == 0) { yyjson_arr_iter values; yyjson_val *value; yyjson_arr_iter_init(yyjson_obj_get(body, "values"), &values); while ((value = yyjson_arr_iter_next(&values)) != NULL) if (literal_equal(entry, value)) return 1; }
      if (strcmp(op, "range") == 0 && entry->type == YAP_V2_METADATA_NUMBER) {
        char buffer[65]; double number; const char *names[] = {"gt", "gte", "lt", "lte"}; size_t i; int ok = 1;
        if (entry->value.len >= sizeof(buffer)) continue;
        memcpy(buffer, entry->value.data, entry->value.len);
        buffer[entry->value.len] = '\0'; number = strtod(buffer, NULL);
        for (i = 0U; i < 4U; i++) { yyjson_val *bound = yyjson_obj_get(body, names[i]); if (bound != NULL) { double value = yyjson_get_num(bound); if ((i == 0U && !(number > value)) || (i == 1U && !(number >= value)) || (i == 2U && !(number < value)) || (i == 3U && !(number <= value))) ok = 0; } }
        if (ok) return 1;
      }
    }
  }
  return 0;
}

void YAP_V2_filter_init(YAP_V2_FILTER *filter) { if (filter != NULL) memset(filter, 0, sizeof(*filter)); }
void YAP_V2_filter_free(YAP_V2_FILTER *filter) { if (filter != NULL) { yyjson_doc_free((yyjson_doc *)filter->document); memset(filter, 0, sizeof(*filter)); } }

int YAP_V2_filter_compile(YAP_V2_BYTES_VIEW json, const YAP_V2_METADATA_INDEX *metadata,
                          YAP_V2_FILTER *filter) {
  yyjson_doc *document; size_t nodes = 0U;
  if (json.data == NULL || json.len == 0U || metadata == NULL || filter == NULL) return YAP_V2_INVALID_ARGUMENT;
  document = yyjson_read((const char *)json.data, json.len, 0U);
  if (document == NULL || !validate_node(yyjson_doc_get_root(document), metadata, 1U, &nodes)) { yyjson_doc_free(document); return YAP_V2_INVALID_FORMAT; }
  YAP_V2_filter_free(filter); filter->document = document; filter->metadata = metadata; return YAP_V2_OK;
}

int YAP_V2_filter_matches(const YAP_V2_FILTER *filter, uint64_t document_ordinal, int *matches) {
  if (filter == NULL || filter->document == NULL || filter->metadata == NULL || matches == NULL || document_ordinal >= filter->metadata->document_count) return YAP_V2_INVALID_ARGUMENT;
  *matches = eval_node(yyjson_doc_get_root((yyjson_doc *)filter->document), filter->metadata, document_ordinal); return YAP_V2_OK;
}

int YAP_V2_filter_accept(void *context, uint32_t object_type, uint64_t object_ordinal) {
  YAP_V2_FILTER *filter = context; int matches = 0;
  if (object_type != YAP_V2_LEXICAL_DOCUMENT || YAP_V2_filter_matches(filter, object_ordinal, &matches) != YAP_V2_OK) return 0;
  return matches;
}
