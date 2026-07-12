#include "yappo_manifest_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define YAP_V2_MAX_MANIFEST_BYTES (16U * 1024U * 1024U)

typedef struct {
  char *data;
  size_t len;
  size_t offset;
} YAP_V2_JSON_PARSER;

static void parser_skip_space(YAP_V2_JSON_PARSER *parser) {
  while (parser->offset < parser->len) {
    char c = parser->data[parser->offset];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      break;
    }
    parser->offset++;
  }
}

static int parser_expect(YAP_V2_JSON_PARSER *parser, char expected) {
  parser_skip_space(parser);
  if (parser->offset >= parser->len || parser->data[parser->offset] != expected) {
    return YAP_V2_INVALID_FORMAT;
  }
  parser->offset++;
  return YAP_V2_OK;
}

static int parser_string(YAP_V2_JSON_PARSER *parser, char *output, size_t output_size,
                         size_t *length_out) {
  size_t length = 0U;

  parser_skip_space(parser);
  if (parser->offset >= parser->len || parser->data[parser->offset] != '"') {
    return YAP_V2_INVALID_FORMAT;
  }
  parser->offset++;
  while (parser->offset < parser->len) {
    unsigned char c = (unsigned char)parser->data[parser->offset++];
    if (c == '"') {
      if (output != NULL) {
        if (length >= output_size) {
          return YAP_V2_OUT_OF_RANGE;
        }
        output[length] = '\0';
      }
      if (length_out != NULL) {
        *length_out = length;
      }
      return YAP_V2_OK;
    }
    if (c == '\\' || c < 0x20U) {
      return YAP_V2_INVALID_FORMAT;
    }
    if (output != NULL) {
      if (length + 1U >= output_size) {
        return YAP_V2_OUT_OF_RANGE;
      }
      output[length] = (char)c;
    }
    length++;
  }
  return YAP_V2_INVALID_FORMAT;
}

static int parser_key(YAP_V2_JSON_PARSER *parser, const char *expected) {
  char key[64];
  int status = parser_string(parser, key, sizeof(key), NULL);

  if (status != YAP_V2_OK) {
    return status;
  }
  return strcmp(key, expected) == 0 ? YAP_V2_OK : YAP_V2_INVALID_FORMAT;
}

static int parser_u64(YAP_V2_JSON_PARSER *parser, uint64_t *value_out) {
  uint64_t value = 0U;
  size_t digits = 0U;

  parser_skip_space(parser);
  while (parser->offset < parser->len) {
    unsigned char c = (unsigned char)parser->data[parser->offset];
    uint64_t digit;
    if (c < '0' || c > '9') {
      break;
    }
    digit = (uint64_t)(c - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return YAP_V2_OUT_OF_RANGE;
    }
    value = value * 10U + digit;
    parser->offset++;
    digits++;
  }
  if (digits == 0U || value_out == NULL) {
    return YAP_V2_INVALID_FORMAT;
  }
  *value_out = value;
  return YAP_V2_OK;
}

static int parser_hex_checksum(YAP_V2_JSON_PARSER *parser, unsigned char checksum[32]) {
  char hex[65];
  size_t i;
  int status = parser_string(parser, hex, sizeof(hex), NULL);

  if (status != YAP_V2_OK || strlen(hex) != 64U) {
    return YAP_V2_INVALID_FORMAT;
  }
  memset(checksum, 0, 32U);
  for (i = 0; i < 32U; i++) {
    unsigned char high = (unsigned char)hex[i * 2U];
    unsigned char low = (unsigned char)hex[i * 2U + 1U];
    unsigned char high_value;
    unsigned char low_value;
    if (high >= '0' && high <= '9') {
      high_value = (unsigned char)(high - '0');
    } else if (high >= 'a' && high <= 'f') {
      high_value = (unsigned char)(high - 'a' + 10U);
    } else if (high >= 'A' && high <= 'F') {
      high_value = (unsigned char)(high - 'A' + 10U);
    } else {
      return YAP_V2_INVALID_FORMAT;
    }
    if (low >= '0' && low <= '9') {
      low_value = (unsigned char)(low - '0');
    } else if (low >= 'a' && low <= 'f') {
      low_value = (unsigned char)(low - 'a' + 10U);
    } else if (low >= 'A' && low <= 'F') {
      low_value = (unsigned char)(low - 'A' + 10U);
    } else {
      return YAP_V2_INVALID_FORMAT;
    }
    checksum[i] = (unsigned char)((high_value << 4) | low_value);
  }
  return YAP_V2_OK;
}

