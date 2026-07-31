#include "query/yappo_lexical_search_v2.h"

#include "query/yappo_bm25.h"
#include "common/yappo_unicode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const YAP_V2_TERM_ENTRY *term;
  YAP_V2_POSTING *postings;
  size_t count;
  size_t index;
  size_t type_frequency[2];
  YAP_V2_POSTING_ITERATOR blocks;
} TERM_STATE;

static uint64_t posting_key(const YAP_V2_POSTING *posting) {
  return ((uint64_t)(posting->object_type - 1U) << 63) | posting->object_ordinal;
}

static int hit_compare(const void *left, const void *right) {
  const YAP_V2_LEXICAL_HIT *a = (const YAP_V2_LEXICAL_HIT *)left;
  const YAP_V2_LEXICAL_HIT *b = (const YAP_V2_LEXICAL_HIT *)right;
  if (a->score != b->score)
    return a->score > b->score ? -1 : 1;
  if (a->object_type != b->object_type)
    return a->object_type < b->object_type ? -1 : 1;
  if (a->object_ordinal != b->object_ordinal)
    return a->object_ordinal < b->object_ordinal ? -1 : 1;
  return 0;
}

static int state_pointer_compare(const void *left, const void *right) {
  const TERM_STATE *a = *(TERM_STATE *const *)left;
  const TERM_STATE *b = *(TERM_STATE *const *)right;
  uint64_t a_key = posting_key(&a->postings[a->index]);
  uint64_t b_key = posting_key(&b->postings[b->index]);
  return a_key < b_key ? -1 : a_key > b_key ? 1 : 0;
}

static double average_length(const YAP_V2_LEXICAL_SEGMENT *segment, uint32_t object_type,
                             size_t field) {
  uint64_t count = field == 2U ? segment->passage_count : segment->document_count;
  if ((field == 2U && object_type != YAP_V2_LEXICAL_PASSAGE) ||
      (field != 2U && object_type != YAP_V2_LEXICAL_DOCUMENT) || count == 0U)
    return 1.0;
  return segment->field_token_count[field] == 0U
           ? 1.0
           : (double)segment->field_token_count[field] / (double)count;
}

static double idf_value(size_t frequency, size_t count) {
  if (frequency == 0U || count == 0U || frequency > count)
    return 0.0;
  return log1p(((double)count - (double)frequency + 0.5) / ((double)frequency + 0.5));
}

static double posting_score(const YAP_V2_LEXICAL_SEGMENT *segment, const TERM_STATE *state,
                            const YAP_V2_POSTING *posting,
                            const YAP_V2_LEXICAL_SEARCH_OPTIONS *options) {
  double weighted_tf = 0.0;
  size_t object_count = posting->object_type == YAP_V2_LEXICAL_DOCUMENT
                          ? (size_t)segment->document_count
                          : (size_t)segment->passage_count;
  size_t field;
  for (field = 0U; field < 3U; field++) {
    double average;
    double norm;
    if (posting->term_frequency[field] == 0U)
      continue;
    average = average_length(segment, posting->object_type, field);
    norm = (1.0 - YAP_BM25_DEFAULT_B) +
           YAP_BM25_DEFAULT_B * ((double)posting->field_length[field] / average);
    weighted_tf += options->field_boost[field] * (double)posting->term_frequency[field] / norm;
  }
  if (weighted_tf <= 0.0)
    return 0.0;
  return idf_value(state->type_frequency[posting->object_type - 1U], object_count) *
         (weighted_tf * (YAP_BM25_DEFAULT_K1 + 1.0)) / (YAP_BM25_DEFAULT_K1 + weighted_tf);
}

static double block_upper_bound(const YAP_V2_LEXICAL_SEGMENT *segment, const TERM_STATE *state,
                                const YAP_V2_LEXICAL_SEARCH_OPTIONS *options) {
  YAP_V2_POSTINGS_BLOCK block;
  size_t block_index = state->index / YAP_V2_POSTINGS_BLOCK_SIZE;
  double best_score = 0.0;
  uint32_t object_type;
  if (YAP_V2_posting_iterator_block(&state->blocks, block_index, &block) != YAP_V2_OK)
    return 0.0;
  for (object_type = YAP_V2_LEXICAL_DOCUMENT; object_type <= YAP_V2_LEXICAL_PASSAGE;
       object_type++) {
    size_t object_count = object_type == YAP_V2_LEXICAL_DOCUMENT ? (size_t)segment->document_count
                                                                 : (size_t)segment->passage_count;
    double best_weight = 0.0;
    double weighted_tf;
    double score;
    size_t field;
    if (options->object_type != 0U && options->object_type != object_type)
      continue;
    for (field = 0U; field < 3U; field++) {
      double average = average_length(segment, object_type, field);
      double norm = (1.0 - YAP_BM25_DEFAULT_B) +
                    YAP_BM25_DEFAULT_B * ((double)block.min_field_length / average);
      double weight = options->field_boost[field] / norm;
      if (weight > best_weight)
        best_weight = weight;
    }
    weighted_tf = best_weight * block.max_term_frequency;
    score = idf_value(state->type_frequency[object_type - 1U], object_count) *
            (weighted_tf * (YAP_BM25_DEFAULT_K1 + 1.0)) / (YAP_BM25_DEFAULT_K1 + weighted_tf);
    if (score > best_score)
      best_score = score;
  }
  return best_score;
}

