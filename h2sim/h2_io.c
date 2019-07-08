/*
 * h2sim - HTTP2 Simple Application Framework using nghttp2
 *
 * Copyright 2019 Lee Yongjae, Telcoware Co.,LTD.
 *
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#ifdef EPOLL_MODE
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

#ifdef TLS_MODE
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#else
/* NOTE: h2_util.h defines dummy SSL_CTX and SSL */
#endif

#include <nghttp2/nghttp2.h>

#include "h2.h"
#include "h2_priv.h"


/* forward declaration */
static int h2_sess_recv_v1_1(h2_sess *sess, const void *data, int size);
static int h2_sess_send_once_v1_1(h2_sess *sess);


/*
 * File Control Flag Utilities ---------------------------------------------
 */

static void h2_set_nonblock(int fd) {
  int v;
  if ((v = fcntl(fd, F_GETFL, 0)) != -1) {
    fcntl(fd, F_SETFL, v | O_NONBLOCK);
  }
}

static void h2_set_close_exec(int fd) {
  int v;
  if ((v = fcntl(fd, F_GETFD, 0)) != -1) {
    fcntl(fd, F_SETFD, v | FD_CLOEXEC);
  }
}


/*
 * Session Send ------------------------------------------------------------
 */

/*
 * Send merge buf size consideration:
 * - min: too small packet causes perf damage including all network components
 * - max: cocurrent streams x req_hdr+data or rsp_hdr+data size
 * - tcp send buf range min value: /proc/sys/net/ipv4/tcp_wmem
 * - TLS record size
 * - tcp MTU: 1360 or less; cf. some public CPs site has MTU 1360
 */

void h2_sess_mark_send_pending(h2_sess *sess) {
  if (!sess->send_pending) {
#ifdef EPOLL_MODE
    struct epoll_event e;
    e.events = EPOLLIN | EPOLLOUT;
    e.data.ptr = &sess->obj;
    epoll_ctl(sess->fd, EPOLL_CTL_MOD, sess->fd, &e);
#endif
    sess->send_pending = 1;
  }
}

inline void h2_sess_clear_send_pending(h2_sess *sess) {
  if (sess->send_pending) {
#ifdef EPOLL_MODE
    struct epoll_event e;
    e.events = EPOLLIN;
    e.data.ptr = &sess->obj;
    epoll_ctl(sess->fd, EPOLL_CTL_MOD, sess->fd, &e);
#endif
    sess->send_pending = 0;
  }
}

static int h2_sess_send_once_v2(h2_sess *sess) {
  h2_wr_buf *wb = &sess->wr_buf;
  int sent, total_sent = 0;
#ifdef TLS_MODE
  SSL *ssl = sess->ssl;
  int r;
#endif
#ifdef EPOLL_MODE
  int mem_send_zero = 0;
#endif

  /* NOTE: send is always blocking */
  /* TODO: save and retry to send on last to_send data */

  if (wb->merge_size > 0 || wb->mem_send_size > 0) {
    warnx("### DEBUG: REENTRY WITH REMAINING WRITE: "
          "merge_size=%d mem_send_size=%d", wb->merge_size, wb->mem_send_size);
  }

  while (wb->mem_send_size <= 0 && wb->merge_size < H2_WR_BUF_SIZE) {
    const uint8_t *mem_send_data;
    ssize_t mem_send_size;
    
    mem_send_size = nghttp2_session_mem_send(sess->ng_sess, &mem_send_data);
    /* DEBUG: to check mem_send size */
    /* fprintf(stderr, "%d ", (int)mem_send_size); */

    if (mem_send_size < 0) {
      /* probablly NGHTTP2_ERR_NOMEM; abort immediately */
      warnx("nghttp2_session_mem_send() error: %s",
            nghttp2_strerror(mem_send_size));
      sess->close_reason = CLOSE_BY_NGHTTP2_ERR;
      return -1;
    } else if (mem_send_size == 0) {
      /* no more data to send */
#ifdef EPOLL_MODE
      mem_send_zero = 1; 
#endif
      break;
    } else if (wb->merge_size + mem_send_size <= (int)sizeof(wb->merge_data)) {
      /* merge to buf */
      memcpy(&wb->merge_data[wb->merge_size], mem_send_data, mem_send_size);
      wb->merge_size += mem_send_size;
    } else {
      /* cannot merge to buf */
      wb->mem_send_data = (void *)mem_send_data;
      wb->mem_send_size = mem_send_size;
      break;
    }
  }

  /* HERE: DEBUG: to check merge_size and mem_send size */
  //fprintf(stderr, "%d+%d ", wb->merge_size, wb->mem_send_size);

  /* try to send merge_data once */
  if (wb->merge_size > 0) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->merge_data, wb->merge_size);
      if (r > 0) {
        sent = wb->merge_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          fprintf(stderr, "DEBUG: TLS SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(merge_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->merge_data, wb->merge_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          fprintf(stderr, "DEBUG: TCP SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error with to_send=%d: %s",
              wb->merge_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -3;
      }
    }

    //warnx("### DEBUG: DATA SENT: merge_buf sent=%d", sent);
    total_sent += sent;

    if (sent < wb->merge_size) {
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      warnx("### DEBUG: MERGE_BUF PARTIAL!!! %d/%d ", sent, wb->merge_size);

      memmove(wb->merge_data, &wb->merge_data[sent], wb->merge_size - sent);
      wb->merge_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->merge_size = 0;
    }
  }

  /* try to send mem_send_data once */
  if (wb->mem_send_size) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->mem_send_data, wb->mem_send_size);
      if (r > 0) {
        sent = wb->mem_send_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          fprintf(stderr, "DEBUG: TLS SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(mem_send_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->mem_send_data, wb->mem_send_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          fprintf(stderr, "DEBUG: TCP SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error with to_send=%d: %s",
              wb->mem_send_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -5;
      }
    }

    //warnx("### DEBUG: DATA SENT: mem_send sent=%d", sent);
    total_sent += sent;

    if (sent < wb->mem_send_size) {
      /* indication for possible block at next */
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      fprintf(stderr, "### DEBUG: MEM_SEND PARTIAL!!!%d/%d ", sent, wb->mem_send_size);
      wb->mem_send_data += sent;
      wb->mem_send_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->mem_send_data = NULL;
      wb->mem_send_size = 0;
    }
  }

  if (total_sent == 0) {
    h2_sess_clear_send_pending(sess);
    /*
    static int c = 0;
    c++;
    warnx("### DEBUG: [%d] EXIT WITHOUT SENT DATA: merge_size=%d "
          "mem_send_size=%d", c, wb->merge_size, wb->mem_send_size);
    */
  }

#ifdef EPOLL_MODE
  if (mem_send_zero && !nghttp2_session_want_read(sess->ng_sess)) {
    sess->close_reason = CLOSE_BY_NGHTTP2_END;
    return -6;
  }
#endif

  return total_sent;
}

int h2_sess_send(h2_sess *sess) {
  int r;

  if (sess->http_ver == H2_HTTP_V2) {
    do {
      r = h2_sess_send_once_v2(sess);
    } while (r > 0);
  } else {
    do {
      r = h2_sess_send_once_v1_1(sess);
    } while (r > 0);
  }

  return r;
}

static int h2_sess_recv(h2_sess *sess) {
  uint8_t buf[H2_RD_BUF_SIZE];
  ssize_t recv_len, read_len;
#ifdef TLS_MODE
  SSL *ssl = sess->ssl;
#endif

#ifdef TLS_MODE
  if (ssl) {
    recv_len = SSL_read(ssl, buf, sizeof(buf));
    if (recv_len < 0) {
      if (SSL_get_error(ssl, recv_len) == SSL_ERROR_WANT_READ) {
        return 0;  /* retry later */
      }
      ERR_print_errors_fp(stderr);
    }
  } else
#endif
  {
    recv_len = recv(sess->fd, buf, sizeof(buf), 0);
  }
  if (recv_len < 0) {
    // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return 0;  /* retry later */
    }
    warnx("network error: %s", strerror(errno));
    sess->close_reason = CLOSE_BY_SOCK_ERR;
    return -1;
  } else if (recv_len == 0) {
    warnx("disconnected from the remote host");
    sess->close_reason = CLOSE_BY_SOCK_EOF;
    return -2;
  }

  //warnx("### DEBUG: DATA RECEIVED: recv_len=%d", (int)recv_len);

  if (sess->http_ver == H2_HTTP_V2) {
    read_len = nghttp2_session_mem_recv(sess->ng_sess, buf, recv_len);
    /* NOTE: read_len is same as recv_len on success case */
    if (read_len < 0) {
      warnx("Fatal error: %s", nghttp2_strerror((int)read_len));
      sess->close_reason = CLOSE_BY_NGHTTP2_ERR;
      return -3;
    }
  } else {
    read_len = h2_sess_recv_v1_1(sess, buf, recv_len);
    if (read_len < 0) {
      warnx("HTTP/1.1 read error: ret=%d", (int)read_len);
      sess->close_reason = CLOSE_BY_HTTP_ERR;
      return -3;
    }
  }

  return (int)read_len;
}


