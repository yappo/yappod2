#include "yappo_ingest.h"

#include "yappo_config_v2.h"
#include "yappo_application_config.h"
#include "yappo_unicode.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

typedef struct { const char *key; size_t key_len; yyjson_val *value; } JSON_PAIR;

static void error_set(char *error, size_t size, const char *message) {
  if (error != NULL && size > 0U) (void)snprintf(error, size, "%s", message);
}

static char *copy_text(const char *text, size_t length) {
  char *copy = (char *)malloc(length + 1U);
  if (copy != NULL) { memcpy(copy, text, length); copy[length] = '\0'; }
  return copy;
}

void YAP_V2_ingest_operation_free(YAP_V2_INGEST_OPERATION *operation) {
  if (operation == NULL) return;
  free(operation->id); free(operation->url); free(operation->title);
  free(operation->body); free(operation->metadata_json);
  free(operation->vectors);
  memset(operation, 0, sizeof(*operation));
}

static int pair_compare(const void *left, const void *right) {
  const JSON_PAIR *a = (const JSON_PAIR *)left, *b = (const JSON_PAIR *)right;
  size_t common = a->key_len < b->key_len ? a->key_len : b->key_len;
  int result = memcmp(a->key, b->key, common);
  if (result != 0) return result;
  return a->key_len < b->key_len ? -1 : a->key_len > b->key_len ? 1 : 0;
}

static int json_keys_unique(yyjson_val *value) {
  if (yyjson_is_obj(value)) {
    size_t count=yyjson_obj_size(value),index=0U;
    JSON_PAIR *pairs=count==0U?NULL:(JSON_PAIR *)calloc(count,sizeof(*pairs));
    yyjson_obj_iter iterator=yyjson_obj_iter_with(value);yyjson_val *key;int unique=1;
    if(count>0U&&pairs==NULL)return 0;
    while((key=yyjson_obj_iter_next(&iterator))!=NULL){pairs[index].key=yyjson_get_str(key);pairs[index].key_len=yyjson_get_len(key);pairs[index].value=yyjson_obj_iter_get_val(key);index++;}
    if (count > 1U) qsort(pairs,count,sizeof(*pairs),pair_compare);
    for(index=0U;index<count;index++){
      if(index>0U&&pairs[index-1U].key_len==pairs[index].key_len&&memcmp(pairs[index-1U].key,pairs[index].key,pairs[index].key_len)==0){unique=0;break;}
      if(!json_keys_unique(pairs[index].value)){unique=0;break;}
    }
    free(pairs);return unique;
  }
  if(yyjson_is_arr(value)){yyjson_arr_iter iterator;yyjson_val *item;yyjson_arr_iter_init(value,&iterator);while((item=yyjson_arr_iter_next(&iterator))!=NULL)if(!json_keys_unique(item))return 0;}
  return 1;
}

static yyjson_mut_val *canonical_copy(yyjson_mut_doc *target, yyjson_val *value) {
  if (yyjson_is_obj(value)) {
    size_t count = yyjson_obj_size(value), index = 0U;
    JSON_PAIR *pairs = count == 0U ? NULL : (JSON_PAIR *)calloc(count, sizeof(*pairs));
    yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
    yyjson_val *key;
    yyjson_mut_val *object = yyjson_mut_obj(target);
    if ((count > 0U && pairs == NULL) || object == NULL) { free(pairs); return NULL; }
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
      pairs[index].key = yyjson_get_str(key); pairs[index].key_len = yyjson_get_len(key);
      pairs[index].value = yyjson_obj_iter_get_val(key); index++;
    }
    if (count > 1U) qsort(pairs, count, sizeof(*pairs), pair_compare);
    for (index = 0U; index < count; index++) {
      yyjson_mut_val *child = canonical_copy(target, pairs[index].value);
      if (child == NULL || !yyjson_mut_obj_add_val(target, object, pairs[index].key, child)) {
        free(pairs); return NULL;
      }
    }
    free(pairs); return object;
  }
  if (yyjson_is_arr(value)) {
    yyjson_mut_val *array = yyjson_mut_arr(target); yyjson_arr_iter iterator;
    yyjson_val *item;
    if (array == NULL) return NULL;
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      yyjson_mut_val *child = canonical_copy(target, item);
      if (child == NULL || !yyjson_mut_arr_append(array, child)) return NULL;
    }
    return array;
  }
  return yyjson_val_mut_copy(target, value);
}

