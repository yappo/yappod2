#ifndef YAPPO_SNIPPET_V2_H
#define YAPPO_SNIPPET_V2_H

#include "yappo_index_v2.h"

int YAP_V2_snippet(YAP_V2_BYTES_VIEW text, const YAP_V2_BYTES_VIEW *terms, size_t term_count,
                   size_t max_graphemes, const char *open_mark, const char *close_mark,
                   char *output, size_t output_capacity, size_t *output_bytes);

#endif
