#include "yappo_lexical_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define POSTING_BYTES 48U
#define BLOCK_BYTES 16U

static uint32_t get_u32(const unsigned char *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static uint64_t get_u64(const unsigned char *data) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0U; i < 8U; i++)
    value |= (uint64_t)data[i] << (i * 8U);
  return value;
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

static int range_valid(size_t offset, size_t bytes, size_t size) {
  return offset <= size && bytes <= size - offset;
}

static int map_component(const char *path, uint32_t type, uint64_t expected_generation,
                         void **map_out, size_t *bytes_out, uint64_t *generation_out) {
  struct stat info;
  YAP_V2_FILE_HEADER header;
  unsigned char *map;
  int fd;
  int status;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return YAP_V2_IO_ERROR;
  if (fstat(fd, &info) != 0 || info.st_size < (off_t)YAP_V2_FILE_HEADER_BYTES ||
      (uint64_t)info.st_size > YAP_V2_FILE_HEADER_BYTES + YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES) {
    close(fd);
    return YAP_V2_INVALID_FORMAT;
  }
  map = (unsigned char *)mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (close(fd) != 0 || map == MAP_FAILED) {
    if (map != MAP_FAILED)
      munmap(map, (size_t)info.st_size);
    return YAP_V2_IO_ERROR;
  }
  status = YAP_V2_file_header_decode(map, &header);
  if (status != YAP_V2_OK || header.file_type != type ||
      header.payload_bytes != (uint64_t)info.st_size - YAP_V2_FILE_HEADER_BYTES ||
      (expected_generation != 0U && header.generation != expected_generation)) {
    munmap(map, (size_t)info.st_size);
    return status == YAP_V2_OK ? YAP_V2_INVALID_FORMAT : status;
  }
  if (crc32c(map + YAP_V2_FILE_HEADER_BYTES, (size_t)header.payload_bytes) !=
      header.payload_crc32c) {
    munmap(map, (size_t)info.st_size);
    return YAP_V2_CHECKSUM_MISMATCH;
  }
  *map_out = map;
  *bytes_out = (size_t)info.st_size;
  *generation_out = header.generation;
  return YAP_V2_OK;
}

void YAP_V2_lexical_segment_init(YAP_V2_LEXICAL_SEGMENT *segment) {
  if (segment != NULL)
    memset(segment, 0, sizeof(*segment));
}

void YAP_V2_lexical_segment_close(YAP_V2_LEXICAL_SEGMENT *segment) {
  size_t i;
  if (segment == NULL)
    return;
  for (i = 0U; i < 3U; i++)
    if (segment->maps[i] != NULL)
      munmap(segment->maps[i], segment->map_bytes[i]);
  free(segment->terms);
  YAP_V2_lexical_segment_init(segment);
}

static int parse_posting(const unsigned char *data, YAP_V2_POSTING *posting) {
  posting->object_type = get_u32(data);
  posting->object_ordinal = get_u64(data + 4U);
  posting->term_frequency[0] = get_u32(data + 12U);
  posting->term_frequency[1] = get_u32(data + 16U);
  posting->term_frequency[2] = get_u32(data + 20U);
  posting->field_length[0] = get_u32(data + 24U);
  posting->field_length[1] = get_u32(data + 28U);
  posting->field_length[2] = get_u32(data + 32U);
  posting->position_offset = get_u64(data + 36U);
  posting->position_count = get_u32(data + 44U);
  return YAP_V2_OK;
}

