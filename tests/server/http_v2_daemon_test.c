#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cmocka.h>
#include "test_env.h"
#include "test_fs.h"
#include "test_http.h"
#include "test_proc.h"
#include "config/yappo_config_v2.h"
#include "components/yappo_lexical_v2.h"
#include "storage/yappo_manifest_v2.h"
#include "components/yappo_metadata_v2.h"
#include "components/yappo_vector_v2.h"
#include "server/yappo_core_http_v2.h"

typedef struct { ytest_env_t env; ytest_daemon_stack_t stack; char run[PATH_MAX]; char policy[PATH_MAX]; } context_t;
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  size_t waiting;
  int start;
} ingest_batch_gate_t;
typedef struct {
  context_t *context;
  ingest_batch_gate_t *gate;
  size_t id;
  char *response;
} ingest_batch_worker_t;
static const char *policy_source = NULL;
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
  YAP_V2_manifest_init(&manifest);manifest.generation=1;assert_int_equal(YAP_V2_config_fingerprint(&config,manifest.config_fingerprint),YAP_V2_OK);assert_int_equal(YAP_V2_manifest_add_segment(&manifest,&descriptor),YAP_V2_OK);assert_int_equal(ytest_path_join(path,sizeof(path),ctx->env.tmp_root,"manifest.yap2"),0);assert_int_equal(YAP_V2_manifest_save_atomic(path,&manifest),YAP_V2_OK);YAP_V2_manifest_free(&manifest);
}

static int setup(void **state){context_t *ctx=calloc(1,sizeof(*ctx));FILE *file;const char *config=NULL;if(!ctx)return -1;ytest_daemon_stack_init(&ctx->stack);if(ytest_env_init(&ctx->env)!=0||ytest_path_join(ctx->run,sizeof(ctx->run),ctx->env.tmp_root,"run")!=0){free(ctx);return -1;}make_index(ctx);if(policy_source!=NULL){if(ytest_path_join(ctx->policy,sizeof(ctx->policy),ctx->env.tmp_root,"runtime.toml")!=0){ytest_env_destroy(&ctx->env);free(ctx);return -1;}file=fopen(ctx->policy,"wb");if(file==NULL){ytest_env_destroy(&ctx->env);free(ctx);return -1;}if(fputs(policy_source,file)<0||fclose(file)!=0){ytest_env_destroy(&ctx->env);free(ctx);return -1;}config=ctx->policy;}if(ytest_daemon_stack_start_with_config(&ctx->stack,ctx->env.build_dir,ctx->env.tmp_root,ctx->run,config)!=0){ytest_daemon_stack_dump_logs(&ctx->stack,stderr);ytest_env_destroy(&ctx->env);free(ctx);return -1;}*state=ctx;return 0;}
static int teardown(void **state){context_t *ctx=*state;if(ctx){ytest_daemon_stack_stop(&ctx->stack);ytest_env_destroy(&ctx->env);free(ctx);}policy_source=NULL;return 0;}

static int setup_index_only(void **state) {
  context_t *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) return -1;
  ytest_daemon_stack_init(&ctx->stack);
  if (ytest_env_init(&ctx->env) != 0 ||
      ytest_path_join(ctx->run, sizeof(ctx->run), ctx->env.tmp_root, "foreground") != 0 ||
      ytest_mkdir_p(ctx->run, 0700) != 0) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
    return -1;
  }
  make_index(ctx);
  *state = ctx;
  return 0;
}

static int teardown_index_only(void **state) {
  context_t *ctx = *state;
  if (ctx != NULL) {
    ytest_daemon_stack_stop(&ctx->stack);
    ytest_env_destroy(&ctx->env);
    free(ctx);
  }
  return 0;
}

static int setup_write_token(void **state) {
  policy_source = "[daemon]\nwrite_token='0123456789abcdef-secure'\n";
  return setup(state);
}

static int teardown_write_token(void **state) {
  return teardown(state);
}

static int setup_tiny_memory_limit(void **state) {
  policy_source = "[daemon]\nmax_inflight_bytes=1\n";
  return setup(state);
}

