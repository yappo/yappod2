#include "yappo_lexical_v2.h"

#include "yappo_unicode.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  unsigned char *data;
  size_t len;
  size_t capacity;
} BUFFER;

typedef struct {
  char *term;
  size_t term_len;
  uint32_t object_type;
  uint64_t object_ordinal;
  uint32_t field;
  uint32_t position;
  uint32_t field_length;
} OCCURRENCE;

typedef struct {
  OCCURRENCE *items;
  size_t count;
  size_t capacity;
} OCCURRENCES;

static void put_u32(unsigned char *out, uint32_t value) {
  out[0] = (unsigned char)value;
  out[1] = (unsigned char)(value >> 8);
  out[2] = (unsigned char)(value >> 16);
  out[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *out, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++)
    out[i] = (unsigned char)(value >> (i * 8U));
}

static int append(BUFFER *buffer, const void *data, size_t len) {
  size_t required;
  size_t capacity;
  unsigned char *next;

  if (buffer == NULL || (len > 0U && data == NULL) || len > SIZE_MAX - buffer->len)
    return YAP_V2_INVALID_ARGUMENT;
  required = buffer->len + len;
  if (required > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES)
    return YAP_V2_OUT_OF_RANGE;
  if (required > buffer->capacity) {
    capacity = buffer->capacity == 0U ? 256U : buffer->capacity;
    while (capacity < required) {
      if (capacity > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES / 2U) {
        capacity = YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES;
        break;
      }
      capacity *= 2U;
    }
    next = (unsigned char *)realloc(buffer->data, capacity);
    if (next == NULL)
      return YAP_V2_ALLOCATION_FAILED;
    buffer->data = next;
    buffer->capacity = capacity;
  }
  if (len > 0U)
    memcpy(buffer->data + buffer->len, data, len);
  buffer->len = required;
  return YAP_V2_OK;
}

static int append_u32(BUFFER *buffer, uint32_t value) {
  unsigned char encoded[4];
  put_u32(encoded, value);
  return append(buffer, encoded, sizeof(encoded));
}

static int append_u64(BUFFER *buffer, uint64_t value) {
  unsigned char encoded[8];
  put_u64(encoded, value);
  return append(buffer, encoded, sizeof(encoded));
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_MAX;
  size_t i;
  unsigned int bit;
  for (i = 0U; i < len; i++) {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++)
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ UINT32_C(0x82f63b78) : crc >> 1;
  }
  return ~crc;
}

static int fsync_parent(const char *path) {
  char *parent = strdup(path);
  char *slash;
  int fd;
  int result;
  if (parent == NULL)
    return -1;
  slash = strrchr(parent, '/');
  if (slash == NULL)
    strcpy(parent, ".");
  else if (slash == parent)
    slash[1] = '\0';
  else
    *slash = '\0';
  fd = open(parent, O_RDONLY);
  free(parent);
  if (fd < 0)
    return -1;
  result = fsync(fd);
  if (close(fd) != 0)
    result = -1;
  return result;
}

static int write_component(const char *path, uint32_t file_type, uint64_t generation,
                           const BUFFER *payload, uint64_t records,
                           YAP_V2_COMPONENT_DESCRIPTOR *component) {
  YAP_V2_FILE_HEADER header;
  unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  char *temporary;
  FILE *file;
  size_t path_len;
  uint64_t bytes;
  int failed = 0;
  int status;

  memset(&header, 0, sizeof(header));
  header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES;
  header.file_type = file_type;
  header.generation = generation;
  header.payload_bytes = payload->len;
  header.payload_crc32c = crc32c(payload->data, payload->len);
  status = YAP_V2_file_header_encode(&header, encoded);
  if (status != YAP_V2_OK)
    return status;
  path_len = strlen(path);
  if (path_len > SIZE_MAX - 5U)
    return YAP_V2_OUT_OF_RANGE;
  temporary = (char *)malloc(path_len + 5U);
  if (temporary == NULL)
    return YAP_V2_ALLOCATION_FAILED;
  (void)snprintf(temporary, path_len + 5U, "%s.tmp", path);
  file = fopen(temporary, "wb");
  if (file == NULL) {
    free(temporary);
    return YAP_V2_IO_ERROR;
  }
  if (fwrite(encoded, 1U, sizeof(encoded), file) != sizeof(encoded) ||
      (payload->len > 0U && fwrite(payload->data, 1U, payload->len, file) != payload->len) ||
      fflush(file) != 0 || fsync(fileno(file)) != 0)
    failed = 1;
  if (fclose(file) != 0)
    failed = 1;
  if (failed || rename(temporary, path) != 0 || fsync_parent(path) != 0) {
    unlink(temporary);
    free(temporary);
    return YAP_V2_IO_ERROR;
  }
  free(temporary);
  memset(component, 0, sizeof(*component));
  component->file_type = file_type;
  component->record_count = records;
  status = YAP_V2_file_sha256(path, component->checksum, &bytes);
  component->file_bytes = bytes;
  return status;
}

