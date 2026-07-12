#ifndef YAPPO_BM25_H
#define YAPPO_BM25_H

#include <stddef.h>

#define YAP_BM25_DEFAULT_K1 1.2
#define YAP_BM25_DEFAULT_B 0.75

double YAP_BM25_score(size_t term_frequency, size_t document_frequency,
                      size_t document_count, size_t document_length,
                      double average_document_length, double boost);

#endif