/*
 * Http2 Settings Handling -------------------------------------------------
 */

void h2_settings_init(h2_settings *settings) {
  /* HTTP/2 */
  settings->header_table_size = -1; 
  settings->enable_push = -1;
  settings->max_concurrent_streams = -1;
  settings->initial_window_size = -1;
  settings->max_frame_size = -1;
  settings->max_header_list_size = -1;
  settings->enable_connect_protocol = -1;
}

static int h2_sess_send_settings(h2_sess *sess, h2_settings *settings) {
  nghttp2_settings_entry iv[16];
  int r, iv_num = 0;

  if (sess->http_ver != H2_HTTP_V2) {
    return 0;  /* simply ignored on HTTP/1.1 session */
  }

  if (settings) {
    if (settings->header_table_size >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
      iv[iv_num++].value = settings->header_table_size;
    }
    if (settings->enable_push >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
      iv[iv_num++].value = settings->enable_push;
    }
    if (settings->max_concurrent_streams >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
      iv[iv_num++].value = settings->max_concurrent_streams;
    }
    if (settings->initial_window_size >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
      iv[iv_num++].value = settings->initial_window_size;
    }
    if (settings->max_frame_size >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_MAX_FRAME_SIZE;
      iv[iv_num++].value = settings->max_frame_size;
    }
    if (settings->max_header_list_size >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
      iv[iv_num++].value = settings->max_header_list_size;
    }
    if (settings->enable_connect_protocol >= 0) {
      iv[iv_num].settings_id = NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL;
      iv[iv_num++].value = settings->enable_connect_protocol;
    }
  }

  r = nghttp2_submit_settings(sess->ng_sess, NGHTTP2_FLAG_NONE, iv, iv_num);
  if (r != 0) {
    warnx("submit setting failed: %s", nghttp2_strerror(r));
    return -1;
  }
  return h2_sess_send(sess);
}


/*
 * HTTP/1.1 I/O Handlers -----------------------------------------------------
 */

static int h2_sess_recv_hdl_once_v1_1(h2_sess *sess) {
  /* returns 1(message parse completed), 0(parse not completed), <0(error) */
  h2_msg *rmsg;
  if (sess->strm_recving) {
    rmsg = sess->strm_recving->rmsg;
  } else {
    /* check for starting new strm and rmsg */
    if (sess->is_server) {
      sess->strm_recving = h2_strm_init(sess, sess->req_cnt * 2 + 1,
                                        H2_REQUEST, NULL, NULL);
    } else {  /* client */
      if (sess->strm_list_head.next == NULL) {
        warnx("%sHTTP/1.1 response received for no request at %d: "
              "rdata_used=%d rdata_size=%d",
              sess->log_prefix, sess->rdata_offset + sess->rdata_used,
              sess->rdata_used, sess->rdata_size);
        return -1;
      }
      sess->strm_recving = sess->strm_list_head.next;
    }
    sess->rmsg_header_done = 0;
    sess->rmsg_header_line = 0;
    sess->rmsg_content_length = 0;
    rmsg = sess->strm_recving->rmsg;
  }

  /* check and parse http header */
  if (!sess->rmsg_header_done) {
    char *base = sess->rdata + sess->rdata_used;
    char *limit = sess->rdata + sess->rdata_size;
    char *p, *q;

    p = base;
    while (p < limit && (p = memchr(p, '\n', limit - p))) {
      char *end = p;
      /* remove '\r\n' from end pointer */
      if (end > base && *(end -1) == '\r') {
        end--;
      }
      /* check emplty line for header end mark */
      if (end == base) { 
        sess->rdata_used = p + 1 - sess->rdata;
        sess->rmsg_header_done = 1;
        break;
      }
      if (sess->rmsg_header_line == 0) {
        /* first line special handling */
        if (sess->is_server) {
          /* parse Request-Line */
          /* trim off version and spaces */
          if (end >= base + 1/*method*/ + 1 + 1/*path*/ + 1 + 8/*version*/ &&
              !memcmp(end - 8, "HTTP/1.1", 8)) {
            end -= 8; 
          } else {
            warnx("%sHTTP/1.1 request line parse failed at %ld",
                  sess->log_prefix, sess->rdata_offset + base - sess->rdata);
            return -1;
          }
          while (end >= base + 1/*method*/ + 1 + 1/*path*/ &&
                 (*(end - 1) == ' ' || *(end - 1) == '\t')) {
            end--;
          }
          /* find method */
          for (q = base; q < end && *q != ' ' && *q != '\t'; q++) {
            /* to count non-space chars */
          }
          h2_set_method_n(rmsg, base, q - base);
          h2_set_scheme(rmsg, (sess->ssl)? "https" : "http");
          h2_set_authority(rmsg,  "http");
          /* find path */
	  for (; q < end && (*q == ' ' || *q == '\t'); q++) {
            /* to skip spaces */
          }
          h2_set_path_n(rmsg, q, end - q);
        } else {
          /* parse Status-Line */
          q = base;
          if (end - base >= 3 &&
              q[0] >= '1' && q[0] <= '5' &&
              q[1] >= '0' && q[1] <= '9' &&
              q[2] >= '0' && q[2] <= '9' &&
              (end - base <= 3 || (q[3] == ' ' || q[3] == '\t'))) {
            /* NOTE: ignore reason part */
            h2_set_status(rmsg,
                          ((int)(q[0] - '0')) * 100 +
                          ((int)(q[1] - '0')) * 10 +
                          ((int)(q[2] - '0')) * 1);
          } else {
            warnx("%sHTTP/1.1 status line parse failed at %ld",
                  sess->log_prefix, sess->rdata_offset + base - sess->rdata);
            return -2;
          }
        }
      } else {
        /* parse message-header */
        /* TODO: NEED TO HANDLE MULTI LINE VALUE */
        if ((q = memchr(base, ':', end - base))) {
          char *name = base;
          int nlen = q - base;
          /* trim off space heading and tail from value */
          char *value = q + 1;
          while (value < end && (*value == ' ' || *value == '\t')) {
            value++;
          }
          while (end > value && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
            end--;
          }
          /* now append header */
          if (nlen == 4 && !strncasecmp(name, "host", 4) &&
              sess->is_server) {
            h2_set_authority_n(rmsg, value, end - value);
          } else if (nlen == 14 && !strncasecmp(name, "content-length", 14)) {
            /* TODO: NEED MORE STRICT CHECKING */
            sess->rmsg_content_length = atoi(value); 
          } else {
            /* TODO: NEED TO HANDLE Connection: for continued or close */
            /* TODO: NEED TO HANDLE Transfer-Encoding: for chunked body */
            h2_add_hdr_n(rmsg, name, nlen, value, end - value);
          }
        } else {
          warnx("%sHTTP/1.1 message header parse failed at %ld",
                sess->log_prefix, sess->rdata_offset + base - sess->rdata);
          return -3;
        }
      }
      p ++;
      base = p;
      sess->rdata_used = base - sess->rdata;
      sess->rmsg_header_line++;
    }
    /* now, create meassage and parse header */
  }

  /* check and parse http body */
  if (sess->rmsg_header_done) {
    if (sess->rmsg_content_length && h2_body_len(rmsg) == 0) {
      /* check for data avaiable for content_length */
      /* TODO: NEED TO HANDLE Chunked Body case */
      if (sess->rdata_size - sess->rdata_used >= sess->rmsg_content_length) {
        h2_cpy_body(rmsg, sess->rdata + sess->rdata_used,
                    sess->rmsg_content_length);
        sess->rdata_used += sess->rmsg_content_length; 
      }
    }
    if (sess->rmsg_content_length == h2_body_len(rmsg)) {
      /* now, message compeleted, call app callbacks */
      int r;
      if (sess->is_server) {
        sess->req_cnt++;
        r = h2_on_request_recv(sess, sess->strm_recving);
      } else {  /* client */
        r = h2_on_response_recv(sess, sess->strm_recving);
        h2_strm_free(sess->strm_recving);
        sess->strm_close_cnt++;
        //{
        //  static int n = 0;
        //  printf("DEBUG[%d]: req_cnt=%d rsp_cnt=%d strm_close_cnt=%d\n",
        //         m++, sess->req_cnt, sess->rsp_cnt, sess->strm_close_cnt);
        //}
      }
      sess->strm_recving = NULL;
      return (r >= 0)? 1 : r;
    }
  }
  return 0;
}

