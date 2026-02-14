#ifndef YTEST_MUTATION_H
#define YTEST_MUTATION_H

int ytest_mutate_malformed_postings(const char *index_dir);
int ytest_mutate_extreme_score_record(const char *index_dir, int fileindex);
int ytest_mutate_filedata_missing_strings(const char *index_dir, int fileindex);

#endif
