/*
 * Copyright (C) 2015 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uninorm.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/buffer.h>

#include "httpd_internal.h"
#include "httpd_streaming.h"
#include "logger.h"
#include "conffile.h"
#include "listener.h"

// Linked list of streaming requests
// TODO is pretty much the same as httpd_streaming_clients - abolish?
struct streaming_session {
  struct httpd_request *hreq;
  struct event *readev;

  enum streaming_format format;
  struct media_quality quality;

  struct streaming_session *next;
};

static struct media_quality streaming_default_quality = {
  .sample_rate = 44100,
  .bits_per_sample = 16,
  .channels = 2,
  .bit_rate = 128000,
};

static pthread_mutex_t streaming_sessions_lock;
static struct streaming_session *streaming_sessions;

/* As streaming quality goes up, we send more data to the remote client.  With a
 * smaller ICY_METAINT value we have to splice metadata more frequently - on
 * some devices with small input buffers, a higher quality stream and low
 * ICY_METAINT can lead to stuttering as observed on a Roku Soundbridge
 */
static unsigned short streaming_icy_metaint = 16384;

// Forward
static void
read_cb(evutil_socket_t fd, short event, void *arg);



static void
session_free(struct streaming_session *session)
{
  if (!session)
    return;

  if (session->readev)
    event_free(session->readev);

  free(session);
}

static void
session_remove(struct streaming_session *session)
{
  struct streaming_session *prev = NULL;
  struct streaming_session *s;

  pthread_mutex_lock(&streaming_sessions_lock);

  for (s = streaming_sessions; s; s = s->next)
    {
      if (s == session)
	break;

      prev = s;
    }

  if (!s)
    {
      DPRINTF(E_LOG, L_STREAMING, "Streaming session to remove not found in list\n");
      goto notfound;
    }

  if (!prev)
    streaming_sessions = session->next;
  else
    prev->next = session->next;

  session_free(session);

 notfound:
  pthread_mutex_unlock(&streaming_sessions_lock);
}

static struct streaming_session *
session_new(struct httpd_request *hreq, enum streaming_format format, struct media_quality quality)
{
  struct streaming_session *session;

  CHECK_NULL(L_STREAMING, session = calloc(1, sizeof(struct streaming_session)));

  session->hreq = hreq;
  session->format = format;
  session->quality = quality;

  pthread_mutex_lock(&streaming_sessions_lock);
  session->next = streaming_sessions;
  streaming_sessions = session;
  pthread_mutex_unlock(&streaming_sessions_lock);

  return session;
}

static void
session_readev_add(struct streaming_session *session, int fd)
{
  struct event_base *evbase;

  pthread_mutex_lock(&streaming_sessions_lock);

  CHECK_NULL(L_STREAMING, evbase = httpd_request_evbase_get(session->hreq));
  CHECK_NULL(L_STREAMING, session->readev = event_new(evbase, fd, EV_READ | EV_PERSIST, read_cb, session));
  event_add(session->readev, NULL);

  pthread_mutex_unlock(&streaming_sessions_lock);
}

static struct httpd_streaming_client *
session_to_clientinfo(void)
{
  struct httpd_streaming_client *clientinfo = NULL;
  struct httpd_streaming_client *client;
  struct streaming_session *session;

  pthread_mutex_lock(&streaming_sessions_lock);
  for (session = streaming_sessions; session; session = session->next)
    {
      client = calloc(1, sizeof(struct httpd_streaming_client));
      client->session = session;
      client->format = session->format;
      client->quality = session->quality;

      client->next = clientinfo;
      clientinfo = client;
    }
  pthread_mutex_unlock(&streaming_sessions_lock);

  return clientinfo;
}

static bool
session_is_valid(struct streaming_session *session)
{
  struct streaming_session *s;

  pthread_mutex_lock(&streaming_sessions_lock);
  for (s = streaming_sessions; s; s = s->next)
    {
      if (s == session)
	break;
    }
  pthread_mutex_unlock(&streaming_sessions_lock);

  return s ? true : false;
}


static void
conn_close_cb(httpd_connection *conn, void *arg)
{
  struct streaming_session *session = arg;

  if (!session_is_valid(session))
    {
      DPRINTF(E_WARN, L_STREAMING, "conn_close_cb() for deleted session detected\n");
      return;
    }

  DPRINTF(E_INFO, L_STREAMING, "Stopping mp3 streaming to %s:%d\n", session->hreq->peer_address, (int)session->hreq->peer_port);

  // Valgrind says libevent doesn't free the request on disconnect (even though it owns it - libevent bug?),
  // so we do it with a reply end
  // TODO on_complete won't be triggered so hreq memleaks
  httpd_send_reply_end(session->hreq);

  session_remove(session);

  listener_notify(LISTENER_STREAMING);
}

static void
read_cb(evutil_socket_t fd, short event, void *arg)
{
  struct streaming_session *session = arg;
  struct httpd_request *hreq = session->hreq;

  evbuffer_read(hreq->out_body, fd, -1);

//  DPRINTF(E_DBG, L_STREAMING, "Read %zu bytes from the streaming pipe\n", len);

  httpd_send_reply_chunk(hreq, hreq->out_body, NULL, NULL);
}


