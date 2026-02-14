#include "test_input_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_line_range(FILE *in, FILE *out, int from, int to) {
  char *line = NULL;
  size_t cap = 0U;
  ssize_t n;
  int line_no = 0;

  while ((n = getline(&line, &cap, in)) >= 0) {
    line_no++;
    if ((from <= 0 || line_no >= from) && (to <= 0 || line_no <= to)) {
      if (fwrite(line, 1U, (size_t)n, out) != (size_t)n) {
        free(line);
        return -1;
      }
    }
  }

  free(line);
  return 0;
}

int ytest_fixture_copy_with_inject(const char *fixture, const char *out_path, int head_lines,
                                   const char *inject, int tail_from) {
  FILE *in = NULL;
  FILE *out = NULL;
  int rc = -1;

  if (fixture == NULL || out_path == NULL || head_lines < 0 || tail_from < 0) {
    return -1;
  }

  in = fopen(fixture, "rb");
  if (in == NULL) {
    return -1;
  }
  out = fopen(out_path, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }

  if (copy_line_range(in, out, 1, head_lines) != 0) {
    goto done;
  }
  if (inject != NULL && inject[0] != '\0') {
    if (fwrite(inject, 1U, strlen(inject), out) != strlen(inject)) {
      goto done;
    }
  }

  if (fseek(in, 0L, SEEK_SET) != 0) {
    goto done;
  }
  if (copy_line_range(in, out, tail_from, 0) != 0) {
    goto done;
  }

  rc = 0;

done:
  fclose(in);
  fclose(out);
  return rc;
}

int ytest_fixture_build_oversized_burst(const char *fixture, const char *out_path,
                                        size_t huge_payload_len) {
  FILE *in = NULL;
  FILE *out = NULL;
  char *line = NULL;
  size_t cap = 0U;
  ssize_t n;
  int line_no = 0;
  size_t i;
  int rc = -1;

  if (fixture == NULL || out_path == NULL || huge_payload_len == 0U) {
    return -1;
  }

  in = fopen(fixture, "rb");
  if (in == NULL) {
    return -1;
  }
  out = fopen(out_path, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }

  while ((n = getline(&line, &cap, in)) >= 0) {
    line_no++;
    if (line_no <= 4) {
      if (fwrite(line, 1U, (size_t)n, out) != (size_t)n) {
        goto done;
      }
    }
  }

  if (fprintf(out, "http://example.com/huge1\tADD\tHuge1\t100\toversizedtoken666lllmmmnnn") < 0) {
    goto done;
  }
  for (i = 0; i < huge_payload_len; i++) {
    if (fputc('X', out) == EOF) {
      goto done;
    }
  }
  if (fputc('\n', out) == EOF) {
    goto done;
  }

  if (fprintf(out, "http://example.com/huge2\tADD\tHuge2\t100\toversizedtoken777ooopppqqq") < 0) {
    goto done;
  }
  for (i = 0; i < huge_payload_len; i++) {
    if (fputc('X', out) == EOF) {
      goto done;
    }
  }
  if (fputc('\n', out) == EOF) {
    goto done;
  }

  if (fseek(in, 0L, SEEK_SET) != 0) {
    goto done;
  }
  line_no = 0;
  while ((n = getline(&line, &cap, in)) >= 0) {
    line_no++;
    if (line_no >= 5) {
      if (fwrite(line, 1U, (size_t)n, out) != (size_t)n) {
        goto done;
      }
    }
  }

  rc = 0;

done:
  free(line);
  fclose(in);
  fclose(out);
  return rc;
}

int ytest_fixture_rewrite_size_boundary(const char *fixture, const char *out_path) {
  FILE *in = NULL;
  FILE *out = NULL;
  char *line = NULL;
  size_t cap = 0U;
  ssize_t n;
  int rc = -1;

  if (fixture == NULL || out_path == NULL) {
    return -1;
  }

  in = fopen(fixture, "rb");
  if (in == NULL) {
    return -1;
  }
  out = fopen(out_path, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }

  while ((n = getline(&line, &cap, in)) >= 0) {
    char *fields[5] = {NULL, NULL, NULL, NULL, NULL};
    char *saveptr = NULL;
    char *dup = strdup(line);
    int i;

    if (dup == NULL) {
      goto done;
    }

    for (i = 0; i < 5; i++) {
      fields[i] = strtok_r(i == 0 ? dup : NULL, "\t\n", &saveptr);
      if (fields[i] == NULL) {
        break;
      }
    }

    if (i == 5) {
      if (strcmp(fields[0], "http://example.com/doc1") == 0) {
        fields[3] = "24";
      } else if (strcmp(fields[0], "http://example.com/doc2") == 0) {
        fields[3] = "23";
      } else if (strcmp(fields[0], "https://example.com/doc3") == 0) {
        fields[3] = "102400";
      } else if (strcmp(fields[0], "http://example.com/doc5") == 0) {
        fields[3] = "102401";
      }

      if (fprintf(out, "%s\t%s\t%s\t%s\t%s\n", fields[0], fields[1], fields[2], fields[3],
                  fields[4]) < 0) {
        free(dup);
        goto done;
      }
    } else {
      if (fwrite(line, 1U, (size_t)n, out) != (size_t)n) {
        free(dup);
        goto done;
      }
    }

    free(dup);
  }

  rc = 0;

done:
  free(line);
  fclose(in);
  fclose(out);
  return rc;
}

int ytest_fixture_build_edge_cases(const char *out_path, size_t huge_payload_len) {
  FILE *out = NULL;
  size_t i;

  if (out_path == NULL || huge_payload_len == 0U) {
    return -1;
  }

  out = fopen(out_path, "wb");
  if (out == NULL) {
    return -1;
  }

  if (fprintf(out, "http://example.com/ok1\tADD\tOK1\t24\taaaaaaaaaaaaaaaaaaaaaaaa\n") < 0 ||
      fprintf(out, "http://example.com/badsize\tADD\tBadSize\t999999999999\tbadsizepayload\n") <
        0 ||
      fprintf(out, "http://example.com/dup\tADD\tDup\t24\tbbbbbbbbbbbbbbbbbbbbbbbb\n") < 0 ||
      fprintf(out, "http://example.com/dup\tADD\tDup2\t24\tbbbbbbbbbbbbbbbbbbbbbbbb\n") < 0 ||
      fprintf(out, "http://example.com/ok2\tADD\tOK2\t24\tcccccccccccccccccccccccc\n") < 0 ||
      fprintf(out, "http://example.com/long\tADD\tLong\t24\t") < 0) {
    fclose(out);
    return -1;
  }

  for (i = 0; i < huge_payload_len; i++) {
    if (fputc('L', out) == EOF) {
      fclose(out);
      return -1;
    }
  }

  if (fprintf(out,
              "\nhttp://example.com/ok3\tADD\tOK3\t24\tdddddddddddddddddddddddd\n"
              "http://example.com/ok4\tADD\tOK4\t24\teeeeeeeeeeeeeeeeeeeeeeee\n") < 0) {
    fclose(out);
    return -1;
  }

  fclose(out);
  return 0;
}