static int teardown_tiny_memory_limit(void **state) {
  return teardown(state);
}

static int setup_tiny_writer_limit(void **state) {
  policy_source = "[daemon]\ncore_writer_queue_bytes=1\n";
  return setup(state);
}

static int teardown_tiny_writer_limit(void **state) {
  return teardown(state);
}

static int setup_single_worker(void **state) {
  policy_source = "[daemon]\nfront_io_threads=1\ncore_io_threads=1\ncore_search_threads=1\n";
  return setup(state);
}

static int teardown_single_worker(void **state) {
  return teardown(state);
}

static int setup_ingest_batch(void **state) {
  policy_source =
    "[daemon]\ncore_writer_queue_capacity=8\n"
    "auto_compact_enabled=false\n";
  return setup(state);
}

static int teardown_ingest_batch(void **state) {
  return teardown(state);
}

static int setup_automatic_compaction(void **state) {
  policy_source =
    "[daemon]\n"
    "auto_compact_check_interval_ms=1000\n"
    "auto_compact_small_segment_bytes=1048576\n"
    "auto_compact_min_small_segments=4\n";
  return setup(state);
}

static int teardown_automatic_compaction(void **state) {
  return teardown(state);
}

static char *post(context_t *ctx, const char *endpoint, const char *body) {
  char request[4096]; char *response = NULL;
  assert_true(snprintf(request,sizeof(request),"POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",endpoint,strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);
  return response;
}

static char *query_port(int port, const char *endpoint, const char *body) {
  char request[4096]; char *response = NULL;
  assert_true(snprintf(request,sizeof(request),"QUERY %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAccept: application/json\r\nContent-Length: %zu\r\n\r\n%s",endpoint,strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(port,request,&response),0);
  return response;
}

static char *query(context_t *ctx, const char *endpoint, const char *body) {
  return query_port(ctx->stack.front_port, endpoint, body);
}

static int connect_core(int port) {
  struct sockaddr_in address;
  struct timeval timeout = {2, 0};
  int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
      setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0 ||
      connect(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(descriptor);
    return -1;
  }
  return descriptor;
}

static int send_all(int descriptor, const char *data, size_t bytes) {
  size_t sent = 0U;
  while (sent < bytes) {
    ssize_t written = send(descriptor, data + sent, bytes - sent, 0);
    if (written <= 0) return -1;
    sent += (size_t)written;
  }
  return 0;
}

static char *receive_http_response(int descriptor) {
  size_t capacity = 4096U, used = 0U, required = 0U;
  char *response = malloc(capacity);
  if (response == NULL) return NULL;
  for (;;) {
    char *header_end;
    ssize_t received;
    if (used + 1U == capacity) {
      char *larger;
      if (capacity >= YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES +
                      YAP_V2_CORE_HTTP_MAX_HEADER_BYTES) {
        free(response);
        return NULL;
      }
      capacity *= 2U;
      larger = realloc(response, capacity);
      if (larger == NULL) { free(response); return NULL; }
      response = larger;
    }
    received = recv(descriptor, response + used, capacity - used - 1U, 0);
    if (received <= 0) { free(response); return NULL; }
    used += (size_t)received;
    response[used] = '\0';
    header_end = strstr(response, "\r\n\r\n");
    if (header_end != NULL && required == 0U) {
      char *length = strstr(response, "Content-Length: ");
      unsigned long long body_bytes;
      if (length == NULL) { free(response); return NULL; }
      body_bytes = strtoull(length + sizeof("Content-Length: ") - 1U,
                            NULL, 10);
      if (body_bytes > YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES) {
        free(response);
        return NULL;
      }
      required = (size_t)(header_end + 4U - response) + (size_t)body_bytes;
    }
    if (required != 0U && used >= required) return response;
  }
}

static char *get(context_t *ctx, const char *endpoint) {
  char request[1024]; char *response = NULL;
  assert_true(snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",endpoint)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);
  return response;
}

static char *post_authorized_port(int port, const char *endpoint, const char *body,
                                  const char *authorization) {
  char request[4096]; char *response = NULL;
  assert_true(snprintf(request,sizeof(request),"POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAuthorization: %s\r\nContent-Length: %zu\r\n\r\n%s",endpoint,authorization,strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(port,request,&response),0);
  return response;
}

static char *post_authorized(context_t *ctx, const char *endpoint, const char *body,
                             const char *authorization) {
  return post_authorized_port(ctx->stack.front_port, endpoint, body, authorization);
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void test_front_core_v2_roundtrip(void **state){context_t *ctx=*state;char *response=NULL,*query_response=NULL;
  const char *body="{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"scope\":\"documents\",\"limit\":1}";char request[1024];
  response=get(ctx,"/health/live");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"status\":\"live\""));free(response);response=NULL;
  response=get(ctx,"/health/ready");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"ready\":true"));assert_non_null(strstr(response,"\"generation\":1"));assert_non_null(strstr(response,"\"segment_health\""));assert_non_null(strstr(response,"\"document_records\":2"));assert_non_null(strstr(response,"\"state\":\"precomputed_ready\""));free(response);response=NULL;
  response=post(ctx,"/v2/passages:prepare","{\"id\":\"doc-new\",\"body\":\"Fresh APPLE.\"}");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"model_id\":\"embed-v1\""));assert_non_null(strstr(response,"\"dimensions\":2"));assert_non_null(strstr(response,"\"text\":\"fresh apple.\""));free(response);response=NULL;
  query_response=query(ctx,"/v2/search",body);assert_non_null(strstr(query_response,"200 OK"));assert_non_null(strstr(query_response,"Accept-Query: application/json"));assert_non_null(strstr(query_response,"\"id\":\"doc-fruit\""));
  response=post(ctx,"/v2/search",body);assert_string_equal(response,query_response);free(response);response=NULL;free(query_response);query_response=NULL;
  body="{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"limit\":1,\"max_context_bytes\":100}";
  response=query(ctx,"/v2/retrieve",body);assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"Accept-Query: application/json"));assert_non_null(strstr(response,"\"passage_id\":\"passage-fruit\""));assert_non_null(strstr(response,"\"url\":\"https://e.test/fruit\""));free(response);response=NULL;
  response=query_port(ctx->stack.core_port,"/v2/search","{\"query\":\"apple\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1}");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"Server: Yappo Search Core/2.0"));assert_non_null(strstr(response,"Accept-Query: application/json"));free(response);response=NULL;
  assert_true(snprintf(request,sizeof(request),"POST /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}")>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.core_port,request,&response),0);assert_non_null(strstr(response,"405 Method Not Allowed"));assert_non_null(strstr(response,"Allow: QUERY"));assert_non_null(strstr(response,"Accept-Query: application/json"));free(response);response=NULL;
  assert_true(snprintf(request,sizeof(request),"GET /health/ready HTTP/1.1\r\nHost: localhost\r\n\r\n")>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.core_port,request,&response),0);assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"service\":\"yappod_core\""));free(response);response=NULL;
  body="{";assert_true(snprintf(request,sizeof(request),"POST /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body)>0);
  assert_int_equal(ytest_http_send_text(ctx->stack.front_port,request,&response),0);assert_non_null(strstr(response,"400 Bad Request"));free(response);response=NULL;
  response=get(ctx,"/healthz");assert_non_null(strstr(response,"404 Not Found"));free(response);response=NULL;
  response=get(ctx,"/yappo/100000/AND/0-10?apple");assert_non_null(strstr(response,"404 Not Found"));free(response);response=NULL;
  response=get(ctx,"/v2/search");assert_non_null(strstr(response,"405 Method Not Allowed"));assert_non_null(strstr(response,"Allow: QUERY, POST"));assert_non_null(strstr(response,"Accept-Query: application/json"));free(response);response=NULL;
  response=query(ctx,"/v2/passages:prepare","{}");assert_non_null(strstr(response,"405 Method Not Allowed"));assert_non_null(strstr(response,"Allow: POST"));assert_null(strstr(response,"Accept-Query:"));free(response);response=NULL;
  response=query(ctx,"/v2/documents:batch","{}");assert_non_null(strstr(response,"405 Method Not Allowed"));assert_non_null(strstr(response,"Allow: POST"));assert_null(strstr(response,"Accept-Query:"));free(response);response=NULL;
  response=get(ctx,"/metrics");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"text/plain; version=0.0.4"));
  assert_non_null(strstr(response,"yappod_v2_manifest_generation 1"));
  assert_non_null(strstr(response,"yappod_v2_manifest_segments 1"));
  assert_non_null(strstr(response,"yappod_v2_manifest_document_records 2"));
  assert_non_null(strstr(response,"yappod_v2_small_segment_run 1"));
  assert_non_null(strstr(response,"yappod_v2_auto_compaction_needed 0"));
  assert_non_null(strstr(response,"yappod_v2_requests_total{operation=\"search\",status_class=\"2xx\"} 2"));
  assert_non_null(strstr(response,"yappod_v2_requests_total{operation=\"retrieve\",status_class=\"2xx\"} 2"));
  assert_non_null(strstr(response,"yappod_v2_requests_total{operation=\"search\",status_class=\"4xx\"} 1"));
  assert_non_null(strstr(response,"yappod_v2_compaction_state{state=\"idle\"} 1"));
  free(response);assert_true(ytest_daemon_stack_alive(&ctx->stack));}