static int
streaming_mp3_handler(struct httpd_request *hreq)
{
  enum streaming_format format;
  struct streaming_session *session;
  const char *name = cfg_getstr(cfg_getsec(cfg, "library"), "name");
  const char *param;
  char buf[9];

  param = httpd_header_find(hreq->in_headers, "Icy-MetaData");
  if (param && strcmp(param, "1") == 0)
    {
      format = STREAMING_FORMAT_MP3_ICY;
      httpd_header_add(hreq->out_headers, "icy-name", name);
      snprintf(buf, sizeof(buf)-1, "%d", streaming_icy_metaint);
      httpd_header_add(hreq->out_headers, "icy-metaint", buf);
    }
  else
    format = STREAMING_FORMAT_MP3;

  session = session_new(hreq, format, streaming_default_quality);
  if (!session)
    return -1;

  // So we also can make a listener call to stop producing audio on disconnect
  httpd_request_closecb_set(hreq, conn_close_cb, session);

  httpd_header_add(hreq->out_headers, "Content-Type", "audio/mpeg");
  httpd_header_add(hreq->out_headers, "Server", PACKAGE_NAME "/" VERSION);
  httpd_header_add(hreq->out_headers, "Cache-Control", "no-cache");
  httpd_header_add(hreq->out_headers, "Pragma", "no-cache");
  httpd_header_add(hreq->out_headers, "Expires", "Mon, 31 Aug 2015 06:00:00 GMT");

  httpd_send_reply_start(hreq, HTTP_OK, "OK");

  return 0;
}

static struct httpd_uri_map streaming_handlers[] =
  {
    {
      .regexp = "^/stream.mp3$",
      .handler = streaming_mp3_handler
    },
    {
      .regexp = NULL,
      .handler = NULL
    }
  };

static void
streaming_request(struct httpd_request *hreq)
{
  int ret;

  if (!hreq->handler)
    {
      DPRINTF(E_LOG, L_STREAMING, "Unrecognized path in streaming request: '%s'\n", hreq->uri);

      httpd_send_error(hreq, HTTP_NOTFOUND, NULL);
      return;
    }

  // Will make outputs/streaming.c start producing audio
  listener_notify(LISTENER_STREAMING);

  ret = hreq->handler(hreq);
  if (ret < 0)
    httpd_send_error(hreq, HTTP_INTERNAL, NULL);
}

static int
streaming_init(void)
{
  int val;

  val = cfg_getint(cfg_getsec(cfg, "streaming"), "sample_rate");
  // Validate against the variations of libmp3lame's supported sample rates: 32000/44100/48000
  if (val % 11025 > 0 && val % 12000 > 0 && val % 8000 > 0)
    DPRINTF(E_LOG, L_STREAMING, "Unsupported streaming sample_rate=%d, defaulting\n", val);
  else
    streaming_default_quality.sample_rate = val;

  val = cfg_getint(cfg_getsec(cfg, "streaming"), "bit_rate");
  switch (val)
  {
    case  64:
    case  96:
    case 128:
    case 192:
    case 320:
      streaming_default_quality.bit_rate = val*1000;
      break;

    default:
      DPRINTF(E_LOG, L_STREAMING, "Unsuppported streaming bit_rate=%d, supports: 64/96/128/192/320, defaulting\n", val);
  }

  DPRINTF(E_INFO, L_STREAMING, "Streaming quality: %d/%d/%d @ %dkbps\n",
    streaming_default_quality.sample_rate, streaming_default_quality.bits_per_sample,
    streaming_default_quality.channels, streaming_default_quality.bit_rate/1000);

  val = cfg_getint(cfg_getsec(cfg, "streaming"), "icy_metaint");
  // Too low a value forces server to send more meta than data
  if (val >= 4096 && val <= 131072)
    streaming_icy_metaint = val;
  else
    DPRINTF(E_INFO, L_STREAMING, "Unsupported icy_metaint=%d, supported range: 4096..131072, defaulting to %d\n", val, streaming_icy_metaint);

  CHECK_ERR(L_STREAMING, mutex_init(&streaming_sessions_lock));

  return 0;
}

static void
streaming_deinit(void)
{
  struct streaming_session *session;

  pthread_mutex_lock(&streaming_sessions_lock);
  for (session = streaming_sessions; streaming_sessions; session = streaming_sessions)
    {
      DPRINTF(E_INFO, L_STREAMING, "Force close stream to %s:%d\n", session->hreq->peer_address, (int)session->hreq->peer_port);

      httpd_request_closecb_set(session->hreq, NULL, NULL);
      httpd_send_reply_end(session->hreq);

      streaming_sessions = session->next;
      session_free(session);
    }
  pthread_mutex_unlock(&streaming_sessions_lock);

  // TODO listener notify?
}

void
httpd_streaming_clientinfo_free(struct httpd_streaming_client *clients)
{
  // TODO
}

struct httpd_streaming_client *
httpd_streaming_clientinfo_get(void)
{
  return session_to_clientinfo();
}

int
httpd_streaming_fd_set(struct httpd_streaming_client *client, int fd)
{
  struct streaming_session *session = client->session;

  if (!session_is_valid(session))
    {
      DPRINTF(E_WARN, L_STREAMING, "fd_set() for deleted session detected\n");
      return -1;
    }

  DPRINTF(E_DBG, L_STREAMING, "Client %p will watch fd %d for streaming data\n", client, fd);

  session_readev_add(session, fd);

  return 0;
}

struct httpd_module httpd_streaming =
{
  .name = "Streaming",
  .type = MODULE_STREAMING,
  .logdomain = L_STREAMING,
  .fullpaths = { "/stream.mp3", NULL },
  .handlers = streaming_handlers,
  .init = streaming_init,
  .deinit = streaming_deinit,
  .request = streaming_request,
};
