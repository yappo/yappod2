#include "yappo_index_v2.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define YAP_V2_SEGMENT_PAYLOAD_VERSION UINT32_C(1)
#define YAP_V2_SEGMENT_RECORD_DOCUMENT UINT32_C(1)
#define YAP_V2_SEGMENT_RECORD_PASSAGE UINT32_C(2)
#define YAP_V2_SEGMENT_RECORD_HEADER_BYTES 8U

typedef struct {
  unsigned char *data;
  size_t len;
  size_t capacity;
} YAP_V2_BUFFER;

static void put_u32_le(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value & 0xffU);
  output[1] = (unsigned char)((value >> 8) & 0xffU);
  output[2] = (unsigned char)((value >> 16) & 0xffU);
  output[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void put_u64_le(unsigned char *output, uint64_t value) {
  size_t i;

  for (i = 0; i < 8U; i++) {
    output[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
  }
}

static uint32_t get_u32_le(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) |
         ((uint32_t)input[3] << 24);
}

static uint64_t get_u64_le(const unsigned char *input) {
  uint64_t value = 0U;
  size_t i;

  for (i = 0; i < 8U; i++) {
    value |= (uint64_t)input[i] << (i * 8U);
  }
  return value;
}

static int bytes_equal(YAP_V2_BYTES_VIEW lhs, YAP_V2_BYTES_VIEW rhs) {
  return lhs.len == rhs.len && (lhs.len == 0U || memcmp(lhs.data, rhs.data, lhs.len) == 0);
}

static void buffer_free(YAP_V2_BUFFER *buffer) {
  if (buffer == NULL) {
    return;
  }
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static int buffer_reserve(YAP_V2_BUFFER *buffer, size_t additional) {
  size_t required;
  size_t next_capacity;
  unsigned char *next;

  if (buffer == NULL || (additional > 0U && buffer->data == NULL && buffer->capacity != 0U)) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  if (additional > (size_t)YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES - buffer->len) {
    return YAP_V2_OUT_OF_RANGE;
  }
  required = buffer->len + additional;
  if (required <= buffer->capacity) {
    return YAP_V2_OK;
  }

  next_capacity = (buffer->capacity == 0U) ? 256U : buffer->capacity;
  while (next_capacity < required) {
    if (next_capacity > (size_t)YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES / 2U) {
      next_capacity = (size_t)YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES;
      break;
    }
    next_capacity *= 2U;
  }
  next = (unsigned char *)realloc(buffer->data, next_capacity);
  if (next == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  buffer->data = next;
  buffer->capacity = next_capacity;
  return YAP_V2_OK;
}

static int buffer_append(YAP_V2_BUFFER *buffer, const void *data, size_t len) {
  int status;

  if (buffer == NULL || (len > 0U && data == NULL)) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = buffer_reserve(buffer, len);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (len > 0U) {
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
  }
  return YAP_V2_OK;
}

static int buffer_append_u32(YAP_V2_BUFFER *buffer, uint32_t value) {
  unsigned char encoded[4];

  put_u32_le(encoded, value);
  return buffer_append(buffer, encoded, sizeof(encoded));
}

static int buffer_append_u64(YAP_V2_BUFFER *buffer, uint64_t value) {
  unsigned char encoded[8];

  put_u64_le(encoded, value);
  return buffer_append(buffer, encoded, sizeof(encoded));
}

static int buffer_append_bytes_view(YAP_V2_BUFFER *buffer, YAP_V2_BYTES_VIEW value) {
  int status;

  if (value.len > UINT32_MAX) {
    return YAP_V2_OUT_OF_RANGE;
  }
  status = buffer_append_u32(buffer, (uint32_t)value.len);
  if (status != YAP_V2_OK) {
    return status;
  }
  return buffer_append(buffer, value.data, value.len);
}

static int validate_segment_views(const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                                 const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count) {
  size_t i;
  size_t j;

  if ((document_count > 0U && documents == NULL) || (passage_count > 0U && passages == NULL)) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  if (document_count > YAP_V2_MAX_SEGMENT_DOCUMENTS ||
      passage_count > YAP_V2_MAX_SEGMENT_PASSAGES) {
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0; i < document_count; i++) {
    int status = YAP_V2_document_validate(&documents[i]);
    if (status != YAP_V2_OK) {
      return status;
    }
    for (j = 0; j < i; j++) {
      if (bytes_equal(documents[i].id, documents[j].id)) {
        return YAP_V2_DUPLICATE;
      }
    }
  }
  for (i = 0; i < passage_count; i++) {
    int status = YAP_V2_passage_validate(&passages[i]);
    int parent_found = 0;

    if (status != YAP_V2_OK) {
      return status;
    }
    for (j = 0; j < document_count; j++) {
      if (bytes_equal(passages[i].parent_document_id, documents[j].id)) {
        parent_found = 1;
        break;
      }
    }
    if (!parent_found) {
      return YAP_V2_INVALID_FORMAT;
    }
    for (j = 0; j < i; j++) {
      if (bytes_equal(passages[i].id, passages[j].id) ||
          (bytes_equal(passages[i].parent_document_id, passages[j].parent_document_id) &&
           passages[i].ordinal == passages[j].ordinal)) {
        return YAP_V2_DUPLICATE;
      }
    }
  }
  return YAP_V2_OK;
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_MAX;
  size_t i;
  unsigned int bit;

  for (i = 0; i < len; i++) {
    crc ^= data[i];
    for (bit = 0; bit < 8U; bit++) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ UINT32_C(0x82f63b78) : crc >> 1;
    }
  }
  return ~crc;
}

typedef struct {
  uint32_t state[8];
  uint64_t bit_length;
  unsigned char block[64];
  size_t block_length;
} YAP_V2_SHA256;

static uint32_t sha256_rotr(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

static void sha256_transform(YAP_V2_SHA256 *ctx, const unsigned char block[64]) {
  static const uint32_t constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
    UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
    UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
    UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
    UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
    UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
    UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
    UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
    UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
    UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
    UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
  };
  uint32_t words[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t t1, t2;
  size_t i;

  for (i = 0; i < 16U; i++) {
    words[i] = ((uint32_t)block[i * 4U] << 24) | ((uint32_t)block[i * 4U + 1U] << 16) |
               ((uint32_t)block[i * 4U + 2U] << 8) | (uint32_t)block[i * 4U + 3U];
  }
  for (i = 16U; i < 64U; i++) {
    uint32_t s0 = sha256_rotr(words[i - 15U], 7U) ^ sha256_rotr(words[i - 15U], 18U) ^
                  (words[i - 15U] >> 3);
    uint32_t s1 = sha256_rotr(words[i - 2U], 17U) ^ sha256_rotr(words[i - 2U], 19U) ^
                  (words[i - 2U] >> 10);
    words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
  }

  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];
  for (i = 0; i < 64U; i++) {
    uint32_t sigma1 = sha256_rotr(e, 6U) ^ sha256_rotr(e, 11U) ^ sha256_rotr(e, 25U);
    uint32_t choose = (e & f) ^ ((~e) & g);
    uint32_t sigma0 = sha256_rotr(a, 2U) ^ sha256_rotr(a, 13U) ^ sha256_rotr(a, 22U);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    t1 = h + sigma1 + choose + constants[i] + words[i];
    t2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void sha256_init(YAP_V2_SHA256 *ctx) {
  static const uint32_t initial_state[8] = {
    UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
    UINT32_C(0x510e527f), UINT32_C(0x9b05688c), UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
  };

  memcpy(ctx->state, initial_state, sizeof(initial_state));
  ctx->bit_length = 0U;
  ctx->block_length = 0U;
}

static void sha256_update(YAP_V2_SHA256 *ctx, const unsigned char *data, size_t len) {
  size_t copied;

  while (len > 0U) {
    copied = 64U - ctx->block_length;
    if (copied > len) {
      copied = len;
    }
    memcpy(ctx->block + ctx->block_length, data, copied);
    ctx->block_length += copied;
    ctx->bit_length += (uint64_t)copied * 8U;
    data += copied;
    len -= copied;
    if (ctx->block_length == 64U) {
      sha256_transform(ctx, ctx->block);
      ctx->block_length = 0U;
    }
  }
}

static void sha256_final(YAP_V2_SHA256 *ctx, unsigned char digest[32]) {
  size_t i;

  ctx->block[ctx->block_length++] = 0x80U;
  while (ctx->block_length != 56U) {
    if (ctx->block_length == 64U) {
      sha256_transform(ctx, ctx->block);
      ctx->block_length = 0U;
    }
    ctx->block[ctx->block_length++] = 0U;
  }
  for (i = 0; i < 8U; i++) {
    ctx->block[56U + i] = (unsigned char)((ctx->bit_length >> (56U - i * 8U)) & 0xffU);
  }
  sha256_transform(ctx, ctx->block);
  for (i = 0; i < 8U; i++) {
    digest[i * 4U] = (unsigned char)(ctx->state[i] >> 24);
    digest[i * 4U + 1U] = (unsigned char)(ctx->state[i] >> 16);
    digest[i * 4U + 2U] = (unsigned char)(ctx->state[i] >> 8);
    digest[i * 4U + 3U] = (unsigned char)ctx->state[i];
  }
}

static void sha256_digest(const unsigned char *data, size_t len, unsigned char digest[32]) {
  YAP_V2_SHA256 ctx;

  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, digest);
}

static int append_document_record(YAP_V2_BUFFER *payload, const YAP_V2_DOCUMENT_VIEW *document) {
  size_t start = payload->len;
  int status;

  status = buffer_append_u32(payload, YAP_V2_SEGMENT_RECORD_DOCUMENT);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = buffer_append_u32(payload, 0U);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = buffer_append_bytes_view(payload, document->id);
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, document->url);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, document->title);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, document->body);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, document->metadata_json);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u64(payload, (uint64_t)document->updated_at_unix_ms);
  }
  if (status != YAP_V2_OK) {
    return status;
  }
  if (payload->len - start - YAP_V2_SEGMENT_RECORD_HEADER_BYTES > UINT32_MAX) {
    return YAP_V2_OUT_OF_RANGE;
  }
  put_u32_le(payload->data + start + 4U,
             (uint32_t)(payload->len - start - YAP_V2_SEGMENT_RECORD_HEADER_BYTES));
  return YAP_V2_OK;
}

