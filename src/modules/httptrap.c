/*
 * Copyright (c) 2011, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2011-2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <mtev_defines.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <yajl/yajl_parse.h>

#include <mtev_rest.h>
#include <mtev_hash.h>

#include "noit_module.h"
#include "noit_check.h"
#include "noit_check_tools.h"
#include "noit_mtev_bridge.h"

#define DEFAULT_HTTPTRAP_DELIMITER '`'
#define MAX_DEPTH 32

static mtev_log_stream_t nlerr = NULL;
static mtev_log_stream_t nldeb = NULL;

typedef struct _mod_config {
  mtev_hash_table *options;
  mtev_boolean asynch_metrics;
} httptrap_mod_config_t;

typedef struct httptrap_closure_s {
  noit_module_t *self;
  int stats_count;
} httptrap_closure_t;

struct value_list {
  char *v;
  struct value_list *next;
};
struct rest_json_payload {
  noit_check_t *check;
  yajl_handle parser;
  int len;
  int complete;
  char delimiter;
  char *error;
  int depth;
  char *keys[MAX_DEPTH];
  char array_depth[MAX_DEPTH];
  unsigned char last_special_key;
  unsigned char saw_complex_type;
  metric_type_t last_type;
  struct value_list *last_value;
  int cnt;
  mtev_boolean immediate;
};

#define NEW_LV(json,a) do { \
  struct value_list *nlv = malloc(sizeof(*nlv)); \
  nlv->v = a; \
  nlv->next = json->last_value; \
  json->last_value = nlv; \
} while(0)

static mtev_boolean
mtev_httptrap_check_aynsch(noit_module_t *self,
                           noit_check_t *check) {
  const char *config_val;
  httptrap_mod_config_t *conf;
  if(!self) return mtev_true;
  conf = noit_module_get_userdata(self);
  if(!conf) return mtev_true;
  mtev_boolean is_asynch = conf->asynch_metrics;
  if(mtev_hash_retr_str(check->config,
                        "asynch_metrics", strlen("asynch_metrics"),
                        (const char **)&config_val)) {
    if(!strcasecmp(config_val, "false") || !strcasecmp(config_val, "off"))
      is_asynch = mtev_false;
    else if(!strcasecmp(config_val, "true") || !strcasecmp(config_val, "on"))
      is_asynch = mtev_true;
  }

  if(is_asynch) check->flags |= NP_SUPPRESS_METRICS;
  else check->flags &= ~NP_SUPPRESS_METRICS;
  return is_asynch;
}

static void
set_array_key(struct rest_json_payload *json) {
  if(json->array_depth[json->depth] > 0) {
    char str[256];
    int strLen;
    snprintf(str, sizeof(str), "%d", json->array_depth[json->depth] - 1);
    json->array_depth[json->depth]++;
    strLen = strlen(str);
    if(json->keys[json->depth]) free(json->keys[json->depth]);
    json->keys[json->depth] = NULL;
    if(json->depth == 0) {
      json->keys[json->depth] = malloc(strLen+1);
      memcpy(json->keys[json->depth], str, strLen);
      json->keys[json->depth][strLen] = '\0';
    }
    else {
      int uplen = strlen(json->keys[json->depth-1]);
      if(uplen + 1 + strLen > 255) return;
      json->keys[json->depth] = malloc(uplen + 1 + strLen + 1);
      memcpy(json->keys[json->depth], json->keys[json->depth-1], uplen);
      json->keys[json->depth][uplen] = json->delimiter;
      memcpy(json->keys[json->depth] + uplen + 1, str, strLen);
      json->keys[json->depth][uplen + 1 + strLen] = '\0';
    }
  }
}
static int
httptrap_yajl_cb_null(void *ctx) {
  struct rest_json_payload *json = ctx;
  if(json->depth<0) return 0;
  set_array_key(json);
  if(json->last_special_key == 0x2) {
    NEW_LV(json, NULL);
    return 1;
  }
  if(json->last_special_key) return 0;
  noit_stats_set_metric(json->check,
      json->keys[json->depth], METRIC_INT32, NULL);
  if(json->immediate)
    noit_stats_log_immediate_metric(json->check,
        json->keys[json->depth], METRIC_INT32, NULL);
  json->cnt++;
  return 1;
}
static int
httptrap_yajl_cb_boolean(void *ctx, int boolVal) {
  int ival;
  struct rest_json_payload *json = ctx;
  if(json->depth<0) return 0;
  set_array_key(json);
  if(json->last_special_key == 0x2) {
    NEW_LV(json, strdup(boolVal ? "1" : "0"));
    return 1;
  }
  if(json->last_special_key) return 0;
  ival = boolVal ? 1 : 0;
  noit_stats_set_metric(json->check,
      json->keys[json->depth], METRIC_INT32, &ival);
  if(json->immediate)
    noit_stats_log_immediate_metric(json->check,
        json->keys[json->depth], METRIC_INT32, &ival);
  json->cnt++;
  return 1;
}
static int
httptrap_yajl_cb_number(void *ctx, const char * numberVal,
                        size_t numberLen) {
  char val[128];
  struct rest_json_payload *json = ctx;
  if(json->depth<0) return 0;
  set_array_key(json);
  if(json->last_special_key == 0x2) {
    char *str;
    str = malloc(numberLen+1);
    memcpy(str, numberVal, numberLen);
    str[numberLen] = '\0';
    NEW_LV(json, str);
    return 1;
  }
  if(json->last_special_key) return 0;
  if(numberLen > sizeof(val)-1) numberLen = sizeof(val)-1;
  memcpy(val, numberVal, numberLen);
  val[numberLen] = '\0';
  noit_stats_set_metric(json->check,
      json->keys[json->depth], METRIC_GUESS, val);
  if(json->immediate)
    noit_stats_log_immediate_metric(json->check,
        json->keys[json->depth], METRIC_GUESS, val);
  json->cnt++;
  return 1;
}
static int
httptrap_yajl_cb_string(void *ctx, const unsigned char * stringVal,
                        size_t stringLen) {
  struct rest_json_payload *json = ctx;
  char val[4096];
  if(json->depth<0) return 0;
  set_array_key(json);
  if(json->last_special_key == 0x1) {
    if(stringLen != 1) return 0;
    if(*stringVal == 'L' || *stringVal == 'l' ||
        *stringVal == 'I' || *stringVal == 'i' ||
        *stringVal == 'n' || *stringVal == 's') {
      json->last_type = *stringVal;
      json->saw_complex_type |= 0x1;
      return 1;
    }
    return 0;
  }
  else if(json->last_special_key == 0x2) {
    char *str;
    str = malloc(stringLen+1);
    memcpy(str, stringVal, stringLen);
    str[stringLen] = '\0';
    NEW_LV(json, str);
    json->saw_complex_type |= 0x2;
    return 1;
  }
  if(stringLen > sizeof(val)-1) stringLen = sizeof(val)-1;
  memcpy(val, stringVal, stringLen);
  val[stringLen] = '\0';
  noit_stats_set_metric(json->check,
      json->keys[json->depth], METRIC_GUESS, val);
  if(json->immediate)
    noit_stats_log_immediate_metric(json->check,
        json->keys[json->depth], METRIC_GUESS, val);
  json->cnt++;
  return 1;
}
static int
httptrap_yajl_cb_start_map(void *ctx) {
  struct rest_json_payload *json = ctx;
  set_array_key(json);
  json->depth++;
  if(json->depth >= MAX_DEPTH) return 0;
  return 1;
}
static int
httptrap_yajl_cb_end_map(void *ctx) {
  struct value_list *p, *last_p = NULL;
  struct rest_json_payload *json = ctx;
  json->depth--;
  if(json->saw_complex_type == 0x3) {
    long double total = 0, cnt = 0;
    mtev_boolean use_avg = mtev_false;
    for(p=json->last_value;p;p=p->next) {
      noit_stats_set_metric_coerce(json->check,
          json->keys[json->depth], json->last_type, p->v);
      last_p = p;
      if(p->v != NULL &&
         (json->last_type == 'L' || json->last_type == 'l' ||
          json->last_type == 'I' || json->last_type == 'i' ||
          json->last_type == 'n')) {
        total += strtold(p->v, NULL);
        cnt = cnt + 1;
        use_avg = mtev_true;
      }
      json->cnt++;
    }
    if(json->immediate && last_p != NULL) {
      if(use_avg) {
        double avg = total / cnt;
        noit_stats_log_immediate_metric(json->check,
            json->keys[json->depth], 'n', &avg);
      }
      else {
        noit_stats_log_immediate_metric(json->check,
            json->keys[json->depth], json->last_type, last_p->v);
      }
    }
  }
  json->saw_complex_type = 0;
  for(p=json->last_value;p;) {
    struct value_list *savenext;
    savenext = p->next;
    if(p->v) free(p->v);
    savenext = p->next;
    free(p);
    p = savenext;
  }
  json->last_value = NULL;
  return 1;
}
static int
httptrap_yajl_cb_start_array(void *ctx) {
  struct rest_json_payload *json = ctx;
  json->depth++;
  json->array_depth[json->depth]++;
  return 1;
}
static int
httptrap_yajl_cb_end_array(void *ctx) {
  struct rest_json_payload *json = ctx;
  json->array_depth[json->depth] = 0;
  json->depth--;
  return 1;
}
static int
httptrap_yajl_cb_map_key(void *ctx, const unsigned char * key,
                         size_t stringLen) {
  struct rest_json_payload *json = ctx;
  if(stringLen > 255) return 0;
  if(json->keys[json->depth]) free(json->keys[json->depth]);
  json->keys[json->depth] = NULL;
  if(stringLen == 5 && memcmp(key, "_type", 5) == 0) {
    json->last_special_key = 0x1;
    if(json->depth > 0) json->keys[json->depth] = strdup(json->keys[json->depth-1]);
    return 1;
  }
  if(stringLen == 6 && memcmp(key, "_value", 6) == 0) {
    if(json->depth > 0) json->keys[json->depth] = strdup(json->keys[json->depth-1]);
    json->last_special_key = 0x2;
    json->saw_complex_type |= 0x2;
    return 1;
  }
  json->last_special_key = 0;
  if(json->depth == 0) {
    json->keys[json->depth] = malloc(stringLen+1);
    memcpy(json->keys[json->depth], key, stringLen);
    json->keys[json->depth][stringLen] = '\0';
  }
  else {
    int uplen = strlen(json->keys[json->depth-1]);
    if(uplen + 1 + stringLen > 255) return 0;
    json->keys[json->depth] = malloc(uplen + 1 + stringLen + 1);
    memcpy(json->keys[json->depth], json->keys[json->depth-1], uplen);
    json->keys[json->depth][uplen] = json->delimiter;
    memcpy(json->keys[json->depth] + uplen + 1, key, stringLen);
    json->keys[json->depth][uplen + 1 + stringLen] = '\0';
  }
  return 1;
}
static yajl_callbacks httptrap_yajl_callbacks = {
  .yajl_null = httptrap_yajl_cb_null,
  .yajl_boolean = httptrap_yajl_cb_boolean,
  .yajl_number = httptrap_yajl_cb_number,
  .yajl_string = httptrap_yajl_cb_string,
  .yajl_start_map = httptrap_yajl_cb_start_map,
  .yajl_map_key = httptrap_yajl_cb_map_key,
  .yajl_end_map = httptrap_yajl_cb_end_map,
  .yajl_start_array = httptrap_yajl_cb_start_array,
  .yajl_end_array = httptrap_yajl_cb_end_array
};

static void
rest_json_payload_free(void *f) {
  int i;
  struct rest_json_payload *json = f;
  if(json->parser) yajl_free(json->parser);
  if(json->error) free(json->error);
  for(i=0;i<MAX_DEPTH;i++)
    if(json->keys[i]) free(json->keys[i]);
  if(json->last_value) free(json->last_value);
  free(json);
}

static struct rest_json_payload *
rest_get_json_upload(mtev_http_rest_closure_t *restc,
                    int *mask, int *complete) {
  struct rest_json_payload *rxc;
  mtev_http_request *req = mtev_http_session_request(restc->http_ctx);
  httptrap_closure_t *ccl;
  int content_length;
  char buffer[32768];

  content_length = mtev_http_request_content_length(req);
  rxc = restc->call_closure;
  ccl = rxc->check->closure;
  rxc->immediate = mtev_httptrap_check_aynsch(ccl->self, rxc->check);
  while(!rxc->complete) {
    int len;
    len = mtev_http_session_req_consume(
            restc->http_ctx, buffer,
            MIN(content_length - rxc->len, sizeof(buffer)),
            sizeof(buffer),
            mask);
    if(len > 0) {
      yajl_status status;
      status = yajl_parse(rxc->parser, (unsigned char *)buffer, len);
      if(status != yajl_status_ok) {
        unsigned char *err;
        *complete = 1;
        err = yajl_get_error(rxc->parser, 0, (unsigned char *)buffer, len);
        rxc->error = strdup((char *)err);
        yajl_free_error(rxc->parser, err);
        return rxc;
      }
      rxc->len += len;
    }
    if(len < 0 && errno == EAGAIN) return NULL;
    else if(len < 0) {
      *complete = 1;
      return NULL;
    }
    content_length = mtev_http_request_content_length(req);
    if((mtev_http_request_payload_chunked(req) && len == 0) ||
       (rxc->len == content_length)) {
      rxc->complete = 1;
      yajl_complete_parse(rxc->parser);
    }
  }

  *complete = 1;
  return rxc;
}

static int httptrap_submit(noit_module_t *self, noit_check_t *check,
                           noit_check_t *cause) {
  httptrap_closure_t *ccl;
  struct timeval duration;
  /* We are passive, so we don't do anything for transient checks */
  if(check->flags & NP_TRANSIENT) return 0;

  mtev_httptrap_check_aynsch(self, check);
  if(!check->closure) {
    ccl = check->closure = (void *)calloc(1, sizeof(httptrap_closure_t)); 
    memset(ccl, 0, sizeof(httptrap_closure_t));
    ccl->self = self;
  } else {
    // Don't count the first run
    struct timeval now, *last;
    stats_t *inprogress;
    char human_buffer[256];
    ccl = (httptrap_closure_t*)check->closure;
    gettimeofday(&now, NULL);
    sub_timeval(now, check->last_fire_time, &duration);
    noit_stats_set_whence(check, &now);
    noit_stats_set_duration(check, duration.tv_sec * 1000 + duration.tv_usec / 1000);

    snprintf(human_buffer, sizeof(human_buffer),
             "dur=%ld,run=%d,stats=%d", duration.tv_sec * 1000 + duration.tv_usec / 1000,
             check->generation, ccl->stats_count);
    mtevL(nldeb, "httptrap(%s) [%s]\n", check->target, human_buffer);

    // Not sure what to do here
    noit_stats_set_available(check, (ccl->stats_count > 0) ?
        NP_AVAILABLE : NP_UNAVAILABLE);
    noit_stats_set_state(check, (ccl->stats_count > 0) ?
        NP_GOOD : NP_BAD);
    noit_stats_set_status(check, human_buffer);
    if(check->last_fire_time.tv_sec)
      noit_check_passive_set_stats(check);

    inprogress = noit_check_get_stats_inprogress(check);
    last = noit_check_stats_whence(inprogress, NULL);
    memcpy(&check->last_fire_time, last, sizeof(duration));
  }
  ccl->stats_count = 0;
  return 0;
}

