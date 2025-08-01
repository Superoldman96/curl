/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "first.h"

#include "testtrace.h"
#include "memdebug.h"

#ifndef CURL_DISABLE_WEBSOCKETS

static CURLcode check_recv(const struct curl_ws_frame *frame,
                           size_t r_offset, size_t nread, size_t exp_len)
{
  if(!frame)
    return CURLE_OK;

  if(frame->flags & CURLWS_CLOSE) {
    curl_mfprintf(stderr, "recv_data: unexpected CLOSE frame from server, "
                  "got %zu bytes, offset=%zu, rflags %x\n",
                  nread, r_offset, frame->flags);
    return CURLE_RECV_ERROR;
  }
  if(!r_offset && !(frame->flags & CURLWS_BINARY)) {
    curl_mfprintf(stderr, "recv_data: wrong frame, got %zu bytes, offset=%zu, "
                  "rflags %x\n",
                  nread, r_offset, frame->flags);
    return CURLE_RECV_ERROR;
  }
  if(frame->offset != (curl_off_t)r_offset) {
    curl_mfprintf(stderr, "recv_data: frame offset, expected %zu, "
                  "got %" CURL_FORMAT_CURL_OFF_T "\n",
                  r_offset, frame->offset);
    return CURLE_RECV_ERROR;
  }
  if(frame->bytesleft != (curl_off_t)(exp_len - r_offset - nread)) {
    curl_mfprintf(stderr, "recv_data: frame bytesleft, "
                  "expected %" CURL_FORMAT_CURL_OFF_T ", "
                  "got %" CURL_FORMAT_CURL_OFF_T "\n",
                  (curl_off_t)(exp_len - r_offset - nread), frame->bytesleft);
    return CURLE_RECV_ERROR;
  }
  if(r_offset + nread > exp_len) {
    curl_mfprintf(stderr, "recv_data: data length, expected %zu, now at %zu\n",
                  exp_len, r_offset + nread);
    return CURLE_RECV_ERROR;
  }
  return CURLE_OK;
}

static CURLcode data_echo(CURL *curl, size_t count,
                          size_t plen_min, size_t plen_max)
{
  CURLcode r = CURLE_OK;
  const struct curl_ws_frame *frame;
  size_t len;
  char *send_buf = NULL, *recv_buf = NULL;
  size_t i, scount = count, rcount = count;
  int rblock, sblock;

  send_buf = calloc(1, plen_max + 1);
  recv_buf = calloc(1, plen_max + 1);
  if(!send_buf || !recv_buf) {
    r = CURLE_OUT_OF_MEMORY;
    goto out;
  }

  for(i = 0; i < plen_max; ++i) {
    send_buf[i] = (char)('0' + ((int)i % 10));
  }

  for(len = plen_min; len <= plen_max; ++len) {
    size_t nwritten, nread, slen = len, rlen = len;
    char *sbuf = send_buf, *rbuf = recv_buf;

    memset(recv_buf, 0, plen_max);
    while(slen || rlen || scount || rcount) {
      sblock = rblock = 1;
      if(slen) {
        r = curl_ws_send(curl, sbuf, slen, &nwritten, 0, CURLWS_BINARY);
        sblock = (r == CURLE_AGAIN);
        if(!r || (r == CURLE_AGAIN)) {
          curl_mfprintf(stderr, "curl_ws_send(len=%zu) -> %d, "
                        "%zu (%" CURL_FORMAT_CURL_OFF_T "/%zu)\n",
                        slen, r, nwritten, (curl_off_t)(len - slen), len);
          sbuf += nwritten;
          slen -= nwritten;
        }
        else
          goto out;
      }
      if(!slen && scount) { /* go again? */
        scount--;
        sbuf = send_buf;
        slen = len;
      }

      if(rlen) {
        size_t max_recv = (64 * 1024);
        r = curl_ws_recv(curl, rbuf, (rlen > max_recv) ? max_recv : rlen,
                         &nread, &frame);
        if(!r || (r == CURLE_AGAIN)) {
          rblock = (r == CURLE_AGAIN);
          curl_mfprintf(stderr, "curl_ws_recv(len=%zu) -> %d, %zu (%ld/%zu) "
                        "\n", rlen, r, nread, (long)(len - rlen), len);
          if(!r) {
            r = check_recv(frame, len - rlen, nread, len);
            if(r)
              goto out;
          }
          rbuf += nread;
          rlen -= nread;
        }
        else
          goto out;
      }
      if(!rlen && rcount) { /* go again? */
        rcount--;
        rbuf = recv_buf;
        rlen = len;
      }

      if(rblock && sblock) {
        curl_mfprintf(stderr, "EAGAIN, sleep, try again\n");
        curlx_wait_ms(100);
      }
    }

    if(memcmp(send_buf, recv_buf, len)) {
      curl_mfprintf(stderr, "recv_data: data differs\n");
      debug_dump("", "expected:", stderr,
                 (const unsigned char *)send_buf, len, FALSE);
      debug_dump("", "received:", stderr,
                 (const unsigned char *)recv_buf, len, FALSE);
      r = CURLE_RECV_ERROR;
      goto out;
    }
  }

out:
  if(!r)
    ws_close(curl);
  free(send_buf);
  free(recv_buf);
  return r;
}

