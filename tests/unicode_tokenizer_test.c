#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_unicode.h"

#include <ctype.h>
#include <string.h>

static void test_nfkc_casefold_word_boundaries(void **state) {
  const char input[] = "Ｆｏｏ Straße 東京";
  YAP_V2_TOKEN_SEQUENCE sequence;
  (void)state;
  assert_int_equal(YAP_V2_unicode_tokenize(input, strlen(input), &sequence), YAP_V2_OK);
  assert_string_equal(sequence.normalized_utf8, "foo strasse 東京");
  assert_int_equal(sequence.token_count, 3);
  assert_memory_equal(sequence.normalized_utf8 + sequence.tokens[0].byte_start, "foo", 3);
  assert_memory_equal(sequence.normalized_utf8 + sequence.tokens[1].byte_start, "strasse", 7);
  assert_int_equal(sequence.tokens[2].char_end - sequence.tokens[2].char_start, 2);
  YAP_V2_token_sequence_free(&sequence);
}

static void test_invalid_utf8_is_rejected(void **state) {
  const char input[] = {(char)0xc3, (char)0x28};
  YAP_V2_TOKEN_SEQUENCE sequence;
  (void)state;
  assert_int_equal(YAP_V2_unicode_tokenize(input, sizeof(input), &sequence), YAP_V2_INVALID_FORMAT);
}

static void test_sentence_chunks_are_deterministic(void **state) {
  const char input[] = "One sentence. Two sentence. Three sentence.";
  YAP_V2_CHUNK_SEQUENCE first, second;
  size_t i;
  (void)state;
  assert_int_equal(YAP_V2_unicode_chunk("doc-1", input, strlen(input), 25, 5, &first), YAP_V2_OK);
  assert_int_equal(YAP_V2_unicode_chunk("doc-1", input, strlen(input), 25, 5, &second), YAP_V2_OK);
  assert_true(first.chunk_count >= 2);
  assert_int_equal(first.chunk_count, second.chunk_count);
  for (i=0;i<first.chunk_count;i++) {
    assert_string_equal(first.chunks[i].id, second.chunks[i].id);
    assert_true(first.chunks[i].end_char - first.chunks[i].start_char <= 25);
    assert_string_equal(first.chunks[i].text, second.chunks[i].text);
  }
  assert_true(first.chunks[1].start_char < first.chunks[0].end_char);
  YAP_V2_chunk_sequence_free(&first); YAP_V2_chunk_sequence_free(&second);
}

static void test_grapheme_fallback_does_not_split_emoji(void **state) {
  const char input[] = "👍🏽👍🏽";
  YAP_V2_CHUNK_SEQUENCE sequence;
  (void)state;
  assert_int_equal(YAP_V2_unicode_chunk("emoji", input, strlen(input), 2, 0, &sequence), YAP_V2_OK);
  assert_int_equal(sequence.chunk_count, 2);
  assert_string_equal(sequence.chunks[0].text, "👍🏽");
  assert_string_equal(sequence.chunks[1].text, "👍🏽");
  YAP_V2_chunk_sequence_free(&sequence);
}

static void test_chunker_does_not_emit_whitespace_only_passages(void **state) {
  const char input[] = "First sentence.\n　\n　\n　\n　\nSecond sentence.";
  YAP_V2_CHUNK_SEQUENCE sequence;
  size_t i, j;
  int found_first = 0, found_second = 0;
  (void)state;
  assert_int_equal(YAP_V2_unicode_chunk("whitespace", input, strlen(input), 12, 3, &sequence), YAP_V2_OK);
  assert_true(sequence.chunk_count >= 2);
  for (i = 0; i < sequence.chunk_count; i++) {
    int has_non_whitespace = 0;
    assert_int_equal(sequence.chunks[i].ordinal, i);
    for (j = 0; j < sequence.chunks[i].text_bytes; j++) {
      if (!isspace((unsigned char)sequence.chunks[i].text[j])) {
        has_non_whitespace = 1;
        break;
      }
    }
    assert_true(has_non_whitespace);
    if (strstr(sequence.chunks[i].text, "first") != NULL) found_first = 1;
    if (strstr(sequence.chunks[i].text, "second") != NULL) found_second = 1;
  }
  assert_true(found_first);
  assert_true(found_second);
  YAP_V2_chunk_sequence_free(&sequence);
}

int main(void) {
  const struct CMUnitTest tests[]={cmocka_unit_test(test_nfkc_casefold_word_boundaries),cmocka_unit_test(test_invalid_utf8_is_rejected),cmocka_unit_test(test_sentence_chunks_are_deterministic),cmocka_unit_test(test_grapheme_fallback_does_not_split_emoji),cmocka_unit_test(test_chunker_does_not_emit_whitespace_only_passages)};
  return cmocka_run_group_tests(tests,NULL,NULL);
}