static int validate_term_stream(YAP_V2_LEXICAL_SEGMENT *segment) {
  const unsigned char *data = (const unsigned char *)segment->maps[0];
  size_t size = segment->map_bytes[0];
  size_t offset = YAP_V2_FILE_HEADER_BYTES;
  uint64_t count;
  size_t i;

  if (!range_valid(offset, 12U, size) || get_u32(data + offset) != YAP_V2_LEXICAL_PAYLOAD_VERSION)
    return YAP_V2_INVALID_FORMAT;
  count = get_u64(data + offset + 4U);
  if (count > SIZE_MAX || count > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES / sizeof(*segment->terms))
    return YAP_V2_OUT_OF_RANGE;
  segment->terms =
    count == 0U ? NULL : (YAP_V2_TERM_ENTRY *)calloc((size_t)count, sizeof(*segment->terms));
  if (count > 0U && segment->terms == NULL)
    return YAP_V2_ALLOCATION_FAILED;
  segment->term_count = (size_t)count;
  offset += 12U;
  for (i = 0U; i < segment->term_count; i++) {
    uint32_t length;
    YAP_V2_TERM_ENTRY *term = &segment->terms[i];
    if (!range_valid(offset, 4U, size))
      return YAP_V2_INVALID_FORMAT;
    length = get_u32(data + offset);
    offset += 4U;
    if (length == 0U || !range_valid(offset, (size_t)length + 40U, size))
      return YAP_V2_INVALID_FORMAT;
    term->term.data = data + offset;
    term->term.len = length;
    offset += length;
    term->document_frequency = get_u64(data + offset);
    term->postings_offset = get_u64(data + offset + 8U);
    term->postings_bytes = get_u64(data + offset + 16U);
    term->positions_offset = get_u64(data + offset + 24U);
    term->positions_bytes = get_u64(data + offset + 32U);
    offset += 40U;
    if (term->document_frequency == 0U ||
        (i > 0U &&
         (segment->terms[i - 1U].term.len > term->term.len
            ? memcmp(segment->terms[i - 1U].term.data, term->term.data, term->term.len) >= 0
            : memcmp(segment->terms[i - 1U].term.data, term->term.data,
                     segment->terms[i - 1U].term.len) >= 0)))
      return YAP_V2_INVALID_FORMAT;
  }
  return offset == size ? YAP_V2_OK : YAP_V2_INVALID_FORMAT;
}

