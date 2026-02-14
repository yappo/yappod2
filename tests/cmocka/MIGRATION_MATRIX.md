# Migration Matrix (Shell -> cmocka)

This matrix documents the 1:1 migration from the previous shell-based tests to the new cmocka suites.

| Legacy ctest name | Legacy file | New cmocka source | New case IDs |
|---|---|---|---|
| `e2e` | `tests/e2e.sh` | `tests/cmocka/suites/e2e_test.c` | `E2E-01` (single suite with all former assertions) |
| `error_cases` | `tests/error_cases.sh` | `tests/cmocka/suites/error_cases_test.c` | `ERR-01` .. `ERR-15` |
| `front_http_error_cases` | `tests/front_http_error_cases.sh` | `tests/cmocka/suites/front_http_error_cases_test.c` | `FHTTP-01` .. `FHTTP-12` |
| `makeindex_input_error_cases` | `tests/makeindex_input_error_cases.sh` | `tests/cmocka/suites/makeindex_input_error_cases_test.c` | `MINPUT-01` .. `MINPUT-05` |
| `mergepos_error_cases` | `tests/mergepos_error_cases.sh` | `tests/cmocka/suites/mergepos_error_cases_test.c` | `MPOS-01` .. `MPOS-10` |
| `opendir_error_cases` | `tests/opendir_error_cases.sh` | `tests/cmocka/suites/opendir_error_cases_test.c` | `OPDIR-01` |
| `relative_index_dir_cases` | `tests/relative_index_dir_cases.sh` | `tests/cmocka/suites/relative_index_dir_cases_test.c` | `RDIR-01` |
| `search_deletefile_bounds` | `tests/search_deletefile_bounds_test.c` | `tests/search_deletefile_bounds_test.c` | `DELB-01` .. `DELB-03` |
| `filedata_decode_bounds` | `tests/filedata_decode_bounds_test.c` | `tests/filedata_decode_bounds_test.c` | `FDB-01` .. `FDB-05` |
| `search_deletefile_concurrency` | `tests/search_deletefile_concurrency_test.c` | `tests/search_deletefile_concurrency_test.c` | `DELC-01` |
| `index_varint_decode` | `tests/index_varint_decode_test.c` | `tests/index_varint_decode_test.c` | `VARINT-01` .. `VARINT-03` |
| `seek_offset_overflow` | `tests/seek_offset_overflow_test.c` | `tests/seek_offset_overflow_test.c` | `SEEK-01` .. `SEEK-03` |
| `pos_index_bounds` | `tests/pos_index_bounds_test.c` | `tests/pos_index_bounds_test.c` | `POS-01` .. `POS-05` |

## Post-migration extensions

| ctest name | cmocka source | New case IDs |
|---|---|---|
| `daemon_port_option_cases` | `tests/cmocka/suites/daemon_port_option_cases_test.c` | `DPORT-01` .. `DPORT-04` |

## Coverage check

- All 13 legacy ctest entries are preserved with the same ctest names.
- Legacy shell scenarios are implemented in cmocka and grouped as deterministic case IDs.
- No legacy scenario is intentionally dropped.
- Additional daemon port validation cases were added after migration.
