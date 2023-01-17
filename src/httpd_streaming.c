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

#include "httpd_internal.h"
#include "httpd_streaming.h"
#include "logger.h"
#include "conffile.h"
#include "listener.h"

// Linked list of streaming requests
struct streaming_session {
  struct httpd_request *hreq;
  struct event *readev;

  struct streaming_session *next;
};

static pthread_mutex_t streaming_sessions_lock;


static int
streaming_mp3_handler(struct httpd_request *hreq)
{
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
}

static int
streaming_init(void)
{
  CHECK_ERR(L_STREAMING, mutex_init(&streaming_sessions_lock));

  return 0;
}

static void
streaming_deinit(void)
{
}

struct media_quality streaming_default_quality = {
  .sample_rate = 44100,
  .bits_per_sample = 16,
  .channels = 2,
  .bit_rate = 128000,
};

void
httpd_streaming_clientinfo_free(struct httpd_streaming_clients *clients)
{
  // TODO
}

struct httpd_streaming_clients *
httpd_streaming_clientinfo_get(void)
{
  struct httpd_streaming_clients *c1, *c2;

  // Replace with pointers into session
  c1 = malloc(sizeof(struct httpd_streaming_clients));
  c2 = malloc(sizeof(struct httpd_streaming_clients));
  c1->format = STREAMING_FORMAT_MP3;
  c2->format = STREAMING_FORMAT_MP3;
  c1->quality = streaming_default_quality;
  c2->quality = streaming_default_quality;
  c1->next = c2;
  c2->next = NULL;

  return c1;
}

static bool
session_is_valid(struct streaming_session *session)
{
  return true; // TODO
}

static void
read_cb(evutil_socket_t fd, short event, void *arg)
{
  return; // TODO
}

int
httpd_streaming_fd_set(struct httpd_streaming_clients *client, int fd)
{
  struct streaming_session *session = client->session;
  struct event_base *evbase;

  pthread_mutex_lock(&streaming_sessions_lock);
  if (!session_is_valid(session))
    {
      DPRINTF(E_WARN, L_STREAMING, "Callback for deleted session detected\n");
      goto error;
    }

  evbase = httpd_request_evbase_get(session->hreq);

  DPRINTF(E_DBG, L_STREAMING, "Will watch fd %d for streaming data\n", fd);
  CHECK_NULL(L_STREAMING, session->readev = event_new(evbase, fd, EV_TIMEOUT | EV_READ | EV_PERSIST, read_cb, session));
  pthread_mutex_lock(&streaming_sessions_lock);

  return 0;

 error:
  pthread_mutex_lock(&streaming_sessions_lock);
  return -1;
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