static int validate_payloads(YAP_V2_LEXICAL_SEGMENT *segment) {
  const unsigned char *postings = (const unsigned char *)segment->maps[1];
  const unsigned char *positions = (const unsigned char *)segment->maps[2];
  size_t postings_size = segment->map_bytes[1];
  size_t positions_size = segment->map_bytes[2];
  size_t posting_cursor = YAP_V2_FILE_HEADER_BYTES + 56U;
  size_t position_cursor = YAP_V2_FILE_HEADER_BYTES + 12U;
  uint64_t counted_postings = 0U;
  uint64_t counted_positions = 0U;
  size_t term_index;

  if (!range_valid(YAP_V2_FILE_HEADER_BYTES, 56U, postings_size) ||
      get_u32(postings + YAP_V2_FILE_HEADER_BYTES) != YAP_V2_LEXICAL_PAYLOAD_VERSION ||
      get_u32(postings + YAP_V2_FILE_HEADER_BYTES + 4U) != YAP_V2_POSTINGS_BLOCK_SIZE ||
      !range_valid(YAP_V2_FILE_HEADER_BYTES, 12U, positions_size) ||
      get_u32(positions + YAP_V2_FILE_HEADER_BYTES) != YAP_V2_LEXICAL_PAYLOAD_VERSION)
    return YAP_V2_INVALID_FORMAT;
  segment->document_count = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 8U);
  segment->passage_count = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 16U);
  segment->posting_count = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 24U);
  segment->field_token_count[0] = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 32U);
  segment->field_token_count[1] = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 40U);
  segment->field_token_count[2] = get_u64(postings + YAP_V2_FILE_HEADER_BYTES + 48U);
  segment->position_count = get_u64(positions + YAP_V2_FILE_HEADER_BYTES + 4U);

  for (term_index = 0U; term_index < segment->term_count; term_index++) {
    const YAP_V2_TERM_ENTRY *term = &segment->terms[term_index];
    uint64_t position_records;
    uint32_t block_count;
    size_t i;
    YAP_V2_POSTING previous = {0};
    size_t posting_data;
    size_t block_data;

    if (term->postings_offset > SIZE_MAX || term->postings_bytes > SIZE_MAX ||
        term->positions_offset > SIZE_MAX || term->positions_bytes > SIZE_MAX ||
        term->postings_offset != posting_cursor - YAP_V2_FILE_HEADER_BYTES ||
        term->positions_offset != position_cursor - YAP_V2_FILE_HEADER_BYTES ||
        term->document_frequency > SIZE_MAX ||
        !range_valid(posting_cursor, (size_t)term->postings_bytes, postings_size) ||
        !range_valid(position_cursor, (size_t)term->positions_bytes, positions_size) ||
        !range_valid(posting_cursor, 20U, postings_size) ||
        get_u64(postings + posting_cursor) != term_index ||
        get_u64(postings + posting_cursor + 8U) != term->document_frequency ||
        !range_valid(position_cursor, 16U, positions_size) ||
        get_u64(positions + position_cursor) != term_index)
      return YAP_V2_INVALID_FORMAT;
    block_count = get_u32(postings + posting_cursor + 16U);
    position_records = get_u64(positions + position_cursor + 8U);
    if (block_count != (term->document_frequency + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                         YAP_V2_POSTINGS_BLOCK_SIZE ||
        term->document_frequency > (SIZE_MAX - 20U) / POSTING_BYTES)
      return YAP_V2_INVALID_FORMAT;
    posting_data = posting_cursor + 20U;
    block_data = posting_data + (size_t)term->document_frequency * POSTING_BYTES;
    if ((uint64_t)(20U + (size_t)term->document_frequency * POSTING_BYTES +
                   (size_t)block_count * BLOCK_BYTES) != term->postings_bytes ||
        position_records > (SIZE_MAX - 16U) / 8U ||
        term->positions_bytes != 16U + position_records * 8U)
      return YAP_V2_INVALID_FORMAT;
    for (i = 0U; i < (size_t)term->document_frequency; i++) {
      YAP_V2_POSTING posting;
      uint64_t tf;
      size_t p;
      uint32_t field_counts[3] = {0U, 0U, 0U};
      uint32_t previous_position[3] = {0U, 0U, 0U};
      parse_posting(postings + posting_data + i * POSTING_BYTES, &posting);
      tf =
        (uint64_t)posting.term_frequency[0] + posting.term_frequency[1] + posting.term_frequency[2];
      if ((posting.object_type != YAP_V2_LEXICAL_DOCUMENT &&
           posting.object_type != YAP_V2_LEXICAL_PASSAGE) ||
          (posting.object_type == YAP_V2_LEXICAL_DOCUMENT &&
           posting.object_ordinal >= segment->document_count) ||
          (posting.object_type == YAP_V2_LEXICAL_PASSAGE &&
           posting.object_ordinal >= segment->passage_count) ||
          tf == 0U || tf != posting.position_count ||
          posting.position_offset + posting.position_count > position_records ||
          (i > 0U && (posting.object_type < previous.object_type ||
                      (posting.object_type == previous.object_type &&
                       posting.object_ordinal <= previous.object_ordinal))))
        return YAP_V2_INVALID_FORMAT;
      for (p = 0U; p < posting.position_count; p++) {
        size_t at = position_cursor + 16U + ((size_t)posting.position_offset + p) * 8U;
        uint32_t field = get_u32(positions + at);
        uint32_t position = get_u32(positions + at + 4U);
        if (field < YAP_V2_FIELD_TITLE || field > YAP_V2_FIELD_PASSAGE ||
            position >= posting.field_length[field - 1U] ||
            (field_counts[field - 1U] > 0U && position <= previous_position[field - 1U]))
          return YAP_V2_INVALID_FORMAT;
        previous_position[field - 1U] = position;
        field_counts[field - 1U]++;
      }
      if (field_counts[0] != posting.term_frequency[0] ||
          field_counts[1] != posting.term_frequency[1] ||
          field_counts[2] != posting.term_frequency[2])
        return YAP_V2_INVALID_FORMAT;
      previous = posting;
    }
    for (i = 0U; i < block_count; i++) {
      uint32_t first = get_u32(postings + block_data + i * BLOCK_BYTES);
      uint32_t count = get_u32(postings + block_data + i * BLOCK_BYTES + 4U);
      uint32_t stored_max_tf = get_u32(postings + block_data + i * BLOCK_BYTES + 8U);
      uint32_t stored_min_length = get_u32(postings + block_data + i * BLOCK_BYTES + 12U);
      uint32_t max_tf = 0U;
      uint32_t min_length = UINT32_MAX;
      size_t posting_index;
      if (first != i * YAP_V2_POSTINGS_BLOCK_SIZE || count == 0U ||
          count > YAP_V2_POSTINGS_BLOCK_SIZE || first + count > term->document_frequency)
        return YAP_V2_INVALID_FORMAT;
      for (posting_index = first; posting_index < (size_t)first + count; posting_index++) {
        YAP_V2_POSTING posting;
        uint32_t tf;
        uint32_t length;
        parse_posting(postings + posting_data + posting_index * POSTING_BYTES, &posting);
        tf = posting.term_frequency[0] + posting.term_frequency[1] + posting.term_frequency[2];
        length = UINT32_MAX;
        if (posting.term_frequency[0] > 0U && posting.field_length[0] < length)
          length = posting.field_length[0];
        if (posting.term_frequency[1] > 0U && posting.field_length[1] < length)
          length = posting.field_length[1];
        if (posting.term_frequency[2] > 0U && posting.field_length[2] < length)
          length = posting.field_length[2];
        if (tf > max_tf)
          max_tf = tf;
        if (length < min_length)
          min_length = length;
      }
      if (stored_max_tf != max_tf || stored_min_length != min_length)
        return YAP_V2_INVALID_FORMAT;
    }
    counted_postings += term->document_frequency;
    counted_positions += position_records;
    posting_cursor += (size_t)term->postings_bytes;
    position_cursor += (size_t)term->positions_bytes;
  }
  return posting_cursor == postings_size && position_cursor == positions_size &&
             counted_postings == segment->posting_count &&
             counted_positions == segment->position_count
           ? YAP_V2_OK
           : YAP_V2_INVALID_FORMAT;
}