static int h2_sess_recv_v1_1(h2_sess *sess, const void *data, int size) {
  /* append to rdata */
  if (sess->rdata == NULL) {
#define RDATA_ALLOC_DEF  (16 * 1024)
    sess->rdata_alloced = (size >= RDATA_ALLOC_DEF)? size : RDATA_ALLOC_DEF;
    sess->rdata = malloc(sess->rdata_alloced);
    memcpy(sess->rdata, data, size);
    sess->rdata_size = size; 
    sess->rdata_used = 0;
  } else if (sess->rdata_alloced >= sess->rdata_size + size) {
    /* just append new data */ 
    memcpy(sess->rdata + sess->rdata_size, data, size);
    sess->rdata_size += size;
  } else {
    /* realloc buffer and remove rdata_used */
    if (sess->rdata_alloced < sess->rdata_size - sess->rdata_used + size) {
      sess->rdata_alloced = sess->rdata_size - sess->rdata_used + size;
      sess->rdata = realloc(sess->rdata, sess->rdata_alloced); 
    }
    sess->rdata_size -= sess->rdata_used;
    sess->rdata_offset += sess->rdata_used;
    if (sess->rdata_size) {
      memmove(sess->rdata, sess->rdata + sess->rdata_used, sess->rdata_size);
    } 
    sess->rdata_used = 0;
    /* append new data */ 
    memcpy(sess->rdata + sess->rdata_size, data, size);
    sess->rdata_size += size;
  }

  /* try to parse and handle message */
  int r;
  while ((r = h2_sess_recv_hdl_once_v1_1(sess)) == 1) {
    /* repeat for all message availiable */
    if (sess->rdata_used == sess->rdata_size || sess->is_terminated == 1) {
      break;
    }
  }
  if (r < 0) {
    warnx("%sh2_sess_recv_hdl_once_v1_1() failed: ret=%d", sess->log_prefix, r);
    return -1;
  }

  /* on all rdata handled, deallocated extended buffer */
  if (sess->rdata_used == sess->rdata_size &&
      sess->rdata_alloced > RDATA_ALLOC_DEF) {
    sess->rdata_offset += sess->rdata_used;
    free(sess->rdata);
    sess->rdata = NULL;
    sess->rdata_alloced = 0;
    sess->rdata_size = 0;
    sess->rdata_used = 0;
  }
  return size;
}

int h2_sess_send_once_v1_1(h2_sess *sess) {
  h2_wr_buf *wb = &sess->wr_buf;
  int sent, total_sent = 0;
#ifdef TLS_MODE
  SSL *ssl = sess->ssl;
  int r;
#endif

  /* NOTE: send is always blocking */

  if (wb->merge_size > 0 || wb->mem_send_size > 0) {
    warnx("### DEBUG: REENTRY WITH REMAINING WRITE: "
          "merge_size=%d mem_send_size=%d", wb->merge_size, wb->mem_send_size);
  }

  /* try to use stream's read buffer for send data */
  while (wb->mem_send_size <= 0 && wb->merge_size < H2_WR_BUF_SIZE) {
    const uint8_t *strm_send_data = NULL;
    ssize_t strm_send_size = 0;
    h2_strm *strm, *strm_next;
    h2_read_buf *rb;
   
    /* TODO: TO BE REIMPLEMNTED */
    if (sess->is_server) {
      /* check and flush last sent strm */
      for (strm = sess->strm_list_head.next;
           strm && strm->response_set;
           strm = strm_next) {
        strm_next = strm->next;
	rb = &strm->send_body_rb;
        if (rb->data_used >= rb->data_size) {
          h2_strm_free(strm);  /* strm.data are all sent; free stream */
          sess->strm_close_cnt++;
          //{
          //  static int n = 0;
          //  printf("DEBUG[%d]: req_cnt=%d rsp_cnt=%d strm_close_cnt=%d\n",
          //         n, sess->req_cnt, sess->rsp_cnt, sess->strm_close_cnt);
          //}
          continue;

        } else {  /* found data to send */
          strm_send_data = rb->data + rb->data_used;
          strm_send_size = rb->data_size - rb->data_used;
          rb->data_used = rb->data_size;
          break;
        }
      }
    } else {  /* client */
      if (sess->strm_sending == NULL) {
        sess->strm_sending = sess->strm_list_head.next;
      } 
      while ((strm = sess->strm_sending)) {
	rb = &strm->send_body_rb;
        if (rb->data_used >= rb->data_size) {
          sess->strm_sending = strm->next;
        } else {
          strm_send_data = rb->data + rb->data_used;
          strm_send_size = rb->data_size - rb->data_used;
          rb->data_used = rb->data_size;
          break;
        }
      }
    }

    /* DEBUG: to check mem_send size */
    /* fprintf(stderr, "%d ", (int)strm_send_size); */

    // HERE: TODO: MARK CURRENT STREAM FOR MEM_SEND_DATA
    if (strm_send_size == 0) {
      /* no more data to send */
      break;
    } else if (wb->merge_size + strm_send_size <= (int)sizeof(wb->merge_data)) {
      /* merge to buf */
      memcpy(&wb->merge_data[wb->merge_size], strm_send_data, strm_send_size);
      wb->merge_size += strm_send_size;
    } else {
      /* cannot merge to buf */
      wb->mem_send_data = (void *)strm_send_data;
      wb->mem_send_size = strm_send_size;
      break;
    }
  }

  /* HERE: DEBUG: to check merge_size and mem_send size */
  //fprintf(stderr, "%d+%d ", wb->merge_size, wb->mem_send_size);

  /* try to send merge_data once */
  if (wb->merge_size > 0) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->merge_data, wb->merge_size);
      if (r > 0) {
        sent = wb->merge_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          fprintf(stderr, "DEBUG: TLS SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(merge_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->merge_data, wb->merge_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          fprintf(stderr, "DEBUG: TCP SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error with to_send=%d: %s",
              wb->merge_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -3;
      }
    }

    //warnx("### DEBUG: DATA SENT: merge_buf sent=%d", sent);
    total_sent += sent;
    sess->send_data_remain -= sent;

    if (sent < wb->merge_size) {
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      warnx("### DEBUG: MERGE_BUF PARTIAL!!! %d/%d ", sent, wb->merge_size);

      memmove(wb->merge_data, &wb->merge_data[sent], wb->merge_size - sent);
      wb->merge_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->merge_size = 0;
    }
  }

  /* try to send mem_send_data once */
  if (wb->mem_send_size) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->mem_send_data, wb->mem_send_size);
      if (r > 0) {
        sent = wb->mem_send_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          fprintf(stderr, "DEBUG: TLS SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(mem_send_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->mem_send_data, wb->mem_send_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          fprintf(stderr, "DEBUG: TCP SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error with to_send=%d: %s",
              wb->mem_send_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -5;
      }
    }

    //warnx("### DEBUG: DATA SENT: mem_send sent=%d", sent);
    total_sent += sent;
    sess->send_data_remain -= sent;

    if (sent < wb->mem_send_size) { 
      /* indication for possible block at next */
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      fprintf(stderr, "### DEBUG: MEM_SEND PARTIAL!!!%d/%d ",
              sent, wb->mem_send_size);

      wb->mem_send_data += sent;
      wb->mem_send_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->mem_send_data = NULL;
      wb->mem_send_size = 0;
    }
  }

  if (total_sent == 0) {
    h2_sess_clear_send_pending(sess);
    /*
    static int c = 0;
    c++;
    warnx("### DEBUG: [%d] EXIT WITHOUT SENT DATA: merge_size=%d "
          "mem_send_size=%d", c, wb->merge_size, wb->mem_send_size);
    */
  }

  return total_sent;
}