static void state_skip_type(TERM_STATE *state, uint32_t object_type) {
  while (state->index < state->count && object_type != 0U &&
         state->postings[state->index].object_type != object_type)
    state->index++;
}

static void state_advance_to(TERM_STATE *state, uint64_t key, uint32_t object_type) {
  do {
    state->index++;
    state_skip_type(state, object_type);
  } while (state->index < state->count && posting_key(&state->postings[state->index]) < key);
}

static int phrase_matches(const YAP_V2_LEXICAL_SEGMENT *segment, TERM_STATE *states,
                          const YAP_V2_LEXICAL_QUERY_PLAN *plan, uint64_t key) {
  const YAP_V2_POSTING *first = NULL;
  size_t first_state = plan->token_terms[0];
  size_t i;
  for (i = 0U; i < plan->tokens.token_count; i++) {
    TERM_STATE *state = &states[plan->token_terms[i]];
    if (state->index >= state->count || posting_key(&state->postings[state->index]) != key)
      return 0;
  }
  first = &states[first_state].postings[states[first_state].index];
  for (i = 0U; i < first->position_count; i++) {
    YAP_V2_POSITION base;
    size_t q;
    int matched = 1;
    if (YAP_V2_posting_position_at(segment, states[first_state].term, first, i, &base) != YAP_V2_OK)
      return 0;
    for (q = 1U; q < plan->tokens.token_count && matched; q++) {
      TERM_STATE *state = &states[plan->token_terms[q]];
      const YAP_V2_POSTING *posting = &state->postings[state->index];
      size_t p;
      matched = 0;
      for (p = 0U; p < posting->position_count; p++) {
        YAP_V2_POSITION position;
        if (YAP_V2_posting_position_at(segment, state->term, posting, p, &position) != YAP_V2_OK)
          return 0;
        if (position.field == base.field && position.position == base.position + q) {
          matched = 1;
          break;
        }
      }
    }
    if (matched)
      return 1;
  }
  return 0;
}

static double hit_threshold(const YAP_V2_LEXICAL_HIT *hits, size_t count, size_t top_k) {
  size_t i;
  double minimum;
  if (count < top_k)
    return 0.0;
  minimum = hits[0].score;
  for (i = 1U; i < count; i++)
    if (hits[i].score < minimum)
      minimum = hits[i].score;
  return minimum;
}

static void add_hit(YAP_V2_LEXICAL_HIT *hits, size_t *count, size_t top_k,
                    const YAP_V2_LEXICAL_HIT *hit) {
  size_t i;
  size_t worst = 0U;
  if (*count < top_k) {
    hits[(*count)++] = *hit;
    return;
  }
  for (i = 1U; i < *count; i++)
    if (hit_compare(&hits[i], &hits[worst]) > 0)
      worst = i;
  if (hit_compare(hit, &hits[worst]) < 0)
    hits[worst] = *hit;
}

void YAP_V2_lexical_search_options_init(YAP_V2_LEXICAL_SEARCH_OPTIONS *options) {
  if (options == NULL)
    return;
  memset(options, 0, sizeof(*options));
  options->query_operator = YAP_V2_QUERY_OR;
  options->field_boost[0] = 2.0;
  options->field_boost[1] = 1.0;
  options->field_boost[2] = 1.0;
  options->top_k = 20U;
}

void YAP_V2_lexical_query_plan_init(YAP_V2_LEXICAL_QUERY_PLAN *plan) {
  if (plan != NULL)
    memset(plan, 0, sizeof(*plan));
}

void YAP_V2_lexical_query_plan_free(YAP_V2_LEXICAL_QUERY_PLAN *plan) {
  if (plan == NULL)
    return;
  free(plan->terms);
  free(plan->token_terms);
  YAP_V2_token_sequence_free(&plan->tokens);
  memset(plan, 0, sizeof(*plan));
}

