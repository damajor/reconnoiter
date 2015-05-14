/*
 * Copyright (c) 2007-2011, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2015, Circonus, Inc. All rights reserved.
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <mtev_defines.h>
#include <eventer/eventer.h>
#include <mtev_listener.h>
#include <mtev_hash.h>
#include <mtev_memory.h>
#include <mtev_rest.h>

#include <jlog.h>
#include <jlog_private.h>
#include "noit_mtev_bridge.h"
#include "noit_jlog_listener.h"

#include <unistd.h>
#include <poll.h>
#include <assert.h>
#define MAX_ROWS_AT_ONCE 1000
#define DEFAULT_MSECONDS_BETWEEN_BATCHES 10000
#define DEFAULT_TRANSIENT_MSECONDS_BETWEEN_BATCHES 500

static mtev_atomic32_t tmpfeedcounter = 0;
static int rest_show_feed(mtev_http_rest_closure_t *restc,
                          int npats, char **pats);
static int rest_delete_feed(mtev_http_rest_closure_t *restc,
                            int npats, char **pats);

void
noit_jlog_listener_init() {
  eventer_name_callback("log_transit/1.0", noit_jlog_handler);
  mtev_control_dispatch_delegate(mtev_control_dispatch,
                                 NOIT_JLOG_DATA_FEED,
                                 noit_jlog_handler);
  mtev_control_dispatch_delegate(mtev_control_dispatch,
                                 NOIT_JLOG_DATA_TEMP_FEED,
                                 noit_jlog_handler);
  assert(mtev_http_rest_register_auth(
    "GET", "/", "^feed$",
    rest_show_feed, mtev_http_rest_client_cert_auth
  ) == 0);
  assert(mtev_http_rest_register_auth(
    "DELETE", "/feed/", "^(.+)$",
    rest_delete_feed, mtev_http_rest_client_cert_auth
  ) == 0);
}

typedef struct {
  jlog_ctx *jlog;
  char *subscriber;
  jlog_id chkpt;
  jlog_id start;
  jlog_id finish;
  jlog_feed_stats_t *feed_stats;
  int count;
  int wants_shutdown;
} noit_jlog_closure_t;

noit_jlog_closure_t *
noit_jlog_closure_alloc(void) {
  noit_jlog_closure_t *jcl;
  jcl = calloc(1, sizeof(*jcl));
  return jcl;
}

void
noit_jlog_closure_free(noit_jlog_closure_t *jcl) {
  if(jcl->jlog) {
    if(jcl->subscriber) {
      if(jcl->subscriber[0] == '~')
        jlog_ctx_remove_subscriber(jcl->jlog, jcl->subscriber);
      free(jcl->subscriber);
    }
    jlog_ctx_close(jcl->jlog);
  }
  free(jcl);
}

static mtev_hash_table feed_stats = MTEV_HASH_EMPTY;

jlog_feed_stats_t *
noit_jlog_feed_stats(const char *sub) {
  void *vs = NULL;
  jlog_feed_stats_t *s = NULL;
  if(mtev_hash_retrieve(&feed_stats, sub, strlen(sub), &vs))
    return (jlog_feed_stats_t *)vs;
  s = calloc(1, sizeof(*s));
  s->feed_name = strdup(sub);
  mtev_hash_store(&feed_stats, s->feed_name, strlen(s->feed_name), s);
  return s;
}
int
noit_jlog_foreach_feed_stats(int (*f)(jlog_feed_stats_t *, void *),
                             void *c) {
  mtev_hash_iter iter = MTEV_HASH_ITER_ZERO;
  const char *key;
  int klen, cnt = 0;
  void *vs;
  while(mtev_hash_next(&feed_stats, &iter, &key, &klen, &vs)) {
    cnt += f((jlog_feed_stats_t *)vs, c);
  }
  return cnt;
}
static int
__safe_Ewrite(eventer_t e, void *b, int l, int *mask) {
  int w, sofar = 0;
  while(l > sofar) {
    w = e->opset->write(e->fd, (char *)b + sofar, l - sofar, mask, e);
    if(w <= 0) return w;
    sofar += w;
  }
  return sofar;
}
#define Ewrite(a,b) __safe_Ewrite(e,a,b,&mask)

static int
noit_jlog_push(eventer_t e, noit_jlog_closure_t *jcl) {
  jlog_message msg;
  int mask;
  u_int32_t n_count;
  n_count = htonl(jcl->count);
  if(Ewrite(&n_count, sizeof(n_count)) != sizeof(n_count))
    return -1;
  while(jcl->count > 0) {
    int rv;
    struct { jlog_id chkpt; u_int32_t n_sec, n_usec, n_len; } payload;
    if(jlog_ctx_read_message(jcl->jlog, &jcl->start, &msg) == -1)
      return -1;

    /* Here we actually push the message */
    payload.chkpt.log = htonl(jcl->start.log);
    payload.chkpt.marker = htonl(jcl->start.marker);
    payload.n_sec  = htonl(msg.header->tv_sec);
    payload.n_usec = htonl(msg.header->tv_usec);
    payload.n_len  = htonl(msg.mess_len);
    if((rv = Ewrite(&payload, sizeof(payload))) != sizeof(payload)) {
      mtevL(noit_error, "Error writing jlog header over SSL %d != %d\n",
            rv, (int)sizeof(payload));
      return -1;
    }
    if((rv = Ewrite(msg.mess, msg.mess_len)) != msg.mess_len) {
      mtevL(noit_error, "Error writing jlog message over SSL %d != %d\n",
            rv, msg.mess_len);
      return -1;
    }
    /* Note what the client must checkpoint */
    jcl->chkpt = jcl->start;

    JLOG_ID_ADVANCE(&jcl->start);
    jcl->count--;
  }
  return 0;
}