/*
 * HTTP/1.1 to HTTP/2 Upgrade Handlers -------------------------------------
 */

#if 0
static int base64_encode(void *_org, int org_len, char *buf)
{
  static char encode_map[64] = {
    /* A - Z */
    65, 66, 67, 68, 69, 70, 71, 72,
    73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
    /* a - z */
    97, 98, 99, 100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
    /* 0 - 9 */
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    /* + / */
    43, 47
  };

  unsigned char *org = (unsigned char *)_org;
  char *e = buf;
  int i, x;

  for (i = 0; i < org_len; i += 3) {
    if (org_len - i >= 3) {
      x = ((int)org[i] << 16) + ((int)org[i+1] << 8) + ((int)org[i+2]);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = encode_map[(x >>  6) & 0x3f];
      e[3] = encode_map[(x) & 0x3f];
    }
    else if (org_len - i == 2) {  /* only two characters remain */
      x = ((int)org[i] << 16) + ((int)org[i+1] << 8);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = encode_map[(x >>  6) & 0x3f];
      e[3] = '=';
    }
    else {  // n - i == 1  /* only one character remains */
      x = ((int)org[i] << 16);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = '=';
      e[3] = '=';
    }
    e += 4;
  }

  /* returns encoded size */
  return e - buf;
}

static int h2_sess_send_upgrade_req_tcp(h2_sess *sess, const char *authority,
                                        h2_settings *settings) {
  char sb[256], sb64[1024];
  int sb64_len = 0;

  if (settings) {
#define ADD_SETTINGS(_id, _field)  \
    if (settings->_field) {  \
      uint16_t id = htons(_id);  \
      uint32_t value = htonl(settings->_field);  \
      memcpy(p + 0, &id, 2);  \
      memcpy(p + 2, &value, 4);  \
      p += 6;  \
    }

    void *p = sb;
    ADD_SETTINGS(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, header_table_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_ENABLE_PUSH, enable_push);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, max_concurrent_streams);
    ADD_SETTINGS(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, initial_window_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_FRAME_SIZE, max_frame_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, max_header_list_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, enable_connect_protocol);
    sb64_len = base64_encode(sb, p - (void *)sb, sb64);
  }
  sb64[sb64_len] = '\0';

  h2_msg_free(req);
  return h2_sess_send(sess);
}
#endif


/*
 * Session Common ----------------------------------------------------------
 */

int h2_sess_terminate(h2_sess *sess, int wait_rsp) {

  if (sess == NULL || sess->is_terminated == 1) {
    return 1;  /* already terminated */
  }

  if (wait_rsp && !sess->is_server && sess->req_cnt > sess->rsp_cnt) {
    if (sess->ctx->verbose) {
      warnx("%sTERMINATE SESSION WAIT RESPONSE", sess->log_prefix);
    }
    sess->is_terminated = 2/* wait_rsp */;
    /* NOTE: submit goways seems not working well */
    /*       just wait untl all response recevied */
    if (sess->http_ver == H2_HTTP_V2) {
#if 0  /* seems not working */
      int n = nghttp2_session_get_next_stream_id(sess->ng_sess) - 1;
      int r = nghttp2_submit_goaway(sess->ng_sess, NGHTTP2_FLAG_NONE,
                                n, NGHTTP2_NO_ERROR, NULL, 0);
      if (r < 0) {
        warnx("%snghttp2_submit_goway() failed: last_stream_id=%d ret=%d", 
              sess->log_prefix, n, r);
        return -1;
      }
#endif
    } else {
      /* half close */
      if (sess->ssl) {
        SSL_set_shutdown(sess->ssl, SSL_SENT_SHUTDOWN);
        SSL_shutdown(sess->ssl);
      } else {
        shutdown(sess->fd, SHUT_WR);
      }
    }
  } else {
    if (sess->ctx->verbose) {
      warnx("%sTERMINATE SESSION IMMEDIATE", sess->log_prefix);
    }
    sess->is_terminated = 1/* immediate */;

    if (sess->http_ver == H2_HTTP_V2) {
      int r = nghttp2_session_terminate_session(sess->ng_sess, NGHTTP2_NO_ERROR);
      if (r < 0) {
        warnx("%snghttp2_session_terminate_session() failed: ret=%d",
              sess->log_prefix, r);
        return -1;
      }
    } else {
      /* close socket force */
      if (sess->ssl) {
        SSL_set_shutdown(sess->ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
        SSL_shutdown(sess->ssl);
      } else {
        shutdown(sess->fd, SHUT_WR | SHUT_RD);
      }
    } 

    h2_sess_mark_send_pending(sess);
  }

  return 0;
}


/*
 * Client Session I/O ------------------------------------------------------
 */

static h2_sess *h2_sess_init_client(h2_ctx *ctx, SSL *ssl,
                                    int fd, const char *authority) {
  h2_sess *sess = calloc(1, sizeof(h2_sess));
  sess->obj.cls = &h2_cls_sess;

  /* insert into ctx session list */
  sess->next = ctx->sess_list_head.next;
  ctx->sess_list_head.next = sess;
  sess->prev = &ctx->sess_list_head;
  if (sess->next) {
    sess->next->prev = sess;
  }
  ctx->sess_num++;

  sess->ctx = ctx;
  sess->http_ver = ctx->http_ver;
  sess->is_server = 0;
  sess->ssl = ssl;
  sess->fd = fd;

  /* use local binding address for session log prefix */
  struct sockaddr_in6 sa;  /* to allow ipv4 and ipv6 */
  socklen_t salen = sizeof(sa);
  if (getsockname(fd, (struct sockaddr *)&sa, &salen) == 0) {
    /* get log prefix info */
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (getnameinfo((struct sockaddr *)&sa, salen, host, sizeof(host),
                    serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV)) {
      sess->log_prefix = strdup("(unknown)");
    } else {
      char log_prefix[1 + NI_MAXHOST + 1 + 1 + NI_MAXSERV + 1];
      if (((struct sockaddr *)&sa)->sa_family == AF_INET6) {
         sprintf(log_prefix, "[%s]:%s ", host, serv);
      } else {
         sprintf(log_prefix, "%s:%s ", host, serv);
      }
      sess->log_prefix = strdup(log_prefix);
    }
  } else {
    sess->log_prefix = malloc(3 + strlen(authority) + 2);
    strcpy(sess->log_prefix, "to:");
    strcat(sess->log_prefix, authority);
    strcat(sess->log_prefix, " ");
  }
  
#ifdef EPOLL_MODE
  struct epoll_event e;
  e.events = EPOLLIN;
  e.data.ptr = &sess->obj;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, sess->fd, &e) < 0) {
    warnx("sess client init failed for epoll_ctl() error: %s", strerror(errno));
    h2_sess_free(sess);
    return NULL;
  }
#endif

  h2_sess_nghttp2_init(sess);

  /* mark start time */
  gettimeofday(&sess->tv_begin, NULL);

  return sess;
}

static h2_sess *h2_sess_client_start(int sock, h2_ctx *ctx,
                    const char *authority, SSL_CTX *client_ssl_ctx,
                    h2_settings *settings) {
  SSL *ssl = NULL;
  int http_ver = 0;
#ifdef TLS_MODE
#else
  (void)client_ssl_ctx;
#endif

  // do blocking and no wait send
  int v = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));

#ifdef TLS_MODE
  if (client_ssl_ctx) {
    const unsigned char *alpn = NULL;
    unsigned int alpnlen = 0;
    int r;

    /* HERE: TODO: MAY NEED TO SET sock NOBLOCKING before  SSL_set_fd() */
    /* HERE: TO CHECK: is sock needs to be close()ed on error exit case? */

    ssl = SSL_new(client_ssl_ctx);
    if (!ssl) {
      warnx("%s connected but cannot create tls session: %s",
            authority, ERR_error_string(ERR_get_error(), NULL));
      return NULL;
    }
    if (ctx->http_ver == H2_HTTP_V2 || ctx->http_ver == H2_HTTP_V2_TRY) {
      SSL_set_alpn_protos(ssl, (const unsigned char *)"\x02h2", 3);
    }
    SSL_set_fd(ssl, sock);
    r = SSL_connect(ssl);
    if (r == 0) {
      warnx("%s connected but shutdown by tls protocol: %d",
            authority, SSL_get_error(ssl, r));
      SSL_free(ssl);
      return NULL;
    } else if (r < 0) {
      warnx("%s tls handshake failed: %s",
            authority, ERR_error_string(ERR_get_error(), NULL));
      SSL_free(ssl);
      return NULL;
    }
    SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
    if (alpn && alpnlen == 2 && !memcmp("h2", alpn, 2)) {
      http_ver = H2_HTTP_V2;
    } else if (ctx->http_ver == H2_HTTP_V2) {
      warnx("%s h2 is not negotiated; HTTP/2 failed", authority);
      SSL_free(ssl);
      return NULL;
    } else {
      http_ver = H2_HTTP_V1_1;
    }
    /* now, http_ver of TLS is fixed to H2_HTTP_V2 or H2_HTTP_V1_1 */
  }