static char *canonical_json(yyjson_val *value) {
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root;
  char *json;
  if (document == NULL) return NULL;
  root = canonical_copy(document, value);
  if (root == NULL) { yyjson_mut_doc_free(document); return NULL; }
  yyjson_mut_doc_set_root(document, root);
  json = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, NULL);
  yyjson_mut_doc_free(document);
  return json;
}

static int only_keys(yyjson_val *object, const char *const *allowed) {
  yyjson_obj_iter iterator = yyjson_obj_iter_with(object); yyjson_val *key;
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key); size_t i; int found = 0;
    for (i=0U; allowed[i] != NULL; i++) if (strcmp(name, allowed[i]) == 0) { found=1; break; }
    if (!found) return 0;
  }
  return 1;
}

static int assign_string(yyjson_val *object, const char *key, int required, size_t max_bytes,
                         char **output, char *error, size_t error_size) {
  yyjson_val *value = yyjson_obj_get(object, key);
  if (value == NULL) return required ? YAP_V2_INVALID_FORMAT : YAP_V2_OK;
  if (!yyjson_is_str(value)) return YAP_V2_INVALID_FORMAT;
  if (yyjson_get_len(value) > max_bytes) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "%s exceeds maximum of %zu bytes (got %zu)",
                     key, max_bytes, yyjson_get_len(value));
    return YAP_V2_OUT_OF_RANGE;
  }
  *output = copy_text(yyjson_get_str(value), yyjson_get_len(value));
  return *output == NULL ? YAP_V2_ALLOCATION_FAILED : YAP_V2_OK;
}