static void test_core_keeps_connection_for_sequential_requests(void **state) {
  context_t *ctx = *state;
  const char request[] =
    "GET /health/ready HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: keep-alive\r\n\r\n";
  char *response;
  int descriptor = connect_core(ctx->stack.core_port);
  assert_true(descriptor >= 0);
  assert_int_equal(send_all(descriptor, request, sizeof(request) - 1U), 0);
  response = receive_http_response(descriptor);
  assert_non_null(response);
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "Connection: keep-alive"));
  free(response);
  assert_int_equal(send_all(descriptor, request, sizeof(request) - 1U), 0);
  response = receive_http_response(descriptor);
  assert_non_null(response);
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "Connection: keep-alive"));
  free(response);
  close(descriptor);
}

static void test_liveness_survives_readiness_failure(void **state) {
  context_t *ctx=*state; char path[PATH_MAX]; char *response;
  assert_int_equal(ytest_path_join(path,sizeof(path),ctx->env.tmp_root,"manifest.yap2"),0);
  { FILE *file=fopen(path,"wb");assert_non_null(file);assert_true(fputs("{}\n",file)>=0);assert_int_equal(fclose(file),0); }
  response=get(ctx,"/health/live");assert_non_null(strstr(response,"200 OK"));free(response);
  response=get(ctx,"/health/ready");assert_non_null(strstr(response,"503 Service Unavailable"));assert_non_null(strstr(response,"\"ready\":false"));free(response);
  response=get(ctx,"/metrics");assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"yappod_v2_ready 0"));free(response);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_front_core_atomic_nrt_updates(void **state) {
  context_t *ctx=*state; char *response; struct timespec start,end;
  response=post(ctx,"/v2/documents:batch","{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-live\",\"url\":\"https://e.test/live\",\"title\":\"Live\",\"body\":\"fresh pear\",\"metadata\":{\"category\":\"fruit\"},\"vectors\":[[1,0]]}]}");
  assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"generation\":2"));free(response);
  assert_int_equal(clock_gettime(CLOCK_MONOTONIC,&start),0);
  response=post(ctx,"/v2/search","{\"query\":\"pear\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_int_equal(clock_gettime(CLOCK_MONOTONIC,&end),0);assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"id\":\"doc-live\""));
  assert_true(elapsed_seconds(start,end)<=1.0);free(response);
  response=post(ctx,"/v2/documents:batch","{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-live\",\"body\":\"ripe banana\",\"vectors\":[[1,0]]}]}");
  assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"generation\":3"));free(response);
  response=post(ctx,"/v2/search","{\"query\":\"pear\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_non_null(strstr(response,"200 OK"));assert_null(strstr(response,"\"id\":\"doc-live\""));free(response);
  response=post(ctx,"/v2/search","{\"query\":\"banana\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_non_null(strstr(response,"\"id\":\"doc-live\""));free(response);
  response=post(ctx,"/v2/documents:batch","{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-atomic\",\"body\":\"atomicword\",\"vectors\":[[1,0]]},{\"operation\":\"upsert\",\"id\":\"bad\",\"body\":\"bad\",\"vectors\":[[1]]}]}");
  assert_non_null(strstr(response,"400 Bad Request"));free(response);
  response=post(ctx,"/v2/search","{\"query\":\"atomicword\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_null(strstr(response,"\"id\":\"doc-atomic\""));assert_non_null(strstr(response,"\"generation\":3"));free(response);
  response=post(ctx,"/v2/documents:batch","{\"operations\":[{\"operation\":\"delete\",\"id\":\"doc-live\"}]}");
  assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"generation\":4"));free(response);
  response=post(ctx,"/v2/search","{\"query\":\"banana\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_null(strstr(response,"\"id\":\"doc-live\""));free(response);assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void *run_ingest_batch_worker(void *opaque) {
  ingest_batch_worker_t *worker = opaque;
  char body[512];
  pthread_mutex_lock(&worker->gate->lock);
  worker->gate->waiting++;
  pthread_cond_broadcast(&worker->gate->ready);
  while (!worker->gate->start)
    pthread_cond_wait(&worker->gate->ready, &worker->gate->lock);
  pthread_mutex_unlock(&worker->gate->lock);
  if (snprintf(
    body, sizeof(body),
    "{\"operations\":[{\"operation\":\"upsert\","
    "\"id\":\"batch-%zu\",\"body\":\"micro batch %zu\","
    "\"vectors\":[[1,0]]}]}", worker->id, worker->id) <= 0)
    return NULL;
  worker->response = post(worker->context, "/v2/documents:batch", body);
  return NULL;
}

