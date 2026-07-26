#include "common/yappo_types_v2.h"

static int bytes_view_validate(YAP_V2_BYTES_VIEW value, size_t max_bytes, int required) {
  size_t i;

  if (required && (value.data == NULL || value.len == 0U)) {
    return YAP_V2_INVALID_FORMAT;
  }
  if (value.len > max_bytes || (value.len > 0U && value.data == NULL)) {
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0; i < value.len; i++) {
    if (value.data[i] == '\0') {
      return YAP_V2_INVALID_FORMAT;
    }
  }
  return YAP_V2_OK;
}

const char *YAP_V2_status_string(YAP_V2_STATUS status) {
  switch (status) {
  case YAP_V2_OK:
    return "ok";
  case YAP_V2_INVALID_ARGUMENT:
    return "invalid argument";
  case YAP_V2_INVALID_FORMAT:
    return "invalid format";
  case YAP_V2_OUT_OF_RANGE:
    return "out of range";
  case YAP_V2_DUPLICATE:
    return "duplicate";
  case YAP_V2_ALLOCATION_FAILED:
    return "allocation failed";
  case YAP_V2_IO_ERROR:
    return "I/O error";
  case YAP_V2_CHECKSUM_MISMATCH:
    return "checksum mismatch";
  case YAP_V2_CONFLICT:
    return "conflict";
  case YAP_V2_NOT_FOUND:
    return "not found";
  case YAP_V2_SEGMENT_CAPACITY_EXCEEDED:
    return "segment capacity exceeded";
  default:
    return "unknown status";
  }
}

int YAP_V2_document_validate(const YAP_V2_DOCUMENT_VIEW *document) {
  int status;

  if (document == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = bytes_view_validate(document->id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->url, YAP_V2_MAX_URL_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->title, YAP_V2_MAX_IDENTIFIER_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->body, YAP_V2_MAX_METADATA_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->metadata_json, YAP_V2_MAX_METADATA_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (document->updated_at_unix_ms < 0) {
    return YAP_V2_OUT_OF_RANGE;
  }
  return YAP_V2_OK;
}

int YAP_V2_passage_validate(const YAP_V2_PASSAGE_VIEW *passage) {
  int status;

  if (passage == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = bytes_view_validate(passage->id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(passage->parent_document_id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(passage->text, YAP_V2_MAX_METADATA_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (passage->end_char < passage->start_char || passage->start_char > YAP_V2_MAX_CHUNK_CHARS ||
      passage->end_char > YAP_V2_MAX_CHUNK_CHARS) {
    return YAP_V2_OUT_OF_RANGE;
  }
  return YAP_V2_OK;
}
