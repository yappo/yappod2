#ifndef YAPPO_LIMITS_H
#define YAPPO_LIMITS_H

/* Per-record decode guards for corrupted index payloads. */
#define YAP_MAX_POSTINGS_BLOB_SIZE (16 * 1024 * 1024)
#define YAP_MAX_FILEDATA_RECORD_SIZE (16 * 1024 * 1024)

/* Query-time aggregation guard for postings bytes. */
#define YAP_DEFAULT_MAX_POSTINGS_QUERY_BYTES (64 * 1024 * 1024)

#endif