#endif /* TLS_MODE */
  
  h2_sess *sess = h2_sess_init_client(ctx, ssl, sock, authority);
  if (sess == NULL) {
    return NULL;
  }
  if (http_ver) {
    sess->http_ver = http_ver;
  }

  char *transport = (client_ssl_ctx)? "TLS" : "TCP";
  if (sess->http_ver == H2_HTTP_V2) {
    /* HTTP2 initial message */
    if (h2_sess_send_settings(sess, settings) < 0) {
      h2_sess_free(sess);
      return NULL;
    }
    fprintf(stderr, "%sCONNECTED %s HTTP/2 TO %s\n",
            sess->log_prefix, transport, authority);
  } else if (sess->http_ver == H2_HTTP_V2_TRY) {
    /* try to upgrade to HTTP2; TCP only */
#if 0  /* TODO: TO BE IMPLEMENTED */
    if (h2_sess_send_upgrade_req_tcp(sess, authority, settings) < 0) {
      h2_sess_free(sess);
      return NULL;
    }
#endif
    fprintf(stderr, "%sCONNECTED %s HTTP/2-TRY TO %s\n",
            sess->log_prefix, transport, authority);
  } else {
    /* HTTP/1.1 */
    fprintf(stderr, "%sCONNECTED %s HTTP/1.1 TO %s\n",
            sess->log_prefix, transport, authority);
  }

  return sess;
}

/* Start connecting to the remote peer |host:port| */
h2_sess *h2_connect(h2_ctx *ctx, const char *authority, SSL_CTX *cli_ssl_ctx,
                    h2_settings *settings,
                    h2_response_cb response_cb,
                    h2_push_promise_cb push_promise_cb,
                    h2_push_response_cb push_response_cb,
                    h2_sess_free_cb sess_free_cb, void *sess_user_data) {

  if ((push_promise_cb && !push_response_cb) ||
      (!push_promise_cb && push_response_cb)) {
    warnx("push_promise_cb and push_response_cb should be set conicide");
    return NULL;
  }

  /* get host and port from req[0].authority */
  char *port, *host = strdup(authority);
  int n;
  if ((port = strrchr(host, ':'))) {
    *(port++) = '\0';  /* close host string and skip ':' */
  }
  if (host[0] == '[' && host[(n = strlen(host)) - 1] == ']' && n >= 3) {
    /* '[ipv6_address]' case */
    memmove(host, host + 1, n - 2);
    host[n - 2] = '\0';
  }
  if (strlen(host) <= 0 || port == 0) {
    warnx("invalid first authority value; should be ip:port formatted: %s",
          authority);
    free(host);
    return NULL;
  }

  struct addrinfo hints;
  struct addrinfo *res, *ai;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif
  hints.ai_protocol = 0;
  if (getaddrinfo(host, port, &hints, &res)) {
    warnx("cannot resolve server address: %s: host='%s' port='%s'", authority,
          host, port);
    free(host);
    return NULL;
  }
  free(host);

  h2_sess *sess = NULL;
  for (ai = res; ai; ai = ai->ai_next) {
    int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock >= 0) {
      h2_set_close_exec(sock);
      if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
        /* connect succeeded */
        if ((sess = h2_sess_client_start(sock, ctx, authority,
                                         cli_ssl_ctx, settings))) {
          break;
        }
      }
      close(sock);
    }
  }
  freeaddrinfo(res);
  if (sess == NULL) {
    warnx("cannot connect to %s", authority);
    return NULL;
  }

  /* init user data and callbacks */ 
  sess->response_cb = response_cb;
  sess->push_promise_cb = push_promise_cb;
  sess->push_response_cb = push_response_cb;
  sess->sess_free_cb = sess_free_cb;
  sess->user_data = sess_user_data;

  h2_set_nonblock(sess->fd);

  return sess;
}


/*
 * Server Session I/O ------------------------------------------------------
 */

static int h2_sess_server_tcp_start(h2_sess *sess, h2_settings *settings) {
  h2_sess_nghttp2_init(sess);

  if (sess->http_ver == H2_HTTP_V2) {
    if (h2_sess_send_settings(sess, settings) < 0) {
      return -1;
    } 
    fprintf(stderr, "%sCONNECTED TCP HTTP/2\n", sess->log_prefix);
  } else if (sess->http_ver == H2_HTTP_V1_1) {
    fprintf(stderr, "%sCONNECTED TCP HTTP/1.1\n", sess->log_prefix);
  } else {
    /* NOTE: on client's setting received, h2_sess_send_settings() is called */
    fprintf(stderr, "%sCONNECTED TCP HTTP/1.1 UPGRADABLE TO HTTP/2\n",
            sess->log_prefix);
  }
  return 0;
}

#ifdef TLS_MODE
static int h2_sess_server_tls_start(h2_sess *sess, h2_settings *settings) {
  const unsigned char *alpn = NULL;
  unsigned int alpnlen = 0;
  SSL *ssl = sess->ssl;

  SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
  if (alpn && alpnlen == 2 && !memcmp("h2", alpn, 2)) {
    sess->http_ver = H2_HTTP_V2;
  } else {
    if (sess->http_ver == H2_HTTP_V2) {
      warnx("%stls alpn h2 is not negotiated: alpn=%p alpnlen=%d",
            sess->log_prefix, alpn, alpnlen);
      return -1;
    }
    /* else, sess->http_ver == H2_HTTP_V2_TRY */
    sess->http_ver = H2_HTTP_V1_1;
  }

  if (sess->http_ver == H2_HTTP_V2) {
    h2_sess_nghttp2_init(sess);
    if (h2_sess_send_settings(sess, settings) < 0) {
       return -1;
    }
    fprintf(stderr, "%sCONNECTED TLS HTTP/2\n", sess->log_prefix);
  } else {
    fprintf(stderr, "%sCONNECTED TLS HTTP/1.1\n", sess->log_prefix);
  }
  return 0;
}
#endif

static h2_sess *h2_sess_init_server(h2_ctx *ctx, h2_svr *svr, int fd, 
                                    struct sockaddr *sa, socklen_t salen) {
  /* NOTE: on error, fd is closed */

  h2_sess *sess = calloc(1, sizeof(h2_sess));
  sess->obj.cls = &h2_cls_sess;

  /* insert into ctx session list */
  sess->next = ctx->sess_list_head.next;
  ctx->sess_list_head.next = sess;
  sess->prev = &ctx->sess_list_head;
  if (sess->next) {
    sess->next->prev = sess;
  }
  ctx->sess_num++;

  sess->ctx = ctx;
  sess->http_ver = ctx->http_ver;
  sess->is_server = 1;

  /* mark start time */
  gettimeofday(&sess->tv_begin, NULL);

  /* get log prefix info */
  char host[NI_MAXHOST], serv[NI_MAXSERV];
  if (getnameinfo(sa, salen, host, sizeof(host),
                  serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV)) {
    sess->log_prefix = strdup("(unknown)");
  } else {
    char log_prefix[1 + NI_MAXHOST + 1 + 1 + NI_MAXSERV + 1];
    if (sa->sa_family == AF_INET6) {
       sprintf(log_prefix, "[%s]:%s ", host, serv);
    } else {
       sprintf(log_prefix, "%s:%s ", host, serv);
    }
    sess->log_prefix = strdup(log_prefix);
  }
  unsigned short port = atoi(serv);

  /* do blocking, no wait send */
  int v = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
  sess->fd = fd;

  h2_settings sess_settings;
  h2_settings_init(&sess_settings);

#ifdef EPOLL_MODE
  struct epoll_event e;
  e.events = EPOLLIN;
  e.data.ptr = &sess->obj;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, sess->fd, &e) < 0) {
    warnx("sess server init failed for epoll_ctl() error: %s", strerror(errno));
    h2_svr_free(svr);
    return NULL;
  }