static int
push_payload_at_check(struct rest_json_payload *rxc) {
  httptrap_closure_t *ccl;
  mtev_boolean immediate;
  char key[256];

  if (!rxc->check || strcmp(rxc->check->module, "httptrap")) return 0;
  if (rxc->check->closure == NULL) return 0;
  ccl = rxc->check->closure;
  immediate = mtev_httptrap_check_aynsch(ccl->self,rxc->check);

  /* do it here */
  ccl->stats_count = rxc->cnt;
  return rxc->cnt;
}

static int
rest_httptrap_handler(mtev_http_rest_closure_t *restc,
                      int npats, char **pats) {
  int mask, complete = 0, cnt;
  struct rest_json_payload *rxc = NULL;
  const char *error = "internal error", *secret = NULL;
  mtev_http_session_ctx *ctx = restc->http_ctx;
  char json_out[128];
  noit_check_t *check;
  uuid_t check_id;

  if(npats != 2) {
    error = "bad uri";
    goto error;
  }
  if(uuid_parse(pats[0], check_id)) {
    error = "uuid parse error";
    goto error;
  }

  if(restc->call_closure == NULL) {
    httptrap_closure_t *ccl;
    const char *delimiter = NULL;
    rxc = restc->call_closure = calloc(1, sizeof(*rxc));
    rxc->delimiter = DEFAULT_HTTPTRAP_DELIMITER;
    check = noit_poller_lookup(check_id);
    if(!check || strcmp(check->module, "httptrap")) {
      error = "no such httptrap check";
      goto error;
    }
    (void)mtev_hash_retr_str(check->config, "secret", strlen("secret"), &secret);
    if(!secret) secret = "";
    if(strcmp(pats[1], secret)) {
      error = "secret mismatch";
      goto error;
    }
    (void)mtev_hash_retr_str(check->config, "delimiter", strlen("delimiter"), &delimiter);
    if(delimiter && *delimiter) rxc->delimiter = *delimiter;
    rxc->check = check;
    ccl = check->closure;
    if(!ccl) {
      error = "noitd is booting, try again in a bit";
      goto error;
    }
    rxc->parser = yajl_alloc(&httptrap_yajl_callbacks, NULL, rxc);
    rxc->depth = -1;
    yajl_config(rxc->parser, yajl_allow_comments, 1);
    yajl_config(rxc->parser, yajl_dont_validate_strings, 1);
    yajl_config(rxc->parser, yajl_allow_trailing_garbage, 1);
    yajl_config(rxc->parser, yajl_allow_partial_values, 1);
    restc->call_closure_free = rest_json_payload_free;
  }
  else rxc = restc->call_closure;

  /* flip threads */
  {
    mtev_http_connection *conn = mtev_http_session_connection(ctx);
    eventer_t e = mtev_http_connection_event(conn);
    if(e) {
      pthread_t tgt = CHOOSE_EVENTER_THREAD_FOR_CHECK(rxc->check);
      if(!pthread_equal(e->thr_owner, tgt)) {
        e->thr_owner = tgt;
        return EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION;
      }
    }
  }

  rxc = rest_get_json_upload(restc, &mask, &complete);
  if(rxc == NULL && !complete) return mask;

  if(!rxc) goto error;
  if(rxc->error) goto error;

  cnt = push_payload_at_check(rxc);

  mtev_http_response_ok(ctx, "application/json");
  snprintf(json_out, sizeof(json_out),
           "{ \"stats\": %d }", cnt);
  mtev_http_response_append(ctx, json_out, strlen(json_out));
  mtev_http_response_end(ctx);
  return 0;

 error:
  mtev_http_response_server_error(ctx, "application/json");
  mtev_http_response_append(ctx, "{ error: \"", 10);
  if(rxc && rxc->error) error = rxc->error;
  mtev_http_response_append(ctx, error, strlen(error));
  mtev_http_response_append(ctx, "\" }", 3);
  mtev_http_response_end(ctx);
  return 0;
}