int YAP_V2_lexical_query_plan_prepare(YAP_V2_BYTES_VIEW query,
                                      YAP_V2_LEXICAL_QUERY_PLAN *plan) {
  size_t i;
  int status;
  if (plan == NULL || query.data == NULL || query.len == 0U)
    return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_lexical_query_plan_free(plan);
  status = YAP_V2_unicode_tokenize((const char *)query.data, query.len, &plan->tokens);
  if (status != YAP_V2_OK) {
    YAP_V2_lexical_query_plan_free(plan);
    return status;
  }
  if (plan->tokens.token_count == 0U)
    return YAP_V2_OK;
  plan->terms = (YAP_V2_BYTES_VIEW *)calloc(plan->tokens.token_count,
                                            sizeof(*plan->terms));
  plan->token_terms = (size_t *)calloc(plan->tokens.token_count,
                                       sizeof(*plan->token_terms));
  if (plan->terms == NULL || plan->token_terms == NULL) {
    YAP_V2_lexical_query_plan_free(plan);
    return YAP_V2_ALLOCATION_FAILED;
  }
  for (i = 0U; i < plan->tokens.token_count; i++) {
    YAP_V2_BYTES_VIEW value;
    size_t existing;
    value.data = (const unsigned char *)plan->tokens.normalized_utf8 +
                 plan->tokens.tokens[i].byte_start;
    value.len = plan->tokens.tokens[i].byte_end - plan->tokens.tokens[i].byte_start;
    for (existing = 0U; existing < plan->term_count; existing++)
      if (plan->terms[existing].len == value.len &&
          memcmp(plan->terms[existing].data, value.data, value.len) == 0)
        break;
    if (existing == plan->term_count)
      plan->terms[plan->term_count++] = value;
    plan->token_terms[i] = existing;
  }
  return YAP_V2_OK;
}

static int query_plan_valid(const YAP_V2_LEXICAL_QUERY_PLAN *plan) {
  size_t i;
  if (plan->tokens.token_count == 0U)
    return plan->term_count == 0U;
  if (plan->terms == NULL || plan->token_terms == NULL || plan->term_count == 0U ||
      plan->term_count > plan->tokens.token_count)
    return 0;
  for (i = 0U; i < plan->term_count; i++)
    if (plan->terms[i].data == NULL || plan->terms[i].len == 0U)
      return 0;
  for (i = 0U; i < plan->tokens.token_count; i++)
    if (plan->token_terms[i] >= plan->term_count)
      return 0;
  return 1;
}

static void states_free(TERM_STATE *states, size_t count) {
  size_t i;
  for (i = 0U; i < count; i++)
    free(states[i].postings);
  free(states);
}