#endif

  /* call user accept callback */
  SSL_CTX *sess_ssl_ctx = NULL;
  if (svr->accept_cb) {
    int r = svr->accept_cb(svr, svr->user_data, host, port,
                           &sess_ssl_ctx, &sess_settings,
                           &sess->request_cb,
                           &sess->sess_free_cb, &sess->user_data);
    if (r < 0) {
      warnx("%saccept_cb failed: %d", sess->log_prefix, r);
      sess->sess_free_cb = NULL;
      sess->user_data = NULL;
      h2_sess_free(sess);
      return NULL;
    }
  }

#ifdef TLS_MODE
  if (svr->ssl_ctx) {
    sess->ssl = SSL_new((sess_ssl_ctx)? sess_ssl_ctx : svr->ssl_ctx);
    if (!sess->ssl) {
      warnx("%scannot create ssl session: %s",
            sess->log_prefix, ERR_error_string(ERR_get_error(), NULL));
      h2_sess_free(sess);
      return NULL;
    }
    SSL_set_fd(sess->ssl, sess->fd);
    if (SSL_accept(sess->ssl) < 0) {
      warnx("%scannot create ssl session: %s",
            sess->log_prefix, ERR_error_string(ERR_get_error(), NULL));
      h2_sess_free(sess);
      return NULL;
    }
    if (h2_sess_server_tls_start(sess, &sess_settings) < 0)  {
      h2_sess_free(sess);
      return NULL;
    }
  } else
#endif
  {
    if (h2_sess_server_tcp_start(sess, &sess_settings) < 0) {
      h2_sess_free(sess);
      return NULL;
    }
  }

  h2_set_nonblock(sess->fd);

  return sess;
}

h2_svr *h2_listen(h2_ctx *ctx, const char *authority, SSL_CTX *svr_ssl_ctx,
                  h2_accept_cb accept_cb,
                  h2_svr_free_cb svr_free_cb, void *svr_user_data) { /* get host and port from req[0].authority */
  char *port, *host = strdup(authority);
  int n;
  if ((port = strrchr(host, ':'))) {
    *(port++) = '\0';  /* close host string and skip ':' */
  }
  if (host[0] == '[' && host[(n = strlen(host)) - 1] == ']' && n >= 3) {
    /* '[ipv6_address]' case */
    memmove(host, host + 1, n - 2);
    host[n - 2] = '\0';
  }
  if (port == NULL) {
    warnx("invalid first authority value; should be ip:port formatted: %s",
          authority);
    free(host);
    return NULL;
  }

  struct addrinfo hints;
  struct addrinfo *res, *ai;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif
  hints.ai_protocol = 0;
  if (getaddrinfo(host, port, &hints, &res)) {
    warnx("cannot resolve server address: %s", authority);
    free(host);
    return NULL;
  }
  free(host);

  int v = 1, sock = -1;
  for (ai = res; ai; ai = ai->ai_next) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock >= 0) {
      h2_set_close_exec(sock);
      if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) < 0) {
        warnx("setsockopt(SO_REUSEADDR) failed; go ahead: %s",
              authority);
      }
      if (bind(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
        if (listen(sock, 1024/* TO BE TUNED WITH SYSTEM SOMAXCONN */) == 0) {
          break;
        }
        warnx("Listen failed: %s error=%s", authority, strerror(errno));
      }
      close(sock);
      sock = -1;
    }
  }
  freeaddrinfo(res);
  if (sock < 0) {
    warnx("cannot listen on %s", authority);
    return NULL;
  }
  /* now, sock is valid listen socket */
  /* ASSUME: authority is not conflicting for bind() already checked */

  h2_svr *svr = calloc(1, sizeof(h2_svr));
  svr->obj.cls = &h2_cls_svr;

  /* insert into ctx server list */
  svr->next = ctx->svr_list_head.next;
  ctx->svr_list_head.next = svr;
  svr->prev = &ctx->svr_list_head;
  if (svr->next) {
    svr->next->prev = svr;
  }
  ctx->svr_num++;

  svr->ctx = ctx;
  svr->authority = strdup(authority);
  svr->ssl_ctx = svr_ssl_ctx;
  svr->accept_fd = sock;

  svr->accept_cb = accept_cb;
  svr->svr_free_cb = svr_free_cb;
  svr->user_data = svr_user_data;

#ifdef EPOLL_MODE
  struct epoll_event e;
  e.events = EPOLLIN;
  e.data.ptr = &svr->obj;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, svr->accept_fd, &e) < 0) {
    warnx("svr init failed for epoll_ctl() error: %s", strerror(errno));
    h2_svr_free(svr);
    return NULL;
  }
#endif

  fprintf(stderr, "listen %s for http2/%s\n",
          authority, (svr_ssl_ctx)? "tls" : "tcp");
  return svr;
}

void h2_svr_free(h2_svr *svr) {
  /* call server user data free callback */
  if (svr->svr_free_cb) {
    svr->svr_free_cb(svr, svr->user_data);
    svr->svr_free_cb = NULL;
    svr->user_data = NULL;
  }

  /* delete from ctx server list */
  svr->prev->next = svr->next;
  if (svr->next) {
    svr->next->prev = svr->prev;
  }
  svr->ctx->svr_num--;

  if (svr->accept_fd >= 0) {
#ifdef EPOLL_MODE
    epoll_ctl(svr->ctx->epoll_fd, EPOLL_CTL_DEL, svr->accept_fd, NULL);
#endif
    close(svr->accept_fd);
    svr->accept_fd = -1;
  }

  free(svr->authority);
  svr->authority = NULL;

  free(svr);
}

const char *h2_svr_authority(h2_svr *svr) {
  return (svr)? svr->authority : NULL;
}

SSL_CTX *h2_svr_ssl_ctx(h2_svr *svr) {
  return (svr)? svr->ssl_ctx : NULL;
}


/*
 * Context and Service Loop common for client and server --------------------
 */

h2_ctx *h2_ctx_init(int http_ver, int verbose) {
  h2_ctx *ctx = calloc(1, sizeof(h2_ctx));
  ctx->obj.cls = &h2_cls_ctx;

#ifdef EPOLL_MODE
  ctx->epoll_fd = epoll_create(1/* not used; just non zero */);
  if (ctx->epoll_fd < 0) {
    warnx("init failed for epoll create error: %s", strerror(errno));
    free(ctx);
    return NULL;
  }
#endif

  ctx->http_ver = http_ver;
  ctx->verbose = verbose;
  return ctx;
}

void h2_ctx_free(h2_ctx *ctx) {
  ctx->service_flag = 0;

  while (ctx->svr_list_head.next) {
    h2_svr_free(ctx->svr_list_head.next);
  }

  while (ctx->peer_list_head.next) {
    h2_peer_free(ctx->peer_list_head.next);
  }

  while (ctx->sess_list_head.next) {
    h2_sess_free(ctx->sess_list_head.next);
  }

#ifdef EPOLL_MODE
  if (ctx->epoll_fd >= 0) {
    close(ctx->epoll_fd);
    ctx->epoll_fd = -1;
  }
#endif

  free(ctx);
}

void h2_ctx_set_http_ver(h2_ctx *ctx, int http_ver) {
  if (ctx) {
    ctx->http_ver = http_ver;
  }
}

void h2_ctx_set_verbose(h2_ctx *ctx, int verbose) {
  if (ctx) {
    ctx->verbose = verbose;
  }
}

void h2_ctx_stop(h2_ctx *ctx) {
  if (ctx) {
    ctx->service_flag = 0;
  }
}

#ifdef EPOLL_MODE