void *
noit_jlog_thread_main(void *e_vptr) {
  int mask, bytes_read, sleeptime, max_sleeptime;
  eventer_t e = e_vptr;
  acceptor_closure_t *ac = e->closure;
  noit_jlog_closure_t *jcl = ac->service_ctx;
  char inbuff[sizeof(jlog_id)];

  mtev_memory_init_thread();
  eventer_set_fd_blocking(e->fd);

  max_sleeptime = DEFAULT_MSECONDS_BETWEEN_BATCHES;
  if(ac->cmd == NOIT_JLOG_DATA_TEMP_FEED)
    max_sleeptime = DEFAULT_TRANSIENT_MSECONDS_BETWEEN_BATCHES;

  sleeptime = max_sleeptime;
  while(1) {
    jlog_id client_chkpt;
    sleeptime = MIN(sleeptime, max_sleeptime);
    jlog_get_checkpoint(jcl->jlog, ac->remote_cn, &jcl->chkpt);
    jcl->count = jlog_ctx_read_interval(jcl->jlog, &jcl->start, &jcl->finish);
    if(jcl->count < 0) {
      char idxfile[PATH_MAX];
      mtevL(noit_error, "jlog_ctx_read_interval: %s\n",
            jlog_ctx_err_string(jcl->jlog));
      switch (jlog_ctx_err(jcl->jlog)) {
        case JLOG_ERR_FILE_CORRUPT:
        case JLOG_ERR_IDX_CORRUPT:
          jlog_repair_datafile(jcl->jlog, jcl->start.log);
          jlog_repair_datafile(jcl->jlog, jcl->start.log + 1);
          mtevL(noit_error,
                "jlog reconstructed, deleting corresponding index.\n");
          STRSETDATAFILE(jcl->jlog, idxfile, jcl->start.log);
          strlcat(idxfile, INDEX_EXT, sizeof(idxfile));
          unlink(idxfile);
          STRSETDATAFILE(jcl->jlog, idxfile, jcl->start.log + 1);
          strlcat(idxfile, INDEX_EXT, sizeof(idxfile));
          unlink(idxfile);
          goto alldone;
          break;
        default:
          goto alldone;
      }
    }
    if(jcl->count > MAX_ROWS_AT_ONCE) {
      /* Artificially set down the range to make the batches a bit easier
       * to handle on the stratcond/postgres end.
       * However, we must have more data, so drop the sleeptime to 0
       */
      jcl->count = MAX_ROWS_AT_ONCE;
      jcl->finish.marker = jcl->start.marker + jcl->count;
    }
    if(jcl->count > 0) {
      sleeptime = 0;
      if(noit_jlog_push(e, jcl)) {
        goto alldone;
      }
      /* Read our jlog_id accounting for possibly short reads */
      bytes_read = 0;
      while(bytes_read < sizeof(jlog_id)) {
        int len;
        if((len = e->opset->read(e->fd, inbuff + bytes_read,
                                 sizeof(jlog_id) - bytes_read,
                                 &mask, e)) <= 0)
          goto alldone;
        bytes_read += len;
      }
      memcpy(&client_chkpt, inbuff, sizeof(jlog_id));
      /* Fix the endian */
      client_chkpt.log = ntohl(client_chkpt.log);
      client_chkpt.marker = ntohl(client_chkpt.marker);
  
      if(memcmp(&jcl->chkpt, &client_chkpt, sizeof(jlog_id))) {
        mtevL(noit_error,
              "client %s submitted invalid checkpoint %u:%u expected %u:%u\n",
              ac->remote_cn, client_chkpt.log, client_chkpt.marker,
              jcl->chkpt.log, jcl->chkpt.marker);
        goto alldone;
      }
      gettimeofday(&jcl->feed_stats->last_checkpoint, NULL);
      jlog_ctx_read_checkpoint(jcl->jlog, &jcl->chkpt);
    }
    else {
      /* we have nothing to write -- maybe we have no checks configured...
       * If this is the case "forever", the remote might disconnect and
       * we would never know. Do the painful work of detecting a
       * disconnected client.
       */
      struct pollfd pfd;
      pfd.fd = e->fd;
      pfd.events = POLLIN | POLLHUP | POLLRDNORM;
      pfd.revents = 0;
      if(poll(&pfd, 1, 0) != 0) {
        /* normally, we'd recv PEEK|DONTWAIT.  However, the client should
         * not be writing to us.  So, we know we can't have any legitimate
         * data on this socket (true even though this is SSL). So, if we're
         * here then "shit went wrong"
         */
        mtevL(noit_error, "jlog client %s disconnected while idle\n",
              ac->remote_cn);
        goto alldone;
      }
    }
    if(sleeptime) {
      usleep(sleeptime * 1000); /* us -> ms */
    }
    sleeptime += 1000; /* 1 s */
  }

 alldone:
  e->opset->close(e->fd, &mask, e);
  mtev_atomic_dec32(&jcl->feed_stats->connections);
  noit_jlog_closure_free(jcl);
  acceptor_closure_free(ac);
  mtev_memory_maintenance();
  return NULL;
}