int YAP_V2_lexical_search_prepared(const YAP_V2_LEXICAL_SEGMENT *segment,
                                   const YAP_V2_LEXICAL_QUERY_PLAN *plan,
                                   const YAP_V2_LEXICAL_SEARCH_OPTIONS *options,
                                   YAP_V2_LEXICAL_HIT *hits, size_t hit_capacity,
                                   size_t *hit_count) {
  TERM_STATE *states = NULL;
  TERM_STATE **active = NULL;
  size_t state_count = 0U, result_count = 0U, i;
  int status;
  if (segment == NULL || plan == NULL || !query_plan_valid(plan) || options == NULL ||
      hit_count == NULL ||
      options->top_k == 0U || options->top_k > hit_capacity || hits == NULL ||
      (options->object_type != 0U && options->object_type != YAP_V2_LEXICAL_DOCUMENT &&
       options->object_type != YAP_V2_LEXICAL_PASSAGE) ||
      (options->query_operator != YAP_V2_QUERY_OR && options->query_operator != YAP_V2_QUERY_AND))
    return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < 3U; i++)
    if (!isfinite(options->field_boost[i]) || options->field_boost[i] < 0.0)
      return YAP_V2_INVALID_ARGUMENT;
  *hit_count = 0U;
  if (plan->tokens.token_count == 0U)
    return YAP_V2_OK;
  states = (TERM_STATE *)calloc(plan->term_count, sizeof(*states));
  active = (TERM_STATE **)calloc(plan->term_count, sizeof(*active));
  if (states == NULL || active == NULL) {
    status = YAP_V2_ALLOCATION_FAILED;
    goto done;
  }
  for (i = 0U; i < plan->term_count; i++) {
    const YAP_V2_TERM_ENTRY *term;
    size_t existing;
    term = YAP_V2_lexical_term_find(segment, plan->terms[i]);
    if (term == NULL) {
      if (options->query_operator == YAP_V2_QUERY_AND || options->phrase) {
        status = YAP_V2_OK;
        goto done;
      }
      continue;
    }
    states[i].term = term;
    states[i].count = (size_t)term->document_frequency;
    states[i].postings = (YAP_V2_POSTING *)calloc(states[i].count,
                                                  sizeof(YAP_V2_POSTING));
    if (states[i].postings == NULL) {
      status = YAP_V2_ALLOCATION_FAILED;
      goto done;
    }
    YAP_V2_posting_iterator_init(segment, term, &states[i].blocks);
    for (existing = 0U; existing < states[i].count; existing++) {
      YAP_V2_posting_iterator_next(&states[i].blocks, &states[i].postings[existing]);
      states[i].type_frequency[states[i].postings[existing].object_type - 1U]++;
    }
    states[i].blocks.index = 0U;
    state_count++;
  }
  if (state_count == 0U) {
    status = YAP_V2_OK;
    goto done;
  }
  for (i = 0U; i < plan->term_count; i++)
    if (states[i].term != NULL)
      state_skip_type(&states[i], options->object_type);

  while (1) {
    size_t active_count = 0U;
    size_t pivot = 0U;
    double bound = 0.0;
    double threshold = hit_threshold(hits, result_count, options->top_k);
    uint64_t pivot_key;
    for (i = 0U; i < plan->term_count; i++)
      if (states[i].term != NULL && states[i].index < states[i].count)
        active[active_count++] = &states[i];
    if (active_count == 0U || ((options->query_operator == YAP_V2_QUERY_AND || options->phrase) &&
                               active_count < state_count))
      break;
    qsort(active, active_count, sizeof(*active), state_pointer_compare);
    if (options->query_operator == YAP_V2_QUERY_AND || options->phrase) {
      pivot_key =
        posting_key(&active[active_count - 1U]->postings[active[active_count - 1U]->index]);
      if (posting_key(&active[0]->postings[active[0]->index]) != pivot_key) {
        for (i = 0U; i < active_count - 1U; i++)
          if (posting_key(&active[i]->postings[active[i]->index]) < pivot_key)
            state_advance_to(active[i], pivot_key, options->object_type);
        continue;
      }
      pivot = active_count - 1U;
    } else {
      for (pivot = 0U; pivot < active_count; pivot++) {
        bound += block_upper_bound(segment, active[pivot], options);
        if (bound >= threshold && bound > 0.0)
          break;
      }
      if (pivot == active_count) {
        TERM_STATE *state = active[0];
        state->index++;
        state_skip_type(state, options->object_type);
        continue;
      }
      pivot_key = posting_key(&active[pivot]->postings[active[pivot]->index]);
      if (posting_key(&active[0]->postings[active[0]->index]) != pivot_key) {
        for (i = 0U; i < pivot; i++)
          state_advance_to(active[i], pivot_key, options->object_type);
        continue;
      }
    }
    {
      YAP_V2_LEXICAL_HIT hit;
      int phrase_ok = !options->phrase || phrase_matches(segment, states, plan, pivot_key);
      memset(&hit, 0, sizeof(hit));
      hit.object_type = active[0]->postings[active[0]->index].object_type;
      hit.object_ordinal = active[0]->postings[active[0]->index].object_ordinal;
      for (i = 0U; i < active_count; i++) {
        if (posting_key(&active[i]->postings[active[i]->index]) == pivot_key) {
          hit.score +=
            posting_score(segment, active[i], &active[i]->postings[active[i]->index], options);
          hit.matched_terms++;
          state_advance_to(active[i], pivot_key + 1U, options->object_type);
        }
      }
      if (phrase_ok &&
          (options->query_operator == YAP_V2_QUERY_OR || hit.matched_terms == state_count) &&
          (options->accept == NULL ||
           options->accept(options->accept_context, hit.object_type, hit.object_ordinal)) &&
          hit.score > 0.0)
        add_hit(hits, &result_count, options->top_k, &hit);
    }
  }
  qsort(hits, result_count, sizeof(*hits), hit_compare);
  *hit_count = result_count;
  status = YAP_V2_OK;
done:
  free(active);
  states_free(states, plan->term_count);
  return status;
}

int YAP_V2_lexical_search(const YAP_V2_LEXICAL_SEGMENT *segment, YAP_V2_BYTES_VIEW query,
                          const YAP_V2_LEXICAL_SEARCH_OPTIONS *options, YAP_V2_LEXICAL_HIT *hits,
                          size_t hit_capacity, size_t *hit_count) {
  YAP_V2_LEXICAL_QUERY_PLAN plan;
  int status;
  YAP_V2_lexical_query_plan_init(&plan);
  status = YAP_V2_lexical_query_plan_prepare(query, &plan);
  if (status == YAP_V2_OK)
    status = YAP_V2_lexical_search_prepared(segment, &plan, options, hits,
                                            hit_capacity, hit_count);
  YAP_V2_lexical_query_plan_free(&plan);
  return status;
}