void h2_ctx_run(h2_ctx *ctx) {
  ctx->service_flag = 1;

  int ea_max, ea_alloced = 1024;
  struct epoll_event *ea;  /* dynamic allcoed epoll_event[epe_alloced] */
  ea = malloc(sizeof(*ea) * ea_alloced); 

  while (ctx->service_flag) {
    /* prepare poll fd array */
    ea_max = ctx->sess_num + ctx->svr_num;
    if (ea_alloced < ea_max) {
      ea_alloced = ((ea_max + 16 + 1023) / 1024) * 1024;
      ea = realloc(ea, sizeof(*ea) * ea_alloced); 
      if (ea == NULL) {
        warnx("epoll event buffer realloc failed; quit run loop: size=%d",
              (int)(sizeof(*ea) * ea_alloced));
        break;
      }
    }
    if (ea_max <= 0) {
      break;  /* no more session to service */
    }

    /* wait for epoll event */
    int r = epoll_wait(ctx->epoll_fd, ea, ea_max, 100);
    if (r == 0 || (r < 0 && errno == EINTR)) {
      continue;
    } else if (r < 0) {
      warnx("epoll_wait() error; quit run loop: %s", strerror(errno));
      break;
    }

    /* check for h2 sess/srv socket */
    struct epoll_event *e = ea;
    int event_num;
    for (event_num = r ; event_num > 0; event_num--, e++) {
      int events = e->events;
      if (((h2_obj *)e->data.ptr)->cls == &h2_cls_svr) {
        /* server acccept event */
        h2_svr *svr = (void *)e->data.ptr;
        if ((events & EPOLLIN)) {
          struct sockaddr_in6 sa;  /* to allow ipv4 and ipv6 */
          socklen_t sa_len = sizeof(sa);  /* in/out argument */
          int fd = accept(svr->accept_fd, (struct sockaddr *)&sa, &sa_len);
          if (fd >= 0) {
            h2_set_close_exec(fd);
            h2_sess_init_server(ctx, svr, fd, (struct sockaddr *)&sa, sa_len);
          } else {
            warnx("accept() failed on server socket: %s", strerror(errno));
          }
        }
      } else if (((h2_obj *)e->data.ptr)->cls == &h2_cls_sess) {
        /* session rw event */
        h2_sess *sess = (void *)e->data.ptr;
        if ((events & EPOLLIN)) {
          if (h2_sess_recv(sess) < 0) {
            h2_sess_free(sess);
            continue;
          }
        }
        if ((events & (EPOLLOUT || EPOLLIN))) {  /* always do send after recv */
          if (sess->is_terminated == 1 && sess->http_ver != H2_HTTP_V2) {
            sess->close_reason = CLOSE_BY_HTTP_END;
            h2_sess_free(sess);
          } else {
            if (h2_sess_send(sess) < 0) {
              h2_sess_free(sess);
              continue;
            }
          }
        }
        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
          sess->close_reason = CLOSE_BY_SOCK_ERR;
          if (!sess->is_terminated) {
            warnx("socket errored: epoll_events=0x%02x sess=%s",
                  events, sess->log_prefix);
          }
          h2_sess_free(sess);
          continue;
        }
      }
    }
  }

  free(ea);
}

#else /* EPOLL_MODE; use poll() */

void h2_ctx_run(h2_ctx *ctx) {
  ctx->service_flag = 1;

  int pfd_alloced = 1024;
  struct pollfd *pfd = NULL;
  h2_obj **pfd_obj = NULL;
  pfd = malloc(sizeof(*pfd) * pfd_alloced); 
  pfd_obj = malloc(sizeof(*pfd_obj) * pfd_alloced); 

  while (ctx->service_flag) {
    /* prepare poll fd array */
    if (pfd_alloced < ctx->sess_num + ctx->svr_num) {
      pfd_alloced = ((ctx->sess_num + ctx->svr_num + 16 + 1023) / 1024) * 1024;
      pfd = realloc(pfd, sizeof(*pfd) * pfd_alloced); 
      pfd_obj = realloc(pfd_obj, sizeof(*pfd_obj) * pfd_alloced); 
    }
    /* fill pollfds and wait for events */
    int n = 0;
    h2_svr *svr;
    for (svr = ctx->svr_list_head.next; svr; svr = svr->next) {
      if (svr->accept_fd >= 0) {
        pfd[n].fd = svr->accept_fd;
        pfd[n].events = POLLIN;
        pfd_obj[n] = &svr->obj;
        n++;
      }
    }
    h2_sess *sess, *sess_next;
    for (sess = ctx->sess_list_head.next; sess; sess = sess_next) {
      sess_next = sess->next;  /* for sess free case */
      pfd[n].fd = sess->fd;
      pfd[n].events = 0;
      if (sess->http_ver == H2_HTTP_V2) {
        if (nghttp2_session_want_read(sess->ng_sess)) {
          pfd[n].events |= POLLIN;
        }
        if (sess->send_pending || nghttp2_session_want_write(sess->ng_sess)) {
          pfd[n].events |= POLLOUT;
        }
      } else {
        if (sess->is_terminated == 0) {
          pfd[n].events |= POLLIN;
        }
        if (sess->send_pending) {
          pfd[n].events |= POLLOUT;
        }
      }
      if (pfd[n].events == 0) {
        sess->close_reason = CLOSE_BY_HTTP_END;
        h2_sess_free(sess);
        continue;
      }
      pfd_obj[n] = &sess->obj;
      n++;
    }
    if (n == 0) { /* quit service if nothing to do */
      break;
    }

    /* wait for event */
    int r = poll(pfd, n, 100);
    if (r == 0 || (r < 0 && errno == EINTR)) {
      continue;
    } else if (r < 0) {
      warnx("poll() error; quit run loop: %s", strerror(errno));
      break;
    }

    /* check for h2 sess/srv socket */
    int i, event_num = r;
    for (i = 0; i < n && event_num > 0; i++) {
      if (pfd[i].revents == 0) {
        continue;
      }
      event_num--;
      int revents = pfd[i].revents;
      if (pfd_obj[i]->cls == &h2_cls_svr) {
        /* server acccept event */
        svr = (void *)pfd_obj[i];
        if ((revents & POLLIN)) {
          struct sockaddr_in6 sa;  /* to allow ipv4 and ipv6 */
          socklen_t sa_len = sizeof(sa);  /* in/out argument */
          int fd = accept(svr->accept_fd, (struct sockaddr *)&sa, &sa_len);
          if (fd >= 0) {
            h2_set_close_exec(fd);
            h2_sess_init_server(ctx, svr, fd, (struct sockaddr *)&sa, sa_len);
          } else {
            warnx("accept() failed on server socket: %s", strerror(errno));
          }
        }
      } else if (pfd_obj[i]->cls == &h2_cls_sess) {
        /* session rw event */
        sess = (void *)pfd_obj[i];
        if ((revents & POLLIN)) {
          if (h2_sess_recv(sess) < 0) {
            h2_sess_free(sess);
            continue;
          }
        }
        if ((revents & (POLLOUT || POLLIN))) {  /* always do send after recv */
          if (h2_sess_send(sess) < 0) {
            h2_sess_free(sess);
            continue;
          }
        }
        if ((revents & POLLRDHUP)) {
          warnx("socket closed by peer");
          sess->close_reason = CLOSE_BY_SOCK_EOF;
          h2_sess_free(sess);
          continue;
        }
        if ((revents & (POLLERR | POLLHUP | POLLNVAL))) {
          warnx("socket errored: revents=0x%02x", revents);
          sess->close_reason = CLOSE_BY_SOCK_ERR;
          h2_sess_free(sess);
          continue;
        }
      }
    }
  }

  free(pfd);
  free(pfd_obj);
}

#endif /* EPOLL_MODE */


/*
 * Client API Peer I/O -----------------------------------------------------
 */

/* forward declaration */
static h2_sess *peer_connect_sess(h2_peer *peer, int sess_idx);

static int peer_response_cb(h2_sess *sess, h2_msg *rsp,
                    void *sess_user_data, void *strm_user_data) {
  h2_peer *peer = sess_user_data;
  (void)sess;

  if (peer->response_cb) {
    return peer->response_cb(peer, rsp, peer->user_data, strm_user_data);
  }
  return 0;
}

static int peer_push_promise_cb(h2_sess *sess, h2_msg *prm_req,
                    void *sess_user_data, void *strm_user_data,
                    h2_strm_free_cb *push_strm_free_cb_ret,
                    void **push_strm_user_data_ret) {
  h2_peer *peer = sess_user_data;
  (void)sess;

  if (peer->push_promise_cb) {
    return peer->push_promise_cb(peer, prm_req, peer->user_data, strm_user_data,
                                push_strm_free_cb_ret, push_strm_user_data_ret);
  }
  return 0;
}

static int peer_push_response_cb(h2_sess *sess, h2_msg *prm_rsp,
                    void *sess_user_data, void *push_strm_user_data) {
  h2_peer *peer = sess_user_data;
  (void)sess;

  if (peer->push_response_cb) {
    return peer->push_response_cb(peer, prm_rsp, peer->user_data,
                                  push_strm_user_data);
  }
  return 0;
}