int
noit_jlog_handler(eventer_t e, int mask, void *closure,
                     struct timeval *now) {
  eventer_t newe;
  pthread_t tid;
  pthread_attr_t tattr;
  int newmask = EVENTER_READ | EVENTER_EXCEPTION;
  acceptor_closure_t *ac = closure;
  noit_jlog_closure_t *jcl = ac->service_ctx;
  char errbuff[256];
  const char *errstr = "unknown error";

  if(mask & EVENTER_EXCEPTION || (jcl && jcl->wants_shutdown)) {
    int len, nlen;
socket_error:
    /* Exceptions cause us to simply snip the connection */
    len = strlen(errstr);
    nlen = htonl(0 - len);
    e->opset->write(e->fd, &nlen, sizeof(nlen), &newmask, e);
    e->opset->write(e->fd, errstr, strlen(errstr), &newmask, e);
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    if(jcl) noit_jlog_closure_free(jcl);
    acceptor_closure_free(ac);
    return 0;
  }

  if(!ac->service_ctx) {
    mtev_log_stream_t ls;
    const char *logname, *type;
    int first_attempt = 1;
    char path[PATH_MAX], subscriber[256], *sub;
    jcl = ac->service_ctx = noit_jlog_closure_alloc();
    if(!mtev_hash_retr_str(ac->config,
                           "log_transit_feed_name",
                           strlen("log_transit_feed_name"),
                           &logname)) {
      errstr = "No 'log_transit_feed_name' specified in log_transit.";
      mtevL(noit_error, "%s\n", errstr);
      goto socket_error;
    }
    ls = mtev_log_stream_find(logname);
    if(!ls) {
      snprintf(errbuff, sizeof(errbuff),
               "Could not find log '%s' for log_transit.", logname);
      errstr = errbuff;
      mtevL(noit_error, "%s\n", errstr);
      goto socket_error;
    }
    type = mtev_log_stream_get_type(ls);
    if(!type || strcmp(type, "jlog")) {
      snprintf(errbuff, sizeof(errbuff),
               "Log '%s' for log_transit is not a jlog.", logname);
      errstr = errbuff;
      mtevL(noit_error, "%s\n", errstr);
      goto socket_error;
    }
    if(ac->cmd == NOIT_JLOG_DATA_FEED) {
      if(!ac->remote_cn) {
        errstr = "jlog transit started to unidentified party.";
        mtevL(noit_error, "%s\n", errstr);
        goto socket_error;
      }
      strlcpy(subscriber, ac->remote_cn, sizeof(subscriber));
      jcl->feed_stats = noit_jlog_feed_stats(subscriber);
    }
    else {
      jcl->feed_stats = noit_jlog_feed_stats("~");
      snprintf(subscriber, sizeof(subscriber),
               "~%07d", mtev_atomic_inc32(&tmpfeedcounter));
    }
    jcl->subscriber = strdup(subscriber);

    strlcpy(path, mtev_log_stream_get_path(ls), sizeof(path));
    sub = strchr(path, '(');
    if(sub) {
      char *esub = strchr(sub, ')');
      if(esub) {
        *esub = '\0';
        *sub++ = '\0';
      }
    }

    jcl->jlog = jlog_new(path);
    if(ac->cmd == NOIT_JLOG_DATA_TEMP_FEED) {
 add_sub:
      if(jlog_ctx_add_subscriber(jcl->jlog, jcl->subscriber, JLOG_END) == -1) {
        snprintf(errbuff, sizeof(errbuff),
                 "jlog reader[%s] error: %s", jcl->subscriber,
                 jlog_ctx_err_string(jcl->jlog));
        errstr = errbuff;
        mtevL(noit_error, "%s\n", errstr);
      }
    }
    if(jlog_ctx_open_reader(jcl->jlog, jcl->subscriber) == -1) {
      if(sub && !strcmp(sub, "*")) {
        if(first_attempt) {
          jlog_ctx_close(jcl->jlog);
          jcl->jlog = jlog_new(path);
          first_attempt = 0;
          goto add_sub;
        }
      }
      snprintf(errbuff, sizeof(errbuff),
               "jlog reader[%s] error: %s", jcl->subscriber,
               jlog_ctx_err_string(jcl->jlog));
      errstr = errbuff;
      mtevL(noit_error, "%s\n", errstr);
      goto socket_error;
    }
  }

  /* The jlog stuff is disk I/O and can block us.
   * We'll create a new thread to just handle this connection.
   */
  eventer_remove_fd(e->fd);
  newe = eventer_alloc();
  memcpy(newe, e, sizeof(*e));
  pthread_attr_init(&tattr);
  pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
  gettimeofday(&jcl->feed_stats->last_connection, NULL);
  mtev_atomic_inc32(&jcl->feed_stats->connections);
  if(pthread_create(&tid, &tattr, noit_jlog_thread_main, newe) == 0) {
    return 0;
  }

  /* Undo our dup */
  eventer_free(newe);
  /* Creating the thread failed, close it down and deschedule. */
  e->opset->close(e->fd, &newmask, e);
  return 0;
}