static void test_concurrent_ingest_uses_one_generation(void **state) {
  enum { WORKERS = 4 };
  context_t *ctx = *state;
  ingest_batch_gate_t gate;
  ingest_batch_worker_t workers[WORKERS];
  pthread_t threads[WORKERS];
  YAP_V2_MANIFEST manifest;
  char path[PATH_MAX];
  size_t i;
  memset(&gate, 0, sizeof(gate));
  memset(workers, 0, sizeof(workers));
  assert_int_equal(pthread_mutex_init(&gate.lock, NULL), 0);
  assert_int_equal(pthread_cond_init(&gate.ready, NULL), 0);
  for (i = 0U; i < WORKERS; i++) {
    workers[i].context = ctx;
    workers[i].gate = &gate;
    workers[i].id = i;
    assert_int_equal(pthread_create(&threads[i], NULL,
                                    run_ingest_batch_worker,
                                    &workers[i]), 0);
  }
  pthread_mutex_lock(&gate.lock);
  while (gate.waiting != WORKERS)
    pthread_cond_wait(&gate.ready, &gate.lock);
  gate.start = 1;
  pthread_cond_broadcast(&gate.ready);
  pthread_mutex_unlock(&gate.lock);
  for (i = 0U; i < WORKERS; i++) {
    assert_int_equal(pthread_join(threads[i], NULL), 0);
    assert_non_null(workers[i].response);
    assert_non_null(strstr(workers[i].response, "200 OK"));
    assert_non_null(strstr(workers[i].response, "\"generation\":2"));
    free(workers[i].response);
  }
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->env.tmp_root,
                                   "manifest.yap2"), 0);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_OK);
  assert_int_equal(manifest.generation, 2U);
  assert_int_equal(manifest.segment_count, 2U);
  YAP_V2_manifest_free(&manifest);
  {
    char *response = get(ctx, "/metrics");
    assert_non_null(strstr(response, "yappod_v2_ingest_microbatches_total 1"));
    assert_non_null(strstr(response, "yappod_v2_ingest_requests_total 4"));
    assert_non_null(strstr(response, "yappod_v2_ingest_operations_total 4"));
    assert_non_null(strstr(
      response, "yappod_v2_ingest_published_generations_total 1"));
    assert_non_null(strstr(
      response, "yappod_v2_ingest_generations_saved_total 3"));
    assert_non_null(strstr(response, "yappod_v2_ingest_max_batch_requests 4"));
    free(response);
  }
  assert_int_equal(pthread_cond_destroy(&gate.ready), 0);
  assert_int_equal(pthread_mutex_destroy(&gate.lock), 0);
}