int YAP_V2_lexical_segment_open(const char *segment_dir, uint64_t expected_generation,
                                YAP_V2_LEXICAL_SEGMENT *segment) {
  static const char *const names[] = {"terms.yap2", "postings.yap2", "positions.yap2"};
  static const uint32_t types[] = {YAP_V2_FILE_TERMS, YAP_V2_FILE_POSTINGS, YAP_V2_FILE_POSITIONS};
  uint64_t generation = 0U;
  size_t i;
  int status = YAP_V2_OK;
  if (segment_dir == NULL || segment == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_lexical_segment_close(segment);
  for (i = 0U; status == YAP_V2_OK && i < 3U; i++) {
    size_t length = strlen(segment_dir) + strlen(names[i]) + 2U;
    char *path = (char *)malloc(length);
    uint64_t file_generation;
    if (path == NULL) {
      status = YAP_V2_ALLOCATION_FAILED;
      break;
    }
    (void)snprintf(path, length, "%s/%s", segment_dir, names[i]);
    status = map_component(path, types[i], expected_generation, &segment->maps[i],
                           &segment->map_bytes[i], &file_generation);
    free(path);
    if (status == YAP_V2_OK && i > 0U && file_generation != generation)
      status = YAP_V2_INVALID_FORMAT;
    if (status == YAP_V2_OK)
      generation = file_generation;
  }
  if (status == YAP_V2_OK)
    status = validate_term_stream(segment);
  if (status == YAP_V2_OK)
    status = validate_payloads(segment);
  if (status != YAP_V2_OK) {
    YAP_V2_lexical_segment_close(segment);
    return status;
  }
  segment->generation = generation;
  return YAP_V2_OK;
}

const YAP_V2_TERM_ENTRY *YAP_V2_lexical_term_find(const YAP_V2_LEXICAL_SEGMENT *segment,
                                                  YAP_V2_BYTES_VIEW term) {
  size_t low = 0U, high;
  if (segment == NULL || (term.len > 0U && term.data == NULL))
    return NULL;
  high = segment->term_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2U;
    const YAP_V2_TERM_ENTRY *entry = &segment->terms[middle];
    size_t common = entry->term.len < term.len ? entry->term.len : term.len;
    int order = memcmp(entry->term.data, term.data, common);
    if (order == 0 && entry->term.len == term.len)
      return entry;
    if (order < 0 || (order == 0 && entry->term.len < term.len))
      low = middle + 1U;
    else
      high = middle;
  }
  return NULL;
}