int YAP_V2_ingest_parse_ndjson(const char *line, size_t length, YAP_V2_INGEST_OPERATION *operation,
                               char *error, size_t error_size) {
  static const char *const keys[]={"operation","id","url","title","body","metadata","updated_at_unix_ms","vectors",NULL};
  yyjson_read_err read_error; yyjson_doc *document; yyjson_val *root,*kind,*metadata,*updated,*vectors;
  int status;
  if(line==NULL||operation==NULL)return YAP_V2_INVALID_ARGUMENT;
  if(error!=NULL&&error_size>0U)error[0]='\0';
  memset(operation,0,sizeof(*operation));
  document=yyjson_read_opts((char *)line,length,YYJSON_READ_NOFLAG,NULL,&read_error);
  if(document==NULL){error_set(error,error_size,"invalid JSON");return YAP_V2_INVALID_FORMAT;}
  root=yyjson_doc_get_root(document);
  if(!yyjson_is_obj(root)||!only_keys(root,keys)||!json_keys_unique(root)){error_set(error,error_size,"record is not an object, has duplicate keys, or contains an unknown key");status=YAP_V2_INVALID_FORMAT;goto done;}
  kind=yyjson_obj_get(root,"operation");
  if(!yyjson_is_str(kind)){error_set(error,error_size,"operation must be a string");status=YAP_V2_INVALID_FORMAT;goto done;}
  if(strcmp(yyjson_get_str(kind),"upsert")==0)operation->kind=YAP_V2_INGEST_UPSERT;
  else if(strcmp(yyjson_get_str(kind),"delete")==0)operation->kind=YAP_V2_INGEST_DELETE;
  else{error_set(error,error_size,"operation must be upsert or delete");status=YAP_V2_INVALID_FORMAT;goto done;}
  status=assign_string(root,"id",1,YAP_V2_MAX_IDENTIFIER_BYTES,&operation->id,error,error_size);if(status!=YAP_V2_OK||operation->id[0]=='\0')goto invalid;
  if(operation->kind==YAP_V2_INGEST_DELETE){
    if(yyjson_obj_size(root)!=2U){status=YAP_V2_INVALID_FORMAT;goto invalid;}
    status=YAP_V2_OK;goto done;
  }
  status=assign_string(root,"url",0,YAP_V2_MAX_URL_BYTES,&operation->url,error,error_size);if(status!=YAP_V2_OK)goto invalid;
  status=assign_string(root,"title",0,YAP_V2_MAX_IDENTIFIER_BYTES,&operation->title,error,error_size);if(status!=YAP_V2_OK)goto invalid;
  status=assign_string(root,"body",1,YAP_V2_MAX_METADATA_BYTES,&operation->body,error,error_size);if(status!=YAP_V2_OK)goto invalid;
  if(operation->url==NULL)operation->url=copy_text("",0U);
  if(operation->title==NULL)operation->title=copy_text("",0U);
  metadata=yyjson_obj_get(root,"metadata");
  if(metadata==NULL){yyjson_doc *empty=yyjson_read("{}",2,0);operation->metadata_json=canonical_json(yyjson_doc_get_root(empty));yyjson_doc_free(empty);}
  else if(yyjson_is_obj(metadata))operation->metadata_json=canonical_json(metadata);
  else goto invalid;
  if(operation->url==NULL||operation->title==NULL||operation->metadata_json==NULL){status=YAP_V2_ALLOCATION_FAILED;goto done;}
  updated=yyjson_obj_get(root,"updated_at_unix_ms");
  if(updated!=NULL){if(!yyjson_is_int(updated))goto invalid;operation->updated_at_unix_ms=yyjson_get_sint(updated);}
  vectors=yyjson_obj_get(root,"vectors");
  if(vectors!=NULL){
    yyjson_arr_iter outer;yyjson_val *row;size_t rows,dimensions=0U,index=0U;
    if(!yyjson_is_arr(vectors)||(rows=yyjson_arr_size(vectors))==0U||rows>YAP_V2_MAX_SEGMENT_PASSAGES)goto invalid;
    yyjson_arr_iter_init(vectors,&outer);
    while((row=yyjson_arr_iter_next(&outer))!=NULL){
      size_t current;if(!yyjson_is_arr(row)||(current=yyjson_arr_size(row))==0U||current>YAP_V2_MAX_VECTOR_DIMENSIONS)goto invalid;
      if(dimensions==0U)dimensions=current;else if(current!=dimensions)goto invalid;
    }
    if(rows>SIZE_MAX/dimensions||rows*dimensions>SIZE_MAX/sizeof(float)){status=YAP_V2_OUT_OF_RANGE;goto done;}
    operation->vectors=malloc(rows*dimensions*sizeof(float));if(operation->vectors==NULL){status=YAP_V2_ALLOCATION_FAILED;goto done;}
    yyjson_arr_iter_init(vectors,&outer);
    while((row=yyjson_arr_iter_next(&outer))!=NULL){
      yyjson_arr_iter inner;yyjson_val *number;yyjson_arr_iter_init(row,&inner);
      while((number=yyjson_arr_iter_next(&inner))!=NULL){double value;if(!yyjson_is_num(number)||(value=yyjson_get_num(number),!isfinite(value))||value>FLT_MAX||value<-FLT_MAX)goto invalid;operation->vectors[index++]=(float)value;}
    }
    operation->vector_count=rows;operation->vector_dimensions=dimensions;
  }
  status=YAP_V2_OK;goto done;
invalid:
  if(status==YAP_V2_OK)status=YAP_V2_INVALID_FORMAT;
  if(error==NULL||error_size==0U||error[0]=='\0')error_set(error,error_size,"record violates the canonical ingest schema");
done:
  yyjson_doc_free(document);
  if(status!=YAP_V2_OK)YAP_V2_ingest_operation_free(operation);
  return status;
}

int YAP_V2_ingest_parse_tsv(char *line,YAP_V2_INGEST_OPERATION *operation,char *error,size_t error_size){
  char *fields[5],*tab,*end;long body_size;int i;
  if(line==NULL||operation==NULL)return YAP_V2_INVALID_ARGUMENT;
  memset(operation,0,sizeof(*operation));fields[0]=line;
  for(i=0;i<4;i++){tab=strchr(fields[i],'\t');if(tab==NULL)goto invalid;*tab='\0';fields[i+1]=tab+1;}
  end=fields[4]+strlen(fields[4]);while(end>fields[4]&&(end[-1]=='\n'||end[-1]=='\r'))*--end='\0';
  errno=0;body_size=strtol(fields[3],&end,10);
  if(errno!=0||*end!='\0'||body_size<0||(size_t)body_size!=strlen(fields[4])||fields[0][0]=='\0')goto invalid;
  operation->id=copy_text(fields[0],strlen(fields[0]));if(operation->id==NULL)return YAP_V2_ALLOCATION_FAILED;
  if(strcmp(fields[1],"DELETE")==0){operation->kind=YAP_V2_INGEST_DELETE;return YAP_V2_OK;}
  if(strcmp(fields[1],"ADD")!=0)goto invalid_free;
  operation->kind=YAP_V2_INGEST_UPSERT;operation->url=copy_text(fields[0],strlen(fields[0]));operation->title=copy_text(fields[2],strlen(fields[2]));operation->body=copy_text(fields[4],strlen(fields[4]));operation->metadata_json=copy_text("{}",2U);
  if(operation->url==NULL||operation->title==NULL||operation->body==NULL||operation->metadata_json==NULL){YAP_V2_ingest_operation_free(operation);return YAP_V2_ALLOCATION_FAILED;}
  return YAP_V2_OK;
invalid_free:YAP_V2_ingest_operation_free(operation);
invalid:error_set(error,error_size,"invalid TSV record");return YAP_V2_INVALID_FORMAT;
}