static int load_file(const char *path, char **data_out, size_t *len_out) {
  FILE *fp;
  long file_size;
  char *data;

  fp = fopen(path, "rb");
  if (fp == NULL) {
    return YAP_V2_IO_ERROR;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  file_size = ftell(fp);
  if (file_size < 0) {
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  if ((uint64_t)file_size > YAP_V2_MAX_MANIFEST_BYTES) {
    fclose(fp);
    return YAP_V2_OUT_OF_RANGE;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  data = (char *)malloc((size_t)file_size + 1U);
  if (data == NULL) {
    fclose(fp);
    return YAP_V2_ALLOCATION_FAILED;
  }
  if (file_size > 0L && fread(data, 1U, (size_t)file_size, fp) != (size_t)file_size) {
    free(data);
    fclose(fp);
    return YAP_V2_IO_ERROR;
  }
  data[file_size] = '\0';
  if (fclose(fp) != 0) {
    free(data);
    return YAP_V2_IO_ERROR;
  }
  *data_out = data;
  *len_out = (size_t)file_size;
  return YAP_V2_OK;
}

static int parse_segment(YAP_V2_JSON_PARSER *parser, YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  char id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  uint64_t value;
  int status;

  status = parser_expect(parser, '{');
  if (status == YAP_V2_OK) {
    status = parser_key(parser, "id");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_string(parser, id, sizeof(id), NULL);
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(parser, "documents");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_u64(parser, &value);
    segment->document_count = value;
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(parser, "passages");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_u64(parser, &value);
    segment->passage_count = value;
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(parser, "file_bytes");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_u64(parser, &segment->file_bytes);
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(parser, "checksum");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_hex_checksum(parser, segment->checksum);
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(parser, '}');
  }
  if (status == YAP_V2_OK) {
    memcpy(segment->id, id, sizeof(segment->id));
    status = YAP_V2_segment_id_validate(segment->id);
  }
  return status;
}

int YAP_V2_manifest_load(const char *path, YAP_V2_MANIFEST *manifest) {
  YAP_V2_JSON_PARSER parser;
  YAP_V2_MANIFEST parsed;
  char *data = NULL;
  size_t data_len = 0U;
  uint64_t value;
  int status;

  if (path == NULL || manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = load_file(path, &data, &data_len);
  if (status != YAP_V2_OK) {
    return status;
  }
  YAP_V2_manifest_init(&parsed);
  parser.data = data;
  parser.len = data_len;
  parser.offset = 0U;
  status = parser_expect(&parser, '{');
  if (status == YAP_V2_OK) {
    status = parser_key(&parser, "format_version");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_u64(&parser, &value);
    if (status == YAP_V2_OK && value != YAP_V2_FORMAT_VERSION) {
      status = YAP_V2_INVALID_FORMAT;
    }
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(&parser, "generation");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_u64(&parser, &parsed.generation);
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ',');
  }
  if (status == YAP_V2_OK) {
    status = parser_key(&parser, "segments");
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ':');
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, '[');
  }
  if (status == YAP_V2_OK) {
    parser_skip_space(&parser);
    if (parser.offset < parser.len && parser.data[parser.offset] != ']') {
      for (;;) {
        YAP_V2_SEGMENT_DESCRIPTOR segment;
        memset(&segment, 0, sizeof(segment));
        status = parse_segment(&parser, &segment);
        if (status == YAP_V2_OK) {
          status = YAP_V2_manifest_add_segment(&parsed, &segment);
        }
        if (status != YAP_V2_OK) {
          break;
        }
        parser_skip_space(&parser);
        if (parser.offset < parser.len && parser.data[parser.offset] == ',') {
          parser.offset++;
          continue;
        }
        break;
      }
    }
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, ']');
  }
  if (status == YAP_V2_OK) {
    status = parser_expect(&parser, '}');
  }
  parser_skip_space(&parser);
  if (status == YAP_V2_OK && parser.offset != parser.len) {
    status = YAP_V2_INVALID_FORMAT;
  }
  if (status == YAP_V2_OK) {
    status = YAP_V2_manifest_validate(&parsed);
  }
  if (status == YAP_V2_OK) {
    YAP_V2_manifest_free(manifest);
    *manifest = parsed;
  } else {
    YAP_V2_manifest_free(&parsed);
  }
  free(data);
  return status;
}

