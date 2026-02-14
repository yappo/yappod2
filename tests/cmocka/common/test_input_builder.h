#ifndef YTEST_INPUT_BUILDER_H
#define YTEST_INPUT_BUILDER_H

#include <stddef.h>

int ytest_fixture_copy_with_inject(const char *fixture, const char *out_path, int head_lines,
                                   const char *inject, int tail_from);

int ytest_fixture_build_oversized_burst(const char *fixture, const char *out_path,
                                        size_t huge_payload_len);

int ytest_fixture_rewrite_size_boundary(const char *fixture, const char *out_path);

int ytest_fixture_build_edge_cases(const char *out_path, size_t huge_payload_len);

#endif