static void test_write_token_protects_daemon_ingest(void **state) {
  context_t *ctx=*state; char *response; char path[PATH_MAX]; const char *body =
    "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-secure\",\"body\":\"secureword\",\"vectors\":[[1,0]]}]}";
  assert_int_equal(ytest_path_join(path,sizeof(path),ctx->run,"core.log"),0);assert_int_equal(access(path,F_OK),0);
  assert_int_equal(ytest_path_join(path,sizeof(path),ctx->run,"front.log"),0);assert_int_equal(access(path,F_OK),0);
  response=post(ctx,"/v2/documents:batch",body);
  assert_non_null(strstr(response,"401 Unauthorized"));free(response);
  response=post_authorized(ctx,"/v2/documents:batch",body,"Bearer wrong-token");
  assert_non_null(strstr(response,"401 Unauthorized"));free(response);
  response=post_authorized(ctx,"/v2/documents:batch",body,"Bearer 0123456789abcdef-secure");
  assert_non_null(strstr(response,"200 OK"));free(response);
  body="{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-core-secure\",\"body\":\"coresecureword\",\"vectors\":[[1,0]]}]}";
  response=post_authorized_port(ctx->stack.core_port,"/v2/documents:batch",body,"Bearer wrong-token");
  assert_non_null(strstr(response,"401 Unauthorized"));free(response);
  response=post_authorized_port(ctx->stack.core_port,"/v2/documents:batch",body,"Bearer 0123456789abcdef-secure");
  assert_non_null(strstr(response,"200 OK"));free(response);
  response=post(ctx,"/v2/search","{\"query\":\"secureword\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}");
  assert_non_null(strstr(response,"200 OK"));assert_non_null(strstr(response,"\"id\":\"doc-secure\""));
  free(response);assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_memory_limit_rejects_before_body_allocation(void **state) {
  context_t *ctx=*state; char *response=post(ctx,"/v2/search","{\"query\":\"apple\",\"mode\":\"lexical\"}");
  assert_non_null(strstr(response,"503 Service Unavailable"));
  assert_non_null(strstr(response,"\"code\":\"overloaded\""));free(response);
  response = post(ctx, "/v2/documents:batch",
                  "{\"operations\":[{\"operation\":\"delete\",\"id\":\"missing\"}]}");
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "\"accepted\":1"));
  free(response);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_configured_single_worker_serves_requests(void **state) {
  context_t *ctx = *state;
  char *response = get(ctx, "/health/ready");
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "\"ready\":true"));
  free(response);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_single_reactor_is_not_blocked_by_partial_request(void **state) {
  context_t *ctx = *state;
  struct sockaddr_in address;
  struct timespec start, end;
  const char partial[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: application/json\r\nContent-Length: 100\r\n";
  const char health[] =
    "GET /health/ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  char *response = NULL;
  int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  assert_true(descriptor >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)ctx->stack.core_port);
  assert_int_equal(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
  assert_int_equal(connect(descriptor, (struct sockaddr *)&address,
                           sizeof(address)), 0);
  assert_int_equal(send(descriptor, partial, sizeof(partial) - 1U, 0),
                   sizeof(partial) - 1U);
  assert_int_equal(clock_gettime(CLOCK_MONOTONIC, &start), 0);
  assert_int_equal(ytest_http_send_text(ctx->stack.core_port, health, &response), 0);
  assert_int_equal(clock_gettime(CLOCK_MONOTONIC, &end), 0);
  assert_non_null(strstr(response, "200 OK"));
  assert_true(elapsed_seconds(start, end) < 1.0);
  free(response);
  close(descriptor);
}

static void test_writer_bytes_rejects_from_headers(void **state) {
  context_t *ctx = *state;
  const char request[] =
    "POST /v2/documents:batch HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n";
  char *response = NULL;
  assert_int_equal(ytest_http_send_text(ctx->stack.core_port, request, &response), 0);
  assert_non_null(strstr(response, "503 Service Unavailable"));
  assert_non_null(strstr(response, "\"code\":\"overloaded\""));
  free(response);
}

static void test_core_automatically_compacts_small_segments(void **state) {
  context_t *ctx = *state;
  const char partial_update[] =
    "POST /v2/documents:batch HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: application/json\r\nContent-Length: 100\r\n\r\n{";
  char *response = NULL;
  char path[PATH_MAX], body[512];
  int partial_descriptor = connect_core(ctx->stack.core_port);
  int attempt;
  size_t i;
  uint64_t generation = 0U;
  size_t segments = 0U;
  assert_true(partial_descriptor >= 0);
  assert_int_equal(send_all(partial_descriptor, partial_update,
                            sizeof(partial_update) - 1U), 0);
  for (i = 0U; i < 3U; i++) {
    assert_true(snprintf(
      body, sizeof(body),
      "{\"operations\":[{\"operation\":\"upsert\","
      "\"id\":\"auto-%zu\",\"body\":\"automaticword\","
      "\"vectors\":[[1,0]]}]}", i) > 0);
    response = post(ctx, "/v2/documents:batch", body);
    assert_non_null(strstr(response, "200 OK"));
    free(response);
  }
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->env.tmp_root,
                                   "manifest.yap2"), 0);
  usleep(1500000);
  {
    YAP_V2_MANIFEST manifest;
    YAP_V2_manifest_init(&manifest);
    assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_OK);
    assert_int_equal(manifest.generation, 4U);
    assert_int_equal(manifest.segment_count, 4U);
    YAP_V2_manifest_free(&manifest);
  }
  close(partial_descriptor);
  for (attempt = 0; attempt < 100; attempt++) {
    YAP_V2_MANIFEST manifest;
    YAP_V2_manifest_init(&manifest);
    if (YAP_V2_manifest_load(path, &manifest) == YAP_V2_OK) {
      generation = manifest.generation;
      segments = manifest.segment_count;
    }
    YAP_V2_manifest_free(&manifest);
    if (generation >= 5U && segments < 4U) break;
    usleep(100000);
  }
  assert_true(generation >= 5U);
  assert_true(segments < 4U);
  response = get(ctx, "/metrics");
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(
    response, "yappod_v2_small_segment_threshold_bytes 1048576"));
  assert_non_null(strstr(response, "yappod_v2_auto_compaction_needed 0"));
  free(response);
  response = post(
    ctx, "/v2/search",
    "{\"query\":\"automaticword\",\"mode\":\"lexical\","
    "\"scope\":\"documents\",\"limit\":10}");
  assert_non_null(strstr(response, "200 OK"));
  for (i = 0U; i < 3U; i++) {
    char id[32];
    assert_true(snprintf(id, sizeof(id), "\"id\":\"auto-%zu\"", i) > 0);
    assert_non_null(strstr(response, id));
  }
  free(response);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static pid_t launch_foreground(char *const argv[], const char *cwd) {
  pid_t pid = fork();
  assert_true(pid >= 0);
  if (pid == 0) {
    if (chdir(cwd) != 0) _exit(126);
    execv(argv[0], argv);
    _exit(127);
  }
  return pid;
}

