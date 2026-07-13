#include "yappo_config_v2.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <toml.h>

typedef struct {
  uint32_t state[8];
  uint64_t bit_count;
  unsigned char block[64];
  size_t block_size;
} SHA256_CTX;

static void set_error(char *error, size_t size, const char *format, ...) {
  va_list args;
  if (error == NULL || size == 0U) return;
  va_start(args, format);
  (void)vsnprintf(error, size, format, args);
  va_end(args);
}

static int key_allowed(const toml_table_t *table, const char *const *allowed) {
  int index;
  for (index = 0;; index++) {
    const char *key = toml_key_in(table, index);
    size_t i;
    int found = 0;
    if (key == NULL) break;
    for (i = 0U; allowed[i] != NULL; i++) {
      if (strcmp(key, allowed[i]) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) return 0;
  }
  return 1;
}

static int copy_string(toml_table_t *table, const char *key, char *output, size_t capacity,
                       int required, char *error, size_t error_size) {
  toml_datum_t value = toml_string_in(table, key);
  size_t length;
  if (!value.ok) {
    if (required || toml_key_exists(table, key)) {
      set_error(error, error_size, "%s must be a string", key);
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  length = strlen(value.u.s);
  if (length >= capacity) {
    free(value.u.s);
    set_error(error, error_size, "%s is too long", key);
    return YAP_V2_OUT_OF_RANGE;
  }
  memcpy(output, value.u.s, length + 1U);
  free(value.u.s);
  return YAP_V2_OK;
}

static int read_uint32(toml_table_t *table, const char *key, uint32_t *output, int required,
                       char *error, size_t error_size) {
  toml_datum_t value = toml_int_in(table, key);
  if (!value.ok) {
    if (required || toml_key_exists(table, key)) {
      set_error(error, error_size, "%s must be an integer", key);
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  if (value.u.i < 0 || (uint64_t)value.u.i > UINT32_MAX) {
    set_error(error, error_size, "%s is out of range", key);
    return YAP_V2_OUT_OF_RANGE;
  }
  *output = (uint32_t)value.u.i;
  return YAP_V2_OK;
}

static int field_compare(const void *left, const void *right) {
  return strcmp((const char *)left, (const char *)right);
}

static int read_filterable_fields(toml_table_t *metadata, YAP_V2_CONFIG *config,
                                  char *error, size_t error_size) {
  toml_array_t *array;
  int count;
  int i;
  if (metadata == NULL)
    return YAP_V2_OK;
  array = toml_array_in(metadata, "filterable_fields");
  if (array == NULL) {
    if (toml_key_exists(metadata, "filterable_fields")) {
      set_error(error, error_size, "filterable_fields must be an array of strings");
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  count = toml_array_nelem(array);
  if (count < 0 || count > (int)YAP_V2_MAX_FILTER_FIELDS) {
    set_error(error, error_size, "too many filterable_fields");
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0; i < count; i++) {
    toml_datum_t value = toml_string_at(array, i);
    size_t length;
    if (!value.ok) {
      set_error(error, error_size, "filterable_fields must contain only strings");
      return YAP_V2_INVALID_FORMAT;
    }
    length = strlen(value.u.s);
    if (length == 0U || length > YAP_V2_MAX_FILTER_FIELD_BYTES) {
      free(value.u.s);
      set_error(error, error_size, "filterable field is empty or too long");
      return YAP_V2_OUT_OF_RANGE;
    }
    memcpy(config->filterable_fields[i], value.u.s, length + 1U);
    free(value.u.s);
  }
  config->filterable_field_count = (size_t)count;
  qsort(config->filterable_fields, config->filterable_field_count,
        sizeof(config->filterable_fields[0]), field_compare);
  for (i = 1; i < count; i++) {
    if (strcmp(config->filterable_fields[i - 1], config->filterable_fields[i]) == 0) {
      set_error(error, error_size, "filterable_fields contains a duplicate");
      return YAP_V2_DUPLICATE;
    }
  }
  return YAP_V2_OK;
}

void YAP_V2_config_init(YAP_V2_CONFIG *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  config->format_version = YAP_V2_FORMAT_VERSION;
  (void)strcpy(config->tokenizer_id, "unicode_nfkc_casefold_v2");
  config->chunk_max_chars = 1200U;
  config->chunk_overlap_chars = 200U;
  config->vector_metric = YAP_V2_VECTOR_DISABLED;
}

int YAP_V2_config_load(const char *path, YAP_V2_CONFIG *config, char *error, size_t error_size) {
  static const char *const root_keys[] = {"format_version", "tokenizer", "chunking", "vector",
                                          "metadata", NULL};
  static const char *const tokenizer_keys[] = {"id", NULL};
  static const char *const chunking_keys[] = {"max_chars", "overlap_chars", NULL};
  static const char *const vector_keys[] = {"enabled", "model_id", "dimensions", "metric", NULL};
  static const char *const metadata_keys[] = {"filterable_fields", NULL};
  FILE *file;
  toml_table_t *root = NULL;
  toml_table_t *tokenizer;
  toml_table_t *chunking;
  toml_table_t *vector;
  toml_table_t *metadata;
  toml_datum_t enabled;
  toml_datum_t metric;
  uint32_t version;
  int status = YAP_V2_INVALID_FORMAT;
  char parse_error[256];

  if (path == NULL || config == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (error != NULL && error_size > 0U) error[0] = '\0';
  file = fopen(path, "r");
  if (file == NULL) {
    set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
    return YAP_V2_IO_ERROR;
  }
  root = toml_parse_file(file, parse_error, (int)sizeof(parse_error));
  (void)fclose(file);
  if (root == NULL) {
    set_error(error, error_size, "invalid TOML: %s", parse_error);
    return YAP_V2_INVALID_FORMAT;
  }
  if (!key_allowed(root, root_keys)) {
    set_error(error, error_size, "config contains an unknown top-level key");
    goto done;
  }
  tokenizer = toml_table_in(root, "tokenizer");
  chunking = toml_table_in(root, "chunking");
  vector = toml_table_in(root, "vector");
  metadata = toml_table_in(root, "metadata");
  if (tokenizer == NULL || chunking == NULL || vector == NULL ||
      !key_allowed(tokenizer, tokenizer_keys) || !key_allowed(chunking, chunking_keys) ||
      !key_allowed(vector, vector_keys) || (metadata != NULL && !key_allowed(metadata, metadata_keys))) {
    set_error(error, error_size, "required table is missing or contains an unknown key");
    goto done;
  }

  YAP_V2_config_init(config);
  version = config->format_version;
  status = read_uint32(root, "format_version", &version, 1, error, error_size);
  if (status != YAP_V2_OK) goto done;
  config->format_version = version;
  status = copy_string(tokenizer, "id", config->tokenizer_id, sizeof(config->tokenizer_id), 0,
                       error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_uint32(chunking, "max_chars", &config->chunk_max_chars, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = read_uint32(chunking, "overlap_chars", &config->chunk_overlap_chars, 0, error, error_size);
  if (status != YAP_V2_OK) goto done;

  enabled = toml_bool_in(vector, "enabled");
  if (!enabled.ok && toml_key_exists(vector, "enabled")) {
    set_error(error, error_size, "enabled must be a boolean");
    status = YAP_V2_INVALID_FORMAT;
    goto done;
  }
  if (enabled.ok && enabled.u.b) {
    status = copy_string(vector, "model_id", config->vector_model_id,
                         sizeof(config->vector_model_id), 1, error, error_size);
    if (status != YAP_V2_OK) goto done;
    status = read_uint32(vector, "dimensions", &config->vector_dimensions, 1, error, error_size);
    if (status != YAP_V2_OK) goto done;
    metric = toml_string_in(vector, "metric");
    if (!metric.ok) {
      set_error(error, error_size, "metric must be a string");
      status = YAP_V2_INVALID_FORMAT;
      goto done;
    }
    if (strcmp(metric.u.s, "cosine") == 0) config->vector_metric = YAP_V2_VECTOR_COSINE;
    else if (strcmp(metric.u.s, "dot") == 0) config->vector_metric = YAP_V2_VECTOR_DOT;
    else if (strcmp(metric.u.s, "l2") == 0) config->vector_metric = YAP_V2_VECTOR_L2;
    else {
      set_error(error, error_size, "metric must be cosine, dot, or l2");
      free(metric.u.s);
      status = YAP_V2_INVALID_FORMAT;
      goto done;
    }
    free(metric.u.s);
  } else {
    toml_datum_t model = toml_string_in(vector, "model_id");
    toml_datum_t dimensions = toml_int_in(vector, "dimensions");
    if ((model.ok && model.u.s[0] != '\0') || (dimensions.ok && dimensions.u.i != 0)) {
      if (model.ok) free(model.u.s);
      set_error(error, error_size, "disabled vector configuration must be empty");
      status = YAP_V2_INVALID_FORMAT;
      goto done;
    }
    if (model.ok) free(model.u.s);
  }
  status = read_filterable_fields(metadata, config, error, error_size);
  if (status != YAP_V2_OK) goto done;
  status = YAP_V2_config_validate(config);
  if (status != YAP_V2_OK) set_error(error, error_size, "configuration violates the v2 schema");
done:
  toml_free(root);
  return status;
}

static uint32_t rotate_right(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

static void sha_transform(SHA256_CTX *ctx, const unsigned char block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
      0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
      0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
      0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
      0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
      0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
      0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
      0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
  uint32_t w[64], a,b,c,d,e,f,g,h;
  size_t i;
  for (i=0;i<16;i++) w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
  for (;i<64;i++) { uint32_t s0=rotate_right(w[i-15],7)^rotate_right(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotate_right(w[i-2],17)^rotate_right(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
  a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];
  for(i=0;i<64;i++){uint32_t s1=rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25);uint32_t ch=(e&f)^((~e)&g);uint32_t t1=h+s1+ch+k[i]+w[i];uint32_t s0=rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22);uint32_t maj=(a&b)^(a&c)^(b&c);uint32_t t2=s0+maj;h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
  ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}

static void sha_init(SHA256_CTX *ctx){static const uint32_t initial[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};memset(ctx,0,sizeof(*ctx));memcpy(ctx->state,initial,sizeof(initial));}
static void sha_update(SHA256_CTX *ctx,const unsigned char *data,size_t length){size_t take;ctx->bit_count+=(uint64_t)length*8U;while(length>0){take=64U-ctx->block_size;if(take>length)take=length;memcpy(ctx->block+ctx->block_size,data,take);ctx->block_size+=take;data+=take;length-=take;if(ctx->block_size==64U){sha_transform(ctx,ctx->block);ctx->block_size=0;}}}
static void sha_final(SHA256_CTX *ctx,unsigned char out[32]){size_t i;uint64_t bits=ctx->bit_count;ctx->block[ctx->block_size++]=0x80U;if(ctx->block_size>56U){memset(ctx->block+ctx->block_size,0,64U-ctx->block_size);sha_transform(ctx,ctx->block);ctx->block_size=0;}memset(ctx->block+ctx->block_size,0,56U-ctx->block_size);for(i=0;i<8;i++)ctx->block[63U-i]=(unsigned char)(bits>>(i*8U));sha_transform(ctx,ctx->block);for(i=0;i<8;i++){out[i*4]=(unsigned char)(ctx->state[i]>>24);out[i*4+1]=(unsigned char)(ctx->state[i]>>16);out[i*4+2]=(unsigned char)(ctx->state[i]>>8);out[i*4+3]=(unsigned char)ctx->state[i];}}

void YAP_V2_sha256_bytes(const unsigned char *data, size_t length, unsigned char output[32]) {
  SHA256_CTX sha;
  if (output == NULL || (length != 0U && data == NULL)) return;
  sha_init(&sha); if (length != 0U) sha_update(&sha, data, length); sha_final(&sha, output);
}

int YAP_V2_config_fingerprint(const YAP_V2_CONFIG *config, unsigned char output[32]) {
  char canonical[32768];
  const char *metric;
  int length;
  size_t used;
  size_t i;
  SHA256_CTX sha;
  if (config == NULL || output == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (YAP_V2_config_validate(config) != YAP_V2_OK) return YAP_V2_INVALID_FORMAT;
  metric = config->vector_metric == YAP_V2_VECTOR_DISABLED ? "disabled" :
           config->vector_metric == YAP_V2_VECTOR_COSINE ? "cosine" :
           config->vector_metric == YAP_V2_VECTOR_DOT ? "dot" : "l2";
  length = snprintf(canonical, sizeof(canonical),
                    "format_version=%" PRIu32 "\ntokenizer.id=%s\nchunking.max_chars=%" PRIu32
                    "\nchunking.overlap_chars=%" PRIu32 "\nvector.model_id=%s\n"
                    "vector.dimensions=%" PRIu32 "\nvector.metric=%s\n",
                    config->format_version, config->tokenizer_id, config->chunk_max_chars,
                    config->chunk_overlap_chars, config->vector_model_id,
                    config->vector_dimensions, metric);
  if (length < 0 || (size_t)length >= sizeof(canonical)) return YAP_V2_OUT_OF_RANGE;
  used = (size_t)length;
  for (i = 0U; i < config->filterable_field_count; i++) {
    length = snprintf(canonical + used, sizeof(canonical) - used, "metadata.filterable_fields[%zu]=%s\n",
                      i, config->filterable_fields[i]);
    if (length < 0 || (size_t)length >= sizeof(canonical) - used) return YAP_V2_OUT_OF_RANGE;
    used += (size_t)length;
  }
  sha_init(&sha); sha_update(&sha, (const unsigned char *)canonical, used); sha_final(&sha, output);
  return YAP_V2_OK;
}

void YAP_V2_config_fingerprint_hex(const unsigned char fingerprint[32], char output[65]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  if (fingerprint == NULL || output == NULL) return;
  for (i=0;i<32;i++){output[i*2]=hex[fingerprint[i]>>4];output[i*2+1]=hex[fingerprint[i]&15U];}
  output[64]='\0';
}