static void occurrences_free(OCCURRENCES *occurrences) {
  size_t i;
  for (i = 0U; i < occurrences->count; i++)
    free(occurrences->items[i].term);
  free(occurrences->items);
  memset(occurrences, 0, sizeof(*occurrences));
}

static int occurrence_add(OCCURRENCES *occurrences, const char *term, size_t term_len,
                          uint32_t object_type, uint64_t object_ordinal, uint32_t field,
                          uint32_t position, uint32_t field_length) {
  OCCURRENCE *next;
  OCCURRENCE *item;
  size_t capacity;
  if (term_len == 0U || term_len > UINT32_MAX)
    return YAP_V2_INVALID_FORMAT;
  if (occurrences->count == occurrences->capacity) {
    capacity = occurrences->capacity == 0U ? 256U : occurrences->capacity * 2U;
    if (capacity < occurrences->capacity || capacity > SIZE_MAX / sizeof(*next))
      return YAP_V2_OUT_OF_RANGE;
    next = (OCCURRENCE *)realloc(occurrences->items, capacity * sizeof(*next));
    if (next == NULL)
      return YAP_V2_ALLOCATION_FAILED;
    occurrences->items = next;
    occurrences->capacity = capacity;
  }
  item = &occurrences->items[occurrences->count++];
  memset(item, 0, sizeof(*item));
  item->term = (char *)malloc(term_len);
  if (item->term == NULL) {
    occurrences->count--;
    return YAP_V2_ALLOCATION_FAILED;
  }
  memcpy(item->term, term, term_len);
  item->term_len = term_len;
  item->object_type = object_type;
  item->object_ordinal = object_ordinal;
  item->field = field;
  item->position = position;
  item->field_length = field_length;
  return YAP_V2_OK;
}

static int tokenize_field(OCCURRENCES *occurrences, YAP_V2_BYTES_VIEW text, uint32_t object_type,
                          uint64_t object_ordinal, uint32_t field) {
  YAP_V2_TOKEN_SEQUENCE sequence = {0};
  size_t i;
  int status;
  if (text.len == 0U)
    return YAP_V2_OK;
  status = YAP_V2_unicode_tokenize((const char *)text.data, text.len, &sequence);
  if (status != YAP_V2_OK)
    return status;
  if (sequence.token_count > UINT32_MAX) {
    YAP_V2_token_sequence_free(&sequence);
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0U; status == YAP_V2_OK && i < sequence.token_count; i++) {
    const YAP_V2_TOKEN *token = &sequence.tokens[i];
    status = occurrence_add(occurrences, sequence.normalized_utf8 + token->byte_start,
                            token->byte_end - token->byte_start, object_type, object_ordinal, field,
                            (uint32_t)i, (uint32_t)sequence.token_count);
  }
  YAP_V2_token_sequence_free(&sequence);
  return status;
}

static int occurrence_compare(const void *left, const void *right) {
  const OCCURRENCE *a = (const OCCURRENCE *)left;
  const OCCURRENCE *b = (const OCCURRENCE *)right;
  size_t common = a->term_len < b->term_len ? a->term_len : b->term_len;
  int order = memcmp(a->term, b->term, common);
  if (order != 0)
    return order;
  if (a->term_len != b->term_len)
    return a->term_len < b->term_len ? -1 : 1;
  if (a->object_type != b->object_type)
    return a->object_type < b->object_type ? -1 : 1;
  if (a->object_ordinal != b->object_ordinal)
    return a->object_ordinal < b->object_ordinal ? -1 : 1;
  if (a->field != b->field)
    return a->field < b->field ? -1 : 1;
  if (a->position != b->position)
    return a->position < b->position ? -1 : 1;
  return 0;
}

static int same_term(const OCCURRENCE *a, const OCCURRENCE *b) {
  return a->term_len == b->term_len && memcmp(a->term, b->term, a->term_len) == 0;
}

static int same_object(const OCCURRENCE *a, const OCCURRENCE *b) {
  return a->object_type == b->object_type && a->object_ordinal == b->object_ordinal;
}