static int write_hex(FILE *fp, const unsigned char checksum[32]) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  if (fputc('"', fp) == EOF) {
    return -1;
  }
  for (i = 0; i < 32U; i++) {
    if (fputc(digits[checksum[i] >> 4], fp) == EOF ||
        fputc(digits[checksum[i] & 0x0fU], fp) == EOF) {
      return -1;
    }
  }
  return fputc('"', fp) == EOF ? -1 : 0;
}

static int write_json(FILE *fp, const YAP_V2_MANIFEST *manifest) {
  size_t i;

  if (fprintf(fp, "{\"format_version\":%u,\"generation\":%" PRIu64
                   ",\"segments\":[",
              manifest->format_version, manifest->generation) < 0) {
    return -1;
  }
  for (i = 0; i < manifest->segment_count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *segment = &manifest->segments[i];
    if (i > 0U && fputc(',', fp) == EOF) {
      return -1;
    }
    if (fprintf(fp, "{\"id\":\"%s\",\"documents\":%" PRIu64
                     ",\"passages\":%" PRIu64 ",\"file_bytes\":%" PRIu64 ",\"checksum\":",
                segment->id, segment->document_count, segment->passage_count,
                segment->file_bytes) < 0 ||
        write_hex(fp, segment->checksum) != 0 || fputc('}', fp) == EOF) {
      return -1;
    }
  }
  return fprintf(fp, "]}") < 0 ? -1 : 0;
}

int YAP_V2_manifest_save_atomic(const char *path, const YAP_V2_MANIFEST *manifest) {
  FILE *fp = NULL;
  char *temporary_path;
  size_t path_len;
  int status = YAP_V2_IO_ERROR;

  if (path == NULL || manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = YAP_V2_manifest_validate(manifest);
  if (status != YAP_V2_OK) {
    return status;
  }
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
  if (write_json(fp, manifest) != 0 || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
    fclose(fp);
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  if (fclose(fp) != 0 || rename(temporary_path, path) != 0) {
    unlink(temporary_path);
    free(temporary_path);
    return YAP_V2_IO_ERROR;
  }
  free(temporary_path);
  return YAP_V2_OK;
}

int YAP_V2_manifest_publish_next(const char *path, YAP_V2_MANIFEST *manifest) {
  YAP_V2_MANIFEST current;
  char *lock_path;
  size_t path_len;
  int lock_fd;
  int status;

  if (path == NULL || manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  path_len = strlen(path);
  if (path_len > SIZE_MAX - 6U) {
    return YAP_V2_OUT_OF_RANGE;
  }
  lock_path = (char *)malloc(path_len + 6U);
  if (lock_path == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  if (snprintf(lock_path, path_len + 6U, "%s.lock", path) < 0) {
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }
  lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0) {
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }
  if (flock(lock_fd, LOCK_EX) != 0) {
    close(lock_fd);
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }

  YAP_V2_manifest_init(&current);
  status = YAP_V2_manifest_load(path, &current);
  if (status == YAP_V2_IO_ERROR) {
    if (access(path, F_OK) != 0 && errno == ENOENT) {
      current.generation = 0U;
      status = YAP_V2_OK;
    }
  }
  if (status == YAP_V2_OK) {
    if (current.generation == UINT64_MAX) {
      status = YAP_V2_OUT_OF_RANGE;
    } else {
      manifest->generation = current.generation + 1U;
      status = YAP_V2_manifest_save_atomic(path, manifest);
    }
  }
  YAP_V2_manifest_free(&current);
  flock(lock_fd, LOCK_UN);
  close(lock_fd);
  free(lock_path);
  return status;
}