static int write_json_string(FILE *output,const char *value){yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=yyjson_mut_strcpy(doc,value);char *json;if(doc==NULL||root==NULL){if(doc)yyjson_mut_doc_free(doc);return -1;}yyjson_mut_doc_set_root(doc,root);json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);if(json==NULL)return -1;fputs(json,output);free(json);return 0;}

int YAP_V2_prepare_main(int argc,char **argv){
  const char *config_path=NULL,*input_path=NULL,*output_path=NULL,*format="ndjson";FILE *input=NULL,*output=NULL;char *line=NULL;size_t capacity=0U;ssize_t length;YAP_APPLICATION_CONFIG application;char error[256];int i,result=EXIT_FAILURE;
  for(i=1;i<argc;i++){const char **target=NULL;if(strcmp(argv[i],"--config")==0)target=&config_path;else if(strcmp(argv[i],"--input")==0)target=&input_path;else if(strcmp(argv[i],"--output")==0)target=&output_path;else if(strcmp(argv[i],"--input-format")==0)target=&format;else{fprintf(stderr,"Unknown prepare option: %s\n",argv[i]);goto done;}if(++i>=argc){fputs("Missing prepare option value\n",stderr);goto done;}*target=argv[i];}
  if(config_path==NULL||input_path==NULL||output_path==NULL||(strcmp(format,"ndjson")!=0&&strcmp(format,"tsv")!=0)){fputs("Usage: yappo_makeindex prepare --config FILE --input FILE --output FILE [--input-format ndjson|tsv]\n",stderr);goto done;}
  if(YAP_application_config_load(config_path,&application,error,sizeof(error))!=YAP_V2_OK){fprintf(stderr,"Config error: %s\n",error);goto done;}
  input=fopen(input_path,"r");output=fopen(output_path,"w");if(input==NULL||output==NULL){perror("prepare");goto done;}
  while((length=getline(&line,&capacity,input))>=0){YAP_V2_INGEST_OPERATION operation;YAP_V2_CHUNK_SEQUENCE chunks;size_t j;int status;if(length==0)continue;status=strcmp(format,"tsv")==0?YAP_V2_ingest_parse_tsv(line,&operation,error,sizeof(error)):YAP_V2_ingest_parse_ndjson(line,(size_t)length,&operation,error,sizeof(error));if(status!=YAP_V2_OK){fprintf(stderr,"Invalid input: %s\n",error);goto done;}if(operation.kind==YAP_V2_INGEST_DELETE){fputs("{\"operation\":\"delete\",\"id\":",output);write_json_string(output,operation.id);fputs("}\n",output);}else{status=YAP_V2_unicode_chunk(operation.id,operation.body,strlen(operation.body),application.index_config.chunk_max_chars,application.index_config.chunk_overlap_chars,&chunks);if(status!=YAP_V2_OK){YAP_V2_ingest_operation_free(&operation);goto done;}for(j=0;j<chunks.chunk_count;j++){fputs("{\"operation\":\"upsert\",\"document_id\":",output);write_json_string(output,operation.id);fputs(",\"passage_id\":",output);write_json_string(output,chunks.chunks[j].id);fprintf(output,",\"ordinal\":%u,\"start_char\":%u,\"end_char\":%u,\"text\":",chunks.chunks[j].ordinal,chunks.chunks[j].start_char,chunks.chunks[j].end_char);write_json_string(output,chunks.chunks[j].text);fputs(",\"metadata\":",output);fputs(operation.metadata_json,output);fputs("}\n",output);}YAP_V2_chunk_sequence_free(&chunks);}YAP_V2_ingest_operation_free(&operation);}
  if (ferror(input) || fflush(output) != 0) {
    goto done;
  }
  result = EXIT_SUCCESS;
done:free(line);if(input)fclose(input);if(output)fclose(output);return result;
}