static int rest_show_feed(mtev_http_rest_closure_t *restc,
                          int npats, char **pats) {
  mtev_http_session_ctx *ctx = restc->http_ctx;
  const char *err = "unknown error";
  const char *jpath_with_sub;
  char jlogpath[PATH_MAX], *cp, **subs = NULL;
  int nsubs, i;
  mtev_log_stream_t feed;
  jlog_ctx *jctx = NULL;
  xmlDocPtr doc = NULL;
  xmlNodePtr root = NULL, subnodes;

  feed = mtev_log_stream_find("feed");
  if(!feed) { err = "cannot find feed"; goto error; }

  jpath_with_sub = mtev_log_stream_get_path(feed);
  strlcpy(jlogpath, jpath_with_sub, sizeof(jlogpath));
  cp = strchr(jlogpath, '(');
  if(cp) *cp = '\0';

  jctx = jlog_new(jlogpath);
  if((nsubs = jlog_ctx_list_subscribers(jctx, &subs)) == -1) {
    err = jlog_ctx_err_string(jctx);
    goto error;
  }

  doc = xmlNewDoc((xmlChar *)"1.0");
  root = xmlNewDocNode(doc, NULL, (xmlChar *)"feed", NULL);
  xmlDocSetRootElement(doc, root);

  subnodes = xmlNewNode(NULL, (xmlChar *)"subscribers");
  for(i=0; i<nsubs; i++) {
    xmlNewChild(subnodes, NULL, (xmlChar *)"subscriber", (xmlChar *)subs[i]);
  }
  xmlAddChild(root, subnodes);

  mtev_http_response_ok(restc->http_ctx, "text/xml");
  mtev_http_response_xml(restc->http_ctx, doc);
  mtev_http_response_end(restc->http_ctx);
  if(subs) jlog_ctx_list_subscribers_dispose(jctx, subs);
  xmlFreeDoc(doc);
  jlog_ctx_close(jctx);
  return 0;

 error:
  if(doc) xmlFreeDoc(doc);
  if(subs) jlog_ctx_list_subscribers_dispose(jctx, subs);
  mtev_http_response_server_error(ctx, "text/plain");
  mtev_http_response_append(ctx, err, strlen(err));
  mtev_http_response_end(ctx);
  if(jctx) jlog_ctx_close(jctx);
  return 0;
}