static void usage_ws_data(const char *msg)
{
  if(msg)
    curl_mfprintf(stderr, "%s\n", msg);
  curl_mfprintf(stderr,
    "usage: [options] url\n"
    "  -m number  minimum frame size\n"
    "  -M number  maximum frame size\n"
  );
}

#endif

static CURLcode test_cli_ws_data(const char *URL)
{
#ifndef CURL_DISABLE_WEBSOCKETS
  CURL *curl;
  CURLcode res = CURLE_OK;
  const char *url;
  size_t plen_min = 0, plen_max = 0, count = 1;
  int ch;

  (void)URL;

  while((ch = cgetopt(test_argc, test_argv, "c:hm:M:")) != -1) {
    switch(ch) {
    case 'h':
      usage_ws_data(NULL);
      res = CURLE_BAD_FUNCTION_ARGUMENT;
      goto cleanup;
    case 'c':
      count = (size_t)strtol(coptarg, NULL, 10);
      break;
    case 'm':
      plen_min = (size_t)strtol(coptarg, NULL, 10);
      break;
    case 'M':
      plen_max = (size_t)strtol(coptarg, NULL, 10);
      break;
    default:
      usage_ws_data("invalid option");
      res = CURLE_BAD_FUNCTION_ARGUMENT;
      goto cleanup;
    }
  }
  test_argc -= coptind;
  test_argv += coptind;

  if(!plen_max)
    plen_max = plen_min;

  if(plen_max < plen_min) {
    curl_mfprintf(stderr, "maxlen must be >= minlen, got %zu-%zu\n",
                  plen_min, plen_max);
    res = CURLE_BAD_FUNCTION_ARGUMENT;
    goto cleanup;
  }

  if(test_argc != 1) {
    usage_ws_data(NULL);
    res = CURLE_BAD_FUNCTION_ARGUMENT;
    goto cleanup;
  }
  url = test_argv[0];

  curl_global_init(CURL_GLOBAL_ALL);

  curl = curl_easy_init();
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* use the callback style */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ws-data");
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */
    res = curl_easy_perform(curl);
    curl_mfprintf(stderr, "curl_easy_perform() returned %u\n", res);
    if(res == CURLE_OK)
      res = data_echo(curl, count, plen_min, plen_max);

    /* always cleanup */
    curl_easy_cleanup(curl);
  }

cleanup:
  curl_global_cleanup();
  return res;

#else /* !CURL_DISABLE_WEBSOCKETS */
  (void)URL;
  curl_mfprintf(stderr, "WebSockets not enabled in libcurl\n");
  return (CURLcode)1;
#endif /* CURL_DISABLE_WEBSOCKETS */
}