static int stop_foreground(pid_t pid) {
  int status = 0, i;
  if (pid <= 0 || kill(pid, SIGTERM) != 0) return -1;
  for (i = 0; i < 150; i++) {
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    if (waited < 0) return -1;
    usleep(20000);
  }
  (void)kill(pid, SIGKILL);
  (void)waitpid(pid, &status, 0);
  return -1;
}

static void test_foreground_process_lifecycle(void **state) {
  context_t *ctx = *state;
  char core[PATH_MAX], front[PATH_MAX], core_port[16], front_port[16], path[PATH_MAX];
  char *core_argv[7], *front_argv[11], *response = NULL;

  assert_int_equal(ytest_pick_unused_port(&ctx->stack.core_port), 0);
  assert_int_equal(ytest_pick_unused_port(&ctx->stack.front_port), 0);
  assert_true(ctx->stack.core_port != ctx->stack.front_port);
  assert_int_equal(ytest_path_join(core, sizeof(core), ctx->env.build_dir, "yappod_core"), 0);
  assert_int_equal(ytest_path_join(front, sizeof(front), ctx->env.build_dir, "yappod_front"), 0);
  assert_true(snprintf(core_port, sizeof(core_port), "%d", ctx->stack.core_port) > 0);
  assert_true(snprintf(front_port, sizeof(front_port), "%d", ctx->stack.front_port) > 0);

  core_argv[0] = core; core_argv[1] = "--foreground"; core_argv[2] = "--index";
  core_argv[3] = ctx->env.tmp_root; core_argv[4] = "--port"; core_argv[5] = core_port;
  core_argv[6] = NULL;
  ctx->stack.core_pid = launch_foreground(core_argv, ctx->run);
  assert_int_equal(ytest_wait_for_port(ctx->stack.core_port, 50, 100), 0);

  front_argv[0] = front; front_argv[1] = "--foreground"; front_argv[2] = "--index";
  front_argv[3] = ctx->env.tmp_root; front_argv[4] = "--core-host";
  front_argv[5] = "127.0.0.1"; front_argv[6] = "--port"; front_argv[7] = front_port;
  front_argv[8] = "--core-port"; front_argv[9] = core_port; front_argv[10] = NULL;
  ctx->stack.front_pid = launch_foreground(front_argv, ctx->run);
  assert_int_equal(ytest_wait_for_port(ctx->stack.front_port, 50, 100), 0);

  response = get(ctx, "/health/ready");
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "\"ready\":true"));
  free(response);

  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "core.pid"), 0);
  assert_int_equal(access(path, F_OK), -1);
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "front.pid"), 0);
  assert_int_equal(access(path, F_OK), -1);
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "core.log"), 0);
  assert_int_equal(access(path, F_OK), -1);
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "front.log"), 0);
  assert_int_equal(access(path, F_OK), -1);
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "core.error"), 0);
  assert_int_equal(access(path, F_OK), -1);
  assert_int_equal(ytest_path_join(path, sizeof(path), ctx->run, "front.error"), 0);
  assert_int_equal(access(path, F_OK), -1);

  assert_int_equal(stop_foreground(ctx->stack.front_pid), 0);
  ctx->stack.front_pid = 0;
  assert_int_equal(stop_foreground(ctx->stack.core_pid), 0);
  ctx->stack.core_pid = 0;
}