static int rest_delete_feed(mtev_http_rest_closure_t *restc,
                            int npats, char **pats) {
  mtev_http_session_ctx *ctx = restc->http_ctx;
  const char *err = "unknown error";
  const char *jpath_with_sub;
  char jlogpath[PATH_MAX], *cp;
  int rv;
  mtev_log_stream_t feed;
  jlog_ctx *jctx;

  feed = mtev_log_stream_find("feed");
  if(!feed) { err = "cannot find feed"; goto error; }

  jpath_with_sub = mtev_log_stream_get_path(feed);
  strlcpy(jlogpath, jpath_with_sub, sizeof(jlogpath));
  cp = strchr(jlogpath, '(');
  if(cp) *cp = '\0';

  jctx = jlog_new(jlogpath);
  rv = jlog_ctx_remove_subscriber(jctx, pats[0]);
  jlog_ctx_close(jctx);
  if(rv < 0) {
    err = jlog_ctx_err_string(jctx);
    goto error;
  }

  /* removed or note, we should do a sweeping cleanup */
  jlog_clean(jlogpath);

  if(rv == 0) {
    mtev_http_response_not_found(ctx, "text/plain");
    mtev_http_response_end(ctx);
    return 0;
  }

  mtev_http_response_standard(ctx, 204, "OK", "text/plain");
  mtev_http_response_end(ctx);
  return 0;

 error:
  mtev_http_response_server_error(ctx, "text/plain");
  mtev_http_response_append(ctx, err, strlen(err));
  mtev_http_response_end(ctx);
  return 0;
}