static int append_passage_record(YAP_V2_BUFFER *payload, const YAP_V2_PASSAGE_VIEW *passage) {
  size_t start = payload->len;
  int status;

  status = buffer_append_u32(payload, YAP_V2_SEGMENT_RECORD_PASSAGE);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = buffer_append_u32(payload, 0U);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = buffer_append_bytes_view(payload, passage->id);
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, passage->parent_document_id);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_bytes_view(payload, passage->text);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u32(payload, passage->ordinal);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u32(payload, passage->start_char);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u32(payload, passage->end_char);
  }
  if (status != YAP_V2_OK) {
    return status;
  }
  if (payload->len - start - YAP_V2_SEGMENT_RECORD_HEADER_BYTES > UINT32_MAX) {
    return YAP_V2_OUT_OF_RANGE;
  }
  put_u32_le(payload->data + start + 4U,
             (uint32_t)(payload->len - start - YAP_V2_SEGMENT_RECORD_HEADER_BYTES));
  return YAP_V2_OK;
}

static int write_atomic(const char *path, const unsigned char *data, size_t len) {
  FILE *fp = NULL;
  char *temporary_path = NULL;
  size_t path_len;

  path_len = strlen(path);
  if (path_len > SIZE_MAX - 5U) {
    return YAP_V2_OUT_OF_RANGE;
  }
  temporary_path = (char *)malloc(path_len + 5U);
  if (temporary_path == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  if (snprintf(temporary_path, path_len + 5U, "%s.tmp", path) < 0) {
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }

  fp = fopen(temporary_path, "wb");
  if (fp == NULL) {
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  if (len > 0U && fwrite(data, 1U, len, fp) != len) {
    fclose(fp);
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
    fclose(fp);
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  if (fclose(fp) != 0) {
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  if (rename(temporary_path, path) != 0) {
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  free(temporary_path);
  return YAP_V2_OK;
}

void YAP_V2_segment_init(YAP_V2_SEGMENT *segment) {
  if (segment == NULL) {
    return;
  }
  memset(segment, 0, sizeof(*segment));
}

void YAP_V2_segment_free(YAP_V2_SEGMENT *segment) {
  if (segment == NULL) {
    return;
  }
  free(segment->documents);
  free(segment->passages);
  free(segment->storage);
  YAP_V2_segment_init(segment);
}

int YAP_V2_segment_write(const char *path, const char *segment_id, uint64_t generation,
                         const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         YAP_V2_SEGMENT_DESCRIPTOR *descriptor) {
  YAP_V2_BUFFER payload = {0};
  YAP_V2_FILE_HEADER header;
  unsigned char header_bytes[YAP_V2_FILE_HEADER_BYTES];
  unsigned char *file_bytes = NULL;
  size_t file_size;
  size_t id_len;
  unsigned char checksum[32];
  int status;
  size_t i;

  if (path == NULL || segment_id == NULL || generation == 0U || descriptor == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = YAP_V2_segment_id_validate(segment_id);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = validate_segment_views(documents, document_count, passages, passage_count);
  if (status != YAP_V2_OK) {
    return status;
  }
  id_len = strlen(segment_id);
  if (id_len > UINT32_MAX) {
    return YAP_V2_OUT_OF_RANGE;
  }
  status = buffer_append_u32(&payload, YAP_V2_SEGMENT_PAYLOAD_VERSION);
  if (status == YAP_V2_OK) {
    status = buffer_append_u32(&payload, (uint32_t)id_len);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append(&payload, segment_id, id_len);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u64(&payload, (uint64_t)document_count);
  }
  if (status == YAP_V2_OK) {
    status = buffer_append_u64(&payload, (uint64_t)passage_count);
  }
  for (i = 0; status == YAP_V2_OK && i < document_count; i++) {
    status = append_document_record(&payload, &documents[i]);
  }
  for (i = 0; status == YAP_V2_OK && i < passage_count; i++) {
    status = append_passage_record(&payload, &passages[i]);
  }
  if (status != YAP_V2_OK) {
    buffer_free(&payload);
    return status;
  }

  memset(&header, 0, sizeof(header));
  header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES;
  header.file_type = YAP_V2_FILE_DOCUMENTS;
  header.generation = generation;
  header.payload_bytes = (uint64_t)payload.len;
  header.payload_crc32c = crc32c(payload.data, payload.len);
  status = YAP_V2_file_header_encode(&header, header_bytes);
  if (status != YAP_V2_OK || payload.len > SIZE_MAX - YAP_V2_FILE_HEADER_BYTES) {
    buffer_free(&payload);
    return status != YAP_V2_OK ? status : YAP_V2_OUT_OF_RANGE;
  }
  file_size = YAP_V2_FILE_HEADER_BYTES + payload.len;
  file_bytes = (unsigned char *)malloc(file_size);
  if (file_bytes == NULL) {
    buffer_free(&payload);
    return YAP_V2_ALLOCATION_FAILED;
  }
  memcpy(file_bytes, header_bytes, sizeof(header_bytes));
  memcpy(file_bytes + sizeof(header_bytes), payload.data, payload.len);
  sha256_digest(file_bytes, file_size, checksum);
  status = write_atomic(path, file_bytes, file_size);
  if (status == YAP_V2_OK) {
    memset(descriptor, 0, sizeof(*descriptor));
    memcpy(descriptor->id, segment_id, id_len + 1U);
    descriptor->document_count = (uint64_t)document_count;
    descriptor->passage_count = (uint64_t)passage_count;
    descriptor->file_bytes = (uint64_t)file_size;
    memcpy(descriptor->checksum, checksum, sizeof(checksum));
  }
  free(file_bytes);
  buffer_free(&payload);
  return status;
}

static int read_u32(const unsigned char *data, size_t data_len, size_t *offset, uint32_t *value) {
  if (*offset > data_len || data_len - *offset < 4U) {
    return YAP_V2_INVALID_FORMAT;
  }
  *value = get_u32_le(data + *offset);
  *offset += 4U;
  return YAP_V2_OK;
}

static int read_u64(const unsigned char *data, size_t data_len, size_t *offset, uint64_t *value) {
  if (*offset > data_len || data_len - *offset < 8U) {
    return YAP_V2_INVALID_FORMAT;
  }
  *value = get_u64_le(data + *offset);
  *offset += 8U;
  return YAP_V2_OK;
}

static int read_bytes_view(const unsigned char *data, size_t data_len, size_t *offset,
                           YAP_V2_BYTES_VIEW *value) {
  uint32_t length;
  int status = read_u32(data, data_len, offset, &length);

  if (status != YAP_V2_OK) {
    return status;
  }
  if ((size_t)length > data_len - *offset) {
    return YAP_V2_INVALID_FORMAT;
  }
  value->data = data + *offset;
  value->len = (size_t)length;
  *offset += (size_t)length;
  return YAP_V2_OK;
}

static int read_file(const char *path, unsigned char **data_out, size_t *size_out) {
  FILE *fp = NULL;
  long file_size;
  unsigned char *data = NULL;

  fp = fopen(path, "rb");
  if (fp == NULL) {
    return YAP_V2_IO_ERROR;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  file_size = ftell(fp);
  if (file_size < 0 || (uint64_t)file_size >
                         (uint64_t)YAP_V2_FILE_HEADER_BYTES + YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES) {
    fclose(fp);
    return file_size < 0 ? YAP_V2_IO_ERROR : YAP_V2_OUT_OF_RANGE;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  data = (unsigned char *)malloc((size_t)file_size);
  if (data == NULL && file_size > 0L) {
    fclose(fp);
    return YAP_V2_ALLOCATION_FAILED;
  }
  if (file_size > 0L && fread(data, 1U, (size_t)file_size, fp) != (size_t)file_size) {
    free(data);
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  if (fclose(fp) != 0) {
    free(data);
    return YAP_V2_IO_ERROR;
  }
  *data_out = data;
  *size_out = (size_t)file_size;
  return YAP_V2_OK;
}

int YAP_V2_segment_read(const char *path, uint64_t expected_generation,
                        YAP_V2_SEGMENT *segment, YAP_V2_SEGMENT_DESCRIPTOR *descriptor) {
  unsigned char *file_bytes = NULL;
  size_t file_size = 0U;
  YAP_V2_FILE_HEADER header;
  unsigned char checksum[32];
  size_t offset;
  uint32_t payload_version;
  uint32_t segment_id_len;
  uint64_t document_count_u64;
  uint64_t passage_count_u64;
  size_t document_count;
  size_t passage_count;
  size_t i;
  int status;

  if (path == NULL || segment == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  YAP_V2_segment_free(segment);
  status = read_file(path, &file_bytes, &file_size);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (file_size < YAP_V2_FILE_HEADER_BYTES) {
    free(file_bytes);
    return YAP_V2_INVALID_FORMAT;
  }
  status = YAP_V2_file_header_decode(file_bytes, &header);
  if (status != YAP_V2_OK || header.file_type != YAP_V2_FILE_DOCUMENTS ||
      header.payload_bytes != (uint64_t)(file_size - YAP_V2_FILE_HEADER_BYTES) ||
      (expected_generation != 0U && header.generation != expected_generation)) {
    free(file_bytes);
    return YAP_V2_INVALID_FORMAT;
  }
  if (crc32c(file_bytes + YAP_V2_FILE_HEADER_BYTES, file_size - YAP_V2_FILE_HEADER_BYTES) !=
      header.payload_crc32c) {
    free(file_bytes);
    return YAP_V2_CHECKSUM_MISMATCH;
  }
  sha256_digest(file_bytes, file_size, checksum);
  if (header.payload_bytes > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES) {
    free(file_bytes);
    return YAP_V2_OUT_OF_RANGE;
  }

  segment->storage_bytes = file_size - YAP_V2_FILE_HEADER_BYTES;
  segment->storage = (unsigned char *)malloc(segment->storage_bytes);
  if (segment->storage == NULL && segment->storage_bytes > 0U) {
    free(file_bytes);
    YAP_V2_segment_init(segment);
    return YAP_V2_ALLOCATION_FAILED;
  }
  memcpy(segment->storage, file_bytes + YAP_V2_FILE_HEADER_BYTES, segment->storage_bytes);
  free(file_bytes);
  segment->generation = header.generation;
  offset = 0U;
  status = read_u32(segment->storage, segment->storage_bytes, &offset, &payload_version);
  if (status == YAP_V2_OK) {
    status = read_u32(segment->storage, segment->storage_bytes, &offset, &segment_id_len);
  }
  if (status != YAP_V2_OK || payload_version != YAP_V2_SEGMENT_PAYLOAD_VERSION ||
      segment_id_len == 0U || segment_id_len > YAP_V2_MAX_IDENTIFIER_BYTES ||
      (size_t)segment_id_len > segment->storage_bytes - offset) {
    YAP_V2_segment_free(segment);
    return YAP_V2_INVALID_FORMAT;
  }
  memcpy(segment->id, segment->storage + offset, segment_id_len);
  segment->id[segment_id_len] = '\0';
  offset += segment_id_len;
  status = YAP_V2_segment_id_validate(segment->id);
  if (status != YAP_V2_OK) {
    YAP_V2_segment_free(segment);
    return status;
  }
  status = read_u64(segment->storage, segment->storage_bytes, &offset, &document_count_u64);
  if (status == YAP_V2_OK) {
    status = read_u64(segment->storage, segment->storage_bytes, &offset, &passage_count_u64);
  }
  if (status != YAP_V2_OK) {
    YAP_V2_segment_free(segment);
    return YAP_V2_INVALID_FORMAT;
  }
  if (document_count_u64 > YAP_V2_MAX_SEGMENT_DOCUMENTS ||
      passage_count_u64 > YAP_V2_MAX_SEGMENT_PASSAGES || document_count_u64 > SIZE_MAX ||
      passage_count_u64 > SIZE_MAX) {
    YAP_V2_segment_free(segment);
    return YAP_V2_OUT_OF_RANGE;
  }
  document_count = (size_t)document_count_u64;
  passage_count = (size_t)passage_count_u64;
  if (document_count > 0U) {
    segment->documents = (YAP_V2_DOCUMENT_VIEW *)calloc(document_count, sizeof(*segment->documents));
    if (segment->documents == NULL) {
      YAP_V2_segment_free(segment);
      return YAP_V2_ALLOCATION_FAILED;
    }
  }
  if (passage_count > 0U) {
    segment->passages = (YAP_V2_PASSAGE_VIEW *)calloc(passage_count, sizeof(*segment->passages));
    if (segment->passages == NULL) {
      YAP_V2_segment_free(segment);
      return YAP_V2_ALLOCATION_FAILED;
    }
  }
  for (i = 0; i < document_count + passage_count; i++) {
    uint32_t record_type;
    uint32_t record_bytes;
    size_t record_end;

    status = read_u32(segment->storage, segment->storage_bytes, &offset, &record_type);
    if (status == YAP_V2_OK) {
      status = read_u32(segment->storage, segment->storage_bytes, &offset, &record_bytes);
    }
    if (status != YAP_V2_OK || (size_t)record_bytes > segment->storage_bytes - offset) {
      YAP_V2_segment_free(segment);
      return YAP_V2_INVALID_FORMAT;
    }
    record_end = offset + (size_t)record_bytes;
    if (i < document_count && record_type == YAP_V2_SEGMENT_RECORD_DOCUMENT) {
      uint64_t updated_at;
      status = read_bytes_view(segment->storage, record_end, &offset, &segment->documents[i].id);
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset, &segment->documents[i].url);
      }
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset, &segment->documents[i].title);
      }
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset, &segment->documents[i].body);
      }
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset,
                                 &segment->documents[i].metadata_json);
      }
      if (status == YAP_V2_OK) {
        status = read_u64(segment->storage, record_end, &offset, &updated_at);
      }
      if (status == YAP_V2_OK && updated_at <= INT64_MAX) {
        segment->documents[i].updated_at_unix_ms = (int64_t)updated_at;
      } else if (status == YAP_V2_OK) {
        status = YAP_V2_OUT_OF_RANGE;
      }
    } else if (i >= document_count && record_type == YAP_V2_SEGMENT_RECORD_PASSAGE) {
      size_t passage_index = i - document_count;
      status = read_bytes_view(segment->storage, record_end, &offset, &segment->passages[passage_index].id);
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset,
                                 &segment->passages[passage_index].parent_document_id);
      }
      if (status == YAP_V2_OK) {
        status = read_bytes_view(segment->storage, record_end, &offset,
                                 &segment->passages[passage_index].text);
      }
      if (status == YAP_V2_OK) {
        status = read_u32(segment->storage, record_end, &offset, &segment->passages[passage_index].ordinal);
      }
      if (status == YAP_V2_OK) {
        status = read_u32(segment->storage, record_end, &offset, &segment->passages[passage_index].start_char);
      }
      if (status == YAP_V2_OK) {
        status = read_u32(segment->storage, record_end, &offset, &segment->passages[passage_index].end_char);
      }
    } else {
      status = YAP_V2_INVALID_FORMAT;
    }
    if (status != YAP_V2_OK || offset != record_end) {
      YAP_V2_segment_free(segment);
      return status != YAP_V2_OK ? status : YAP_V2_INVALID_FORMAT;
    }
    offset = record_end;
  }
  if (offset != segment->storage_bytes ||
      validate_segment_views(segment->documents, document_count, segment->passages, passage_count) !=
        YAP_V2_OK) {
    YAP_V2_segment_free(segment);
    return YAP_V2_INVALID_FORMAT;
  }
  segment->document_count = document_count;
  segment->passage_count = passage_count;
  if (descriptor != NULL) {
    memset(descriptor, 0, sizeof(*descriptor));
    memcpy(descriptor->id, segment->id, sizeof(segment->id));
    descriptor->document_count = document_count_u64;
    descriptor->passage_count = passage_count_u64;
    descriptor->file_bytes = (uint64_t)(segment->storage_bytes + YAP_V2_FILE_HEADER_BYTES);
    memcpy(descriptor->checksum, checksum, sizeof(checksum));
  }
  return YAP_V2_OK;
}
