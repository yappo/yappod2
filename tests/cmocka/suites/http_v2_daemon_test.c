#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>
#include "test_env.h"
#include "test_fs.h"
#include "test_http.h"
#include "test_proc.h"
#include "yappo_config_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_vector_v2.h"

typedef struct { ytest_env_t env; ytest_daemon_stack_t stack; char run[PATH_MAX]; } context_t;
static YAP_V2_BYTES_VIEW view(const char *s) { YAP_V2_BYTES_VIEW v={(const unsigned char *)s,strlen(s)}; return v; }
static void add(YAP_V2_SEGMENT_DESCRIPTOR *s,const YAP_V2_COMPONENT_DESCRIPTOR *c){assert_int_equal(YAP_V2_segment_descriptor_add_component(s,c),YAP_V2_OK);}

static void make_index(context_t *ctx) {
  YAP_V2_CONFIG config; YAP_V2_DOCUMENT_VIEW docs[2]; YAP_V2_PASSAGE_VIEW passages[2];
  YAP_V2_COMPONENT_DESCRIPTOR lexical[3], vectors, metadata; YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_MANIFEST manifest; YAP_EMBEDDING_RESULT embeddings; float values[]={1,0,0,1};
  char segments[PATH_MAX],segment[PATH_MAX],path[PATH_MAX]; FILE *file;
  assert_int_equal(ytest_path_join(path,sizeof(path),ctx->env.tmp_root,"config.toml"),0);
  file=fopen(path,"wb"); assert_non_null(file);
  assert_true(fputs("format_version=2\n[tokenizer]\nid=\"unicode_nfkc_cf_v1\"\n[chunking]\nmax_chars=100\noverlap_chars=0\n[vector]\nenabled=true\nmodel_id=\"embed-v1\"\ndimensions=2\nmetric=\"cosine\"\n[metadata]\nfilterable_fields=[\"category\"]\n",file)>=0);
  assert_int_equal(fclose(file),0); assert_int_equal(YAP_V2_config_load(path,&config,NULL,0),YAP_V2_OK);
  memset(docs,0,sizeof(docs)); docs[0].id=view("doc-fruit");docs[0].url=view("https://e.test/fruit");docs[0].title=view("Fruit");docs[0].body=view("fresh apple");docs[0].metadata_json=view("{\"category\":\"fruit\"}");
  docs[1].id=view("doc-tech");docs[1].url=view("https://e.test/tech");docs[1].title=view("Tech");docs[1].body=view("apple computer");docs[1].metadata_json=view("{\"category\":\"tech\"}");
  memset(passages,0,sizeof(passages));passages[0].id=view("passage-fruit");passages[0].parent_document_id=docs[0].id;passages[0].text=view("fresh apple");passages[0].end_char=11;
  passages[1].id=view("passage-tech");passages[1].parent_document_id=docs[1].id;passages[1].text=view("apple computer");passages[1].end_char=14;
  assert_int_equal(ytest_path_join(segments,sizeof(segments),ctx->env.tmp_root,"segments"),0);assert_int_equal(ytest_path_join(segment,sizeof(segment),segments,"seg-1"),0);assert_int_equal(ytest_mkdir_p(segment,0700),0);
  assert_int_equal(ytest_path_join(path,sizeof(path),segment,"documents.yap2"),0);assert_int_equal(YAP_V2_segment_write(path,"seg-1",1,docs,2,passages,2,&descriptor),YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(segment,1,docs,2,passages,2,lexical),YAP_V2_OK);add(&descriptor,&lexical[0]);add(&descriptor,&lexical[1]);add(&descriptor,&lexical[2]);
  embeddings.values=values;embeddings.input_count=2;embeddings.dimensions=2;assert_int_equal(ytest_path_join(path,sizeof(path),segment,"vectors.yap2"),0);assert_int_equal(YAP_V2_vectors_write(path,1,&config,passages,2,&embeddings,&vectors),YAP_V2_OK);add(&descriptor,&vectors);
  assert_int_equal(ytest_path_join(path,sizeof(path),segment,"metadata.yap2"),0);assert_int_equal(YAP_V2_metadata_write(path,1,&config,docs,2,&metadata),YAP_V2_OK);add(&descriptor,&metadata);
  YAP_V2_manifest_init(&manifest);manifest.generation=1;assert_int_equal(YAP_V2_config_fingerprint(&config,manifest.config_fingerprint),YAP_V2_OK);assert_int_equal(YAP_V2_manifest_add_segment(&manifest,&descriptor),YAP_V2_OK);assert_int_equal(ytest_path_join(path,sizeof(path),ctx->env.tmp_root,"manifest.json"),0);assert_int_equal(YAP_V2_manifest_save_atomic(path,&manifest),YAP_V2_OK);YAP_V2_manifest_free(&manifest);
}

static int setup(void **state){context_t *ctx=calloc(1,sizeof(*ctx));if(!ctx)return -1;ytest_daemon_stack_init(&ctx->stack);if(ytest_env_init(&ctx->env)!=0||ytest_path_join(ctx->run,sizeof(ctx->run),ctx->env.tmp_root,"run")!=0){free(ctx);return -1;}make_index(ctx);if(ytest_daemon_stack_start(&ctx->stack,ctx->env.build_dir,ctx->env.tmp_root,ctx->run)!=0){ytest_daemon_stack_dump_logs(&ctx->stack,stderr);ytest_env_destroy(&ctx->env);free(ctx);return -1;}*state=ctx;return 0;}
static int teardown(void **state){context_t *ctx=*state;if(ctx){ytest_daemon_stack_stop(&ctx->stack);ytest_env_destroy(&ctx->env);free(ctx);}return 0;}

static void test_front_core_v2_roundtrip(void **state){context_t *ctx=*state;char *response=NULL;
  const char *body="{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"scope\":\"documents\",\"limit\":1}";char request[1024];
  assert_true(snprintf(request,sizeof(request),"POST /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"id\":\"doc-fruit\""));free(response);response=NULL;
  body="{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"limit\":1,\"max_context_bytes\":100}";
  assert_true(snprintf(request,sizeof(request),"POST /v2/retrieve HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"passage_id\":\"passage-fruit\""));assert_non_null(strstr(response,"\"url\":\"https://e.test/fruit\""));free(response);response=NULL;
  body="{";assert_true(snprintf(request,sizeof(request),"POST /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);assert_non_null(strstr(response,"400 Bad Request"));free(response);assert_true(ytest_daemon_stack_alive(&ctx->stack));}
int main(void){const struct CMUnitTest tests[]={cmocka_unit_test_setup_teardown(test_front_core_v2_roundtrip,setup,teardown)};return cmocka_run_group_tests(tests,NULL,NULL);}