int YAP_V2_posting_iterator_init(const YAP_V2_LEXICAL_SEGMENT *segment,
                                 const YAP_V2_TERM_ENTRY *term, YAP_V2_POSTING_ITERATOR *iterator) {
  if (segment == NULL || term == NULL || iterator == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  memset(iterator, 0, sizeof(*iterator));
  iterator->segment = segment;
  iterator->term = term;
  iterator->offset = YAP_V2_FILE_HEADER_BYTES + (size_t)term->postings_offset + 20U;
  iterator->blocks_offset = iterator->offset + (size_t)term->document_frequency * POSTING_BYTES;
  return YAP_V2_OK;
}

int YAP_V2_posting_iterator_next(YAP_V2_POSTING_ITERATOR *iterator, YAP_V2_POSTING *posting) {
  const unsigned char *data;
  if (iterator == NULL || posting == NULL || iterator->term == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (iterator->index >= iterator->term->document_frequency)
    return YAP_V2_OUT_OF_RANGE;
  data = (const unsigned char *)iterator->segment->maps[1];
  parse_posting(data + iterator->offset, posting);
  iterator->offset += POSTING_BYTES;
  iterator->index++;
  return YAP_V2_OK;
}

int YAP_V2_posting_iterator_block(const YAP_V2_POSTING_ITERATOR *iterator, size_t block_index,
                                  YAP_V2_POSTINGS_BLOCK *block) {
  const unsigned char *data;
  size_t count;
  if (iterator == NULL || block == NULL || iterator->term == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  count = ((size_t)iterator->term->document_frequency + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
          YAP_V2_POSTINGS_BLOCK_SIZE;
  if (block_index >= count)
    return YAP_V2_OUT_OF_RANGE;
  data = (const unsigned char *)iterator->segment->maps[1] + iterator->blocks_offset +
         block_index * BLOCK_BYTES;
  block->first_posting = get_u32(data);
  block->posting_count = get_u32(data + 4U);
  block->max_term_frequency = get_u32(data + 8U);
  block->min_field_length = get_u32(data + 12U);
  return YAP_V2_OK;
}

int YAP_V2_position_iterator_init(const YAP_V2_LEXICAL_SEGMENT *segment,
                                  const YAP_V2_TERM_ENTRY *term,
                                  YAP_V2_POSITION_ITERATOR *iterator) {
  if (segment == NULL || term == NULL || iterator == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  memset(iterator, 0, sizeof(*iterator));
  iterator->segment = segment;
  iterator->term = term;
  iterator->offset = YAP_V2_FILE_HEADER_BYTES + (size_t)term->positions_offset + 16U;
  return YAP_V2_OK;
}

int YAP_V2_position_iterator_next(YAP_V2_POSITION_ITERATOR *iterator, YAP_V2_POSITION *position) {
  const unsigned char *data;
  size_t count;
  if (iterator == NULL || position == NULL || iterator->term == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  count = ((size_t)iterator->term->positions_bytes - 16U) / 8U;
  if (iterator->index >= count)
    return YAP_V2_OUT_OF_RANGE;
  data = (const unsigned char *)iterator->segment->maps[2] + iterator->offset;
  position->field = get_u32(data);
  position->position = get_u32(data + 4U);
  iterator->offset += 8U;
  iterator->index++;
  return YAP_V2_OK;
}

int YAP_V2_posting_position_at(const YAP_V2_LEXICAL_SEGMENT *segment, const YAP_V2_TERM_ENTRY *term,
                               const YAP_V2_POSTING *posting, size_t index,
                               YAP_V2_POSITION *position) {
  const unsigned char *data;
  size_t offset;
  if (segment == NULL || term == NULL || posting == NULL || position == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (index >= posting->position_count || term->positions_offset > SIZE_MAX ||
      posting->position_offset > SIZE_MAX - index)
    return YAP_V2_OUT_OF_RANGE;
  offset = YAP_V2_FILE_HEADER_BYTES + (size_t)term->positions_offset + 16U +
           ((size_t)posting->position_offset + index) * 8U;
  if (!range_valid(offset, 8U, segment->map_bytes[2]))
    return YAP_V2_INVALID_FORMAT;
  data = (const unsigned char *)segment->maps[2] + offset;
  position->field = get_u32(data);
  position->position = get_u32(data + 4U);
  return YAP_V2_OK;
}