static void peer_sess_free_cb(h2_sess *sess, void *sess_user_data) {
  h2_peer *peer = sess_user_data;
  int i;

  for (i = 0; i < peer->sess_num; i++) {
    if (peer->sess[i] == sess) {
      break;
    }
  }
  if (i >= peer->sess_num) {
    warnx("peer_sess_free_cb:: unknown session for peer: peer=%s sess=%s",
          peer->authority, sess->log_prefix);
    return;
  }
  /* now, peer->sess[i] is known to be freed */

  /* gather stat from sess */
  peer->req_cnt += sess->req_cnt;
  peer->rsp_cnt += sess->rsp_cnt;
  peer->rsp_rst_cnt += sess->rsp_rst_cnt;
  peer->strm_close_cnt += sess->strm_close_cnt;
  peer->sess_close_cnt++;

  peer->sess[i] = NULL; 
  if (peer->act_sess[i]) {
    peer->act_sess[i] = 0;
    peer->act_sess_num--;
  }

  /* try reconnect is peer or ctx is not termiating */
  if (!peer->is_terminated && peer->ctx->service_flag) {
    peer_connect_sess(peer, i);
  }
}

static h2_sess *peer_connect_sess(h2_peer *peer, int sess_idx) {
  h2_sess *sess = h2_connect(peer->ctx, peer->authority, peer->ssl_ctx,
                             &peer->settings,
                             peer_response_cb,
                             peer_push_promise_cb, peer_push_response_cb,
                             peer_sess_free_cb, peer);
  if (sess) {
    peer->sess[sess_idx] = sess;
    if (!peer->act_sess[sess_idx]) {
      /* init peers sess status */
      peer->act_sess[sess_idx] = 1;
      peer->act_sess_num++;
    }
  }
  return sess;
}

/* client side context create api to start sessions */
h2_peer *h2_peer_connect(int sess_num, int req_thr_for_reconn,
                    h2_ctx *ctx, const char *authority,
                    SSL_CTX *cli_ssl_ctx,
                    h2_settings *settings,
                    h2_peer_response_cb response_cb,
                    h2_peer_push_promise_cb push_promise_cb,
                    h2_peer_push_response_cb push_response_cb,
                    h2_peer_free_cb peer_free_cb, void *peer_user_data) {
  h2_peer *peer;

  if (req_thr_for_reconn != 0 && sess_num == 1) {
    req_thr_for_reconn = 0;
    warnx("h2_peer_connect:: ignore req_thr_for_reconn for sess_num=1: "
          "authority=%s", authority);
  }

  peer = calloc(1, sizeof(*peer)); 
  peer->obj.cls = &h2_cls_peer;

  /* add to ctx peer list */
  peer->next = ctx->peer_list_head.next;
  ctx->peer_list_head.next = peer;
  peer->prev = &ctx->peer_list_head;
  if (peer->next) {
    peer->next->prev = peer; }
  ctx->peer_num++;
  peer->ctx = ctx;

  peer->sess_num = sess_num;
  peer->req_thr_for_reconn = req_thr_for_reconn;

  peer->authority = strdup(authority);
  peer->ssl_ctx = cli_ssl_ctx;
  if (settings) {
    peer->settings = *settings;
  } else {
    h2_settings_init(&peer->settings);
  }

  peer->sess = calloc(sess_num, sizeof(*peer->sess));
  peer->next_sess_idx = 0;
  peer->act_sess = calloc(sess_num, sizeof(*peer->act_sess));
  peer->act_sess_num = 0;

  /* mark start time */
  peer->sess_close_cnt = 0;
  peer->strm_close_cnt = 0;
  gettimeofday(&peer->tv_begin, NULL);

  /* connect to peer as sess_num sessions */
  int i;
  for (i = 0; i < sess_num; i++) {
    peer_connect_sess(peer, i);
  }
  if (peer->act_sess_num <= 0) {
    warnx("cannot connect to peer: %s", authority);
    h2_peer_free(peer);
    return NULL;
  }

  /* lazy callbacks assigned for no callback on intial connect failure cases */
  peer->response_cb = response_cb;
  peer->push_promise_cb = push_promise_cb;
  peer->push_response_cb = push_response_cb;
  peer->peer_free_cb = peer_free_cb;
  peer->user_data = peer_user_data;

  return peer;
}

void h2_peer_free(h2_peer *peer) {
  int i;

  /* free all sessions */
  for (i = 0; i < peer->sess_num; i++) {
    if (peer->sess[i]) {
      h2_sess_free(peer->sess[i]);
      peer->sess[i] = NULL;
    }
  }

  /* free user data */
  if (peer->peer_free_cb) {
    peer->peer_free_cb(peer, peer->user_data);
    peer->peer_free_cb = NULL;
    peer->user_data = NULL;
  }

  /* delete from ctx sess list */
  peer->prev->next = peer->next;
  if (peer->next) {
    peer->next->prev = peer->prev;
  }
  peer->ctx->peer_num--;
  peer->ctx = NULL;

  peer->ssl_ctx = NULL;

  /* show performance */
  gettimeofday(&peer->tv_end, NULL);
  double elapsed =
     ((peer->tv_end.tv_sec - peer->tv_begin.tv_sec) * 1.0 +
      (peer->tv_end.tv_usec - peer->tv_begin.tv_usec) * 0.000001);
  if (peer->sess_num > 1) {
    fprintf(stderr, "PEER CLOSED %s: %.0f tps (%.3f secs for "
            "%d reqs %d rsps(%d rsts) %d streams in %d sessions)%s\n",
            peer->authority, peer->strm_close_cnt / elapsed,
            elapsed, peer->req_cnt, peer->rsp_cnt, peer->rsp_rst_cnt,
            peer->strm_close_cnt, peer->sess_close_cnt,
            (peer->req_cnt != peer->rsp_cnt|| peer->rsp_rst_cnt)? " !!!" : "");
  }

  free(peer->authority);
  free(peer->sess);
  free(peer->act_sess);
  free(peer);
}

/* h2 client application api for request on peer with sess load balancing */
int h2_peer_send_request(h2_peer *peer, h2_msg *req,
                         h2_strm_free_cb strm_free_cb, void *strm_user_data) {
  h2_sess *sess = NULL;
  int i, r, n = peer->sess_num, nsi = peer->next_sess_idx;

  if (peer->is_terminated) {
    warnx("cannot send request for peer is terminated: %s\n", peer->authority);
    return -1;
  }

  /* find active session with round-robin load balancing */
  for (i = 0; i < n; i++) {
    int si = (nsi + i) % n;
    if ((sess = peer->sess[si]) && peer->act_sess[si])  {
      /* house keep for to-be-terminated */
      if (peer->req_thr_for_reconn > 0 &&
          sess->req_cnt >= peer->req_thr_for_reconn &&
          peer->act_sess_num >= peer->sess_num) {
        /* terminate for too may requests handled */
        if (peer->act_sess[si]) {  /* update before sess terminate call */
          peer->act_sess[si] = 0;
          peer->act_sess_num--;
        }
        h2_sess_terminate(sess, 1/* wait_rsp */);
        sess = NULL;  /* try other sess */
      } else {
        /* use this session */
        break;
      }
    }
  }
  peer->next_sess_idx = (nsi + i + 1) % n;  /* advances even no valid sess */

  if (sess == NULL) {
    /* TODO: try to connect server */
  }

  if (sess) {
    r = h2_send_request(sess, req, strm_free_cb, strm_user_data);
  } else {
    warnx("no session available to peer: %s", peer->authority);
    r = -1;
  }

  /* try to house keep till act_sess_num */
  if (sess && peer->act_sess_num < peer->sess_num) {
    /* TODO: try to connect server */ 
  }

  return r;
}

/* terminalte all sessions on the peer */
int h2_peer_terminate(h2_peer *peer, int wait_rsp) {
  int i;

  if (peer == NULL || peer->is_terminated == 1) {
    return 1;
  }

  peer->is_terminated = (wait_rsp)? 2 : 1;  /* 1:immediate, 2:wait_rsp */

  for (i = 0; i < peer->sess_num; i++) {
    if (peer->act_sess[i]) {
      peer->act_sess[i] = 0;
      peer->act_sess_num--;
    }
    h2_sess_terminate(peer->sess[i], wait_rsp);  /* go ahread even on error */
  }
  return 0;
}