static int mtev_httptrap_initiate_check(noit_module_t *self,
                                        noit_check_t *check,
                                        int once, noit_check_t *cause) {
  check->flags |= NP_PASSIVE_COLLECTION;
  if (check->closure == NULL) {
    httptrap_closure_t *ccl;
    ccl = check->closure = (void *)calloc(1, sizeof(httptrap_closure_t));
    ccl->self = self;
  }
  INITIATE_CHECK(httptrap_submit, self, check, cause);
  return 0;
}

static int mtev_httptrap_config(noit_module_t *self, mtev_hash_table *options) {
  httptrap_mod_config_t *conf;
  conf = noit_module_get_userdata(self);
  if(conf) {
    if(conf->options) {
      mtev_hash_destroy(conf->options, free, free);
      free(conf->options);
    }
  }
  else
    conf = calloc(1, sizeof(*conf));
  conf->options = options;
  noit_module_set_userdata(self, conf);
  return 1;
}

static int mtev_httptrap_onload(mtev_image_t *self) {
  if(!nlerr) nlerr = mtev_log_stream_find("error/httptrap");
  if(!nldeb) nldeb = mtev_log_stream_find("debug/httptrap");
  if(!nlerr) nlerr = noit_error;
  if(!nldeb) nldeb = noit_debug;
  return 0;
}