int main(void){const struct CMUnitTest tests[]={
  cmocka_unit_test_setup_teardown(test_front_core_v2_roundtrip,setup,teardown),
  cmocka_unit_test_setup_teardown(test_core_keeps_connection_for_sequential_requests,setup,teardown),
  cmocka_unit_test_setup_teardown(test_liveness_survives_readiness_failure,setup,teardown),
  cmocka_unit_test_setup_teardown(test_front_core_atomic_nrt_updates,setup,teardown),
  cmocka_unit_test_setup_teardown(test_concurrent_ingest_uses_one_generation,setup_ingest_batch,teardown_ingest_batch),
  cmocka_unit_test_setup_teardown(test_write_token_protects_daemon_ingest,setup_write_token,teardown_write_token),
  cmocka_unit_test_setup_teardown(test_memory_limit_rejects_before_body_allocation,setup_tiny_memory_limit,teardown_tiny_memory_limit),
  cmocka_unit_test_setup_teardown(test_configured_single_worker_serves_requests,setup_single_worker,teardown_single_worker),
  cmocka_unit_test_setup_teardown(test_single_reactor_is_not_blocked_by_partial_request,setup_single_worker,teardown_single_worker),
  cmocka_unit_test_setup_teardown(test_writer_bytes_rejects_from_headers,setup_tiny_writer_limit,teardown_tiny_writer_limit),
  cmocka_unit_test_setup_teardown(test_core_automatically_compacts_small_segments,setup_automatic_compaction,teardown_automatic_compaction),
  cmocka_unit_test_setup_teardown(test_foreground_process_lifecycle,setup_index_only,teardown_index_only)
};return cmocka_run_group_tests(tests,NULL,NULL);}