static int build_payloads(OCCURRENCES *occurrences, size_t document_count, size_t passage_count,
                          BUFFER *terms, BUFFER *postings, BUFFER *positions,
                          uint64_t *term_count_out, uint64_t *posting_count_out) {
  size_t term_start;
  uint64_t term_ordinal = 0U;
  uint64_t posting_total = 0U;
  int status;

  if (occurrences->count > 1U)
    qsort(occurrences->items, occurrences->count, sizeof(*occurrences->items), occurrence_compare);
  status = append_u32(terms, YAP_V2_LEXICAL_PAYLOAD_VERSION);
  if (status == YAP_V2_OK)
    status = append_u64(terms, 0U);
  if (status == YAP_V2_OK)
    status = append_u32(postings, YAP_V2_LEXICAL_PAYLOAD_VERSION);
  if (status == YAP_V2_OK)
    status = append_u32(postings, YAP_V2_POSTINGS_BLOCK_SIZE);
  if (status == YAP_V2_OK)
    status = append_u64(postings, document_count);
  if (status == YAP_V2_OK)
    status = append_u64(postings, passage_count);
  if (status == YAP_V2_OK)
    status = append_u64(postings, 0U);
  if (status == YAP_V2_OK)
    status = append_u32(positions, YAP_V2_LEXICAL_PAYLOAD_VERSION);
  if (status == YAP_V2_OK)
    status = append_u64(positions, 0U);

  for (term_start = 0U; status == YAP_V2_OK && term_start < occurrences->count;) {
    size_t term_end = term_start + 1U;
    size_t object_start;
    uint64_t document_frequency = 0U;
    uint64_t posting_offset = postings->len;
    uint64_t position_offset = positions->len;
    uint64_t term_positions = 0U;

    while (term_end < occurrences->count &&
           same_term(&occurrences->items[term_start], &occurrences->items[term_end]))
      term_end++;
    for (object_start = term_start; object_start < term_end;) {
      size_t object_end = object_start + 1U;
      while (object_end < term_end &&
             same_object(&occurrences->items[object_start], &occurrences->items[object_end]))
        object_end++;
      document_frequency++;
      object_start = object_end;
    }
    status = append_u64(postings, term_ordinal);
    if (status == YAP_V2_OK)
      status = append_u64(postings, document_frequency);
    if (status == YAP_V2_OK)
      status =
        append_u32(postings, (uint32_t)((document_frequency + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                                        YAP_V2_POSTINGS_BLOCK_SIZE));
    if (status == YAP_V2_OK)
      status = append_u64(positions, term_ordinal);
    if (status == YAP_V2_OK)
      status = append_u64(positions, term_end - term_start);

    for (object_start = term_start; status == YAP_V2_OK && object_start < term_end;) {
      size_t object_end = object_start + 1U;
      size_t i;
      uint32_t title_tf = 0U, body_tf = 0U, passage_tf = 0U;
      uint32_t title_len = 0U, body_len = 0U, passage_len = 0U;
      while (object_end < term_end &&
             same_object(&occurrences->items[object_start], &occurrences->items[object_end]))
        object_end++;
      for (i = object_start; i < object_end; i++) {
        const OCCURRENCE *item = &occurrences->items[i];
        if (item->field == YAP_V2_FIELD_TITLE) {
          title_tf++;
          title_len = item->field_length;
        } else if (item->field == YAP_V2_FIELD_BODY) {
          body_tf++;
          body_len = item->field_length;
        } else {
          passage_tf++;
          passage_len = item->field_length;
        }
        status = append_u32(positions, item->field);
        if (status == YAP_V2_OK)
          status = append_u32(positions, item->position);
      }
      if (status == YAP_V2_OK)
        status = append_u32(postings, occurrences->items[object_start].object_type);
      if (status == YAP_V2_OK)
        status = append_u64(postings, occurrences->items[object_start].object_ordinal);
      if (status == YAP_V2_OK)
        status = append_u32(postings, title_tf);
      if (status == YAP_V2_OK)
        status = append_u32(postings, body_tf);
      if (status == YAP_V2_OK)
        status = append_u32(postings, passage_tf);
      if (status == YAP_V2_OK)
        status = append_u32(postings, title_len);
      if (status == YAP_V2_OK)
        status = append_u32(postings, body_len);
      if (status == YAP_V2_OK)
        status = append_u32(postings, passage_len);
      if (status == YAP_V2_OK)
        status = append_u64(postings, term_positions);
      if (status == YAP_V2_OK)
        status = append_u32(postings, (uint32_t)(object_end - object_start));
      term_positions += object_end - object_start;
      posting_total++;
      object_start = object_end;
    }
    /* Per-term block maxima permit a reader to skip blocks without decoding positions. */
    if (status == YAP_V2_OK) {
      size_t block_start;
      size_t posting_index = 0U;
      for (block_start = term_start; block_start < term_end;) {
        size_t block_end = block_start;
        uint32_t max_tf = 0U;
        uint32_t min_length = UINT32_MAX;
        size_t block_objects = 0U;
        while (block_end < term_end && block_objects < YAP_V2_POSTINGS_BLOCK_SIZE) {
          size_t object_end = block_end + 1U;
          uint32_t tf = 0U;
          uint32_t length = 0U;
          while (object_end < term_end &&
                 same_object(&occurrences->items[block_end], &occurrences->items[object_end]))
            object_end++;
          tf = (uint32_t)(object_end - block_end);
          length = occurrences->items[block_end].field_length;
          if (tf > max_tf)
            max_tf = tf;
          if (length < min_length)
            min_length = length;
          block_end = object_end;
          block_objects++;
        }
        status = append_u32(postings, (uint32_t)posting_index);
        if (status == YAP_V2_OK)
          status = append_u32(postings, (uint32_t)block_objects);
        if (status == YAP_V2_OK)
          status = append_u32(postings, max_tf);
        if (status == YAP_V2_OK)
          status = append_u32(postings, min_length == UINT32_MAX ? 0U : min_length);
        posting_index += block_objects;
        block_start = block_end;
      }
    }
    if (status == YAP_V2_OK)
      status = append_u32(terms, (uint32_t)occurrences->items[term_start].term_len);
    if (status == YAP_V2_OK)
      status =
        append(terms, occurrences->items[term_start].term, occurrences->items[term_start].term_len);
    if (status == YAP_V2_OK)
      status = append_u64(terms, document_frequency);
    if (status == YAP_V2_OK)
      status = append_u64(terms, posting_offset);
    if (status == YAP_V2_OK)
      status = append_u64(terms, postings->len - posting_offset);
    if (status == YAP_V2_OK)
      status = append_u64(terms, position_offset);
    if (status == YAP_V2_OK)
      status = append_u64(terms, positions->len - position_offset);
    term_ordinal++;
    term_start = term_end;
  }
  if (status == YAP_V2_OK) {
    put_u64(terms->data + 4U, term_ordinal);
    put_u64(postings->data + 24U, posting_total);
    put_u64(positions->data + 4U, occurrences->count);
    *term_count_out = term_ordinal;
    *posting_count_out = posting_total;
  }
  return status;
}

int YAP_V2_lexical_write(const char *segment_dir, uint64_t generation,
                         const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         YAP_V2_COMPONENT_DESCRIPTOR components[3]) {
  static const char *const names[] = {"terms.yap2", "postings.yap2", "positions.yap2"};
  static const uint32_t types[] = {YAP_V2_FILE_TERMS, YAP_V2_FILE_POSTINGS, YAP_V2_FILE_POSITIONS};
  OCCURRENCES occurrences = {0};
  BUFFER payloads[3] = {{0}};
  uint64_t term_count = 0U;
  uint64_t posting_count = 0U;
  uint64_t records[3];
  size_t i;
  int status = YAP_V2_OK;

  if (segment_dir == NULL || generation == 0U || components == NULL ||
      (document_count > 0U && documents == NULL) || (passage_count > 0U && passages == NULL) ||
      document_count > YAP_V2_MAX_SEGMENT_DOCUMENTS || passage_count > YAP_V2_MAX_SEGMENT_PASSAGES)
    return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; status == YAP_V2_OK && i < document_count; i++) {
    status = YAP_V2_document_validate(&documents[i]);
    if (status == YAP_V2_OK)
      status = tokenize_field(&occurrences, documents[i].title, YAP_V2_LEXICAL_DOCUMENT, i,
                              YAP_V2_FIELD_TITLE);
    if (status == YAP_V2_OK)
      status = tokenize_field(&occurrences, documents[i].body, YAP_V2_LEXICAL_DOCUMENT, i,
                              YAP_V2_FIELD_BODY);
  }
  for (i = 0U; status == YAP_V2_OK && i < passage_count; i++) {
    status = YAP_V2_passage_validate(&passages[i]);
    if (status == YAP_V2_OK)
      status = tokenize_field(&occurrences, passages[i].text, YAP_V2_LEXICAL_PASSAGE, i,
                              YAP_V2_FIELD_PASSAGE);
  }
  if (status == YAP_V2_OK)
    status = build_payloads(&occurrences, document_count, passage_count, &payloads[0], &payloads[1],
                            &payloads[2], &term_count, &posting_count);
  records[0] = term_count;
  records[1] = posting_count;
  records[2] = occurrences.count;
  for (i = 0U; status == YAP_V2_OK && i < 3U; i++) {
    size_t path_len = strlen(segment_dir) + strlen(names[i]) + 2U;
    char *path = (char *)malloc(path_len);
    if (path == NULL) {
      status = YAP_V2_ALLOCATION_FAILED;
      break;
    }
    (void)snprintf(path, path_len, "%s/%s", segment_dir, names[i]);
    status = write_component(path, types[i], generation, &payloads[i], records[i], &components[i]);
    if (status == YAP_V2_OK)
      (void)strcpy(components[i].name, names[i]);
    free(path);
  }
  for (i = 0U; i < 3U; i++)
    free(payloads[i].data);
  occurrences_free(&occurrences);
  return status;
}