static int mtev_httptrap_init(noit_module_t *self) {
  const char *config_val;
  httptrap_mod_config_t *conf;
  conf = noit_module_get_userdata(self);

  conf->asynch_metrics = mtev_true;
  if(mtev_hash_retr_str(conf->options,
                        "asynch_metrics", strlen("asynch_metrics"),
                        (const char **)&config_val)) {
    if(!strcasecmp(config_val, "false") || !strcasecmp(config_val, "off"))
      conf->asynch_metrics = mtev_false;
  }

  noit_module_set_userdata(self, conf);

  /* register rest handler */
  mtev_http_rest_register("PUT", "/module/httptrap/",
                          "^(" UUID_REGEX ")/([^/]*).*$",
                          rest_httptrap_handler);
  mtev_http_rest_register("POST", "/module/httptrap/",
                          "^(" UUID_REGEX ")/([^/]*).*$",
                          rest_httptrap_handler);
  return 0;
}

#include "httptrap.xmlh"
noit_module_t httptrap = {
  {
    .magic = NOIT_MODULE_MAGIC,
    .version = NOIT_MODULE_ABI_VERSION,
    .name = "httptrap",
    .description = "httptrap collection",
    .xml_description = httptrap_xml_description,
    .onload = mtev_httptrap_onload
  },
  mtev_httptrap_config,
  mtev_httptrap_init,
  mtev_httptrap_initiate_check,
  NULL
};
