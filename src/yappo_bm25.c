#include "yappo_bm25.h"

#include <math.h>

double YAP_BM25_score(size_t term_frequency, size_t document_frequency,
                      size_t document_count, size_t document_length,
                      double average_document_length, double boost) {
  double idf_numerator;
  double idf_denominator;
  double idf;
  double length_norm;
  double tf_component;
  double denominator;

  if (term_frequency == 0U || document_frequency == 0U || document_count == 0U ||
      document_frequency > document_count) {
    return 0.0;
  }
  if (!isfinite(average_document_length) || average_document_length <= 0.0) {
    average_document_length = 1.0;
  }
  if (!isfinite(boost) || boost <= 0.0) {
    boost = 1.0;
  }
  if (document_length == 0U) {
    document_length = 1U;
  }

  idf_numerator = (double)document_count - (double)document_frequency + 0.5;
  idf_denominator = (double)document_frequency + 0.5;
  if (idf_numerator < 0.0 || idf_denominator <= 0.0) {
    return 0.0;
  }
  idf = log1p(idf_numerator / idf_denominator);
  if (!isfinite(idf) || idf < 0.0) {
    return 0.0;
  }

  length_norm = (1.0 - YAP_BM25_DEFAULT_B) +
                YAP_BM25_DEFAULT_B * ((double)document_length / average_document_length);
  denominator = (double)term_frequency + YAP_BM25_DEFAULT_K1 * length_norm;
  if (!isfinite(length_norm) || length_norm <= 0.0 || !isfinite(denominator) || denominator <= 0.0) {
    return 0.0;
  }

  tf_component = ((double)term_frequency * (YAP_BM25_DEFAULT_K1 + 1.0)) / denominator;
  if (!isfinite(tf_component) || tf_component < 0.0) {
    return 0.0;
  }
  return idf * tf_component * boost;
}
