/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
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
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <uninorm.h>
#include <fcntl.h>

#include "outputs.h"
#include "misc.h"
#include "worker.h"
#include "transcode.h"
#include "logger.h"
#include "listener.h"
#include "httpd_streaming.h"

/* About
 *
 * This output takes the writes from the player thread, gives them to a worker
 * thread for mp3 encoding, and then the mp3 is written to a fd for the httpd
 * request handler to read and pass to clients. If there is no writing from the
 * player, but there are clients, it instead writes silence to the fd.
 */

// How many times per second we send silence when player is idle (to prevent
// client from hanging up). This value matches the player tick interval.
#define SILENCE_TICKS_PER_SEC 100

// #define DEBUG_WRITE_MP3 1

struct streaming_wanted
{
  bool keep;
  int fd[2]; // pipe where 0 is reading end, 1 is writing
  enum streaming_formats format;
  struct media_quality quality_in;
  struct media_quality quality_out;

  struct encode_ctx *xcode_ctx;
  struct evbuffer *encoded_data;

  struct streaming_wanted *next;
};

struct streaming_ctx
{
  struct streaming_wanted *wanted;
  struct event *clientev;
  struct event *silenceev;
  struct timeval silencetv;
  struct media_quality last_quality;
};

struct encode_cmdarg
{
  uint8_t *buf;
  size_t bufsize;
  int samples;
  struct media_quality quality;
};

static pthread_mutex_t streaming_wanted_lck;
static struct streaming_ctx streaming =
{
  .silencetv = { 0, (1000000 / SILENCE_TICKS_PER_SEC) },
};

extern struct event_base *evbase_player;


/* -----------------------------ICY Utilities ------------------------------- */


/* ------------------------------- Helpers ---------------------------------- */

#ifndef DEBUG_WRITE_MP3
static int
fd_open(int fd[2])
{
  int ret;
#ifdef HAVE_PIPE2
  ret = pipe2(fd, O_CLOEXEC | O_NONBLOCK);
#else
  if ( pipe(fd) < 0 ||
       fcntl(fd[0], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 ||
       fcntl(fd[1], F_SETFL, O_CLOEXEC | O_NONBLOCK) < 0 )
    ret = -1;
  else
    ret = 0;
#endif
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_STREAMING, "Could not create pipe: %s\n", strerror(errno));
      fd[0] = -1;
      fd[1] = -1;
      return -1;
    }

  return 0;
}
#else
static int
fd_open(int fd[2])
{
  fd[1] = open("testfile.mp3", O_CREAT | O_RDWR, 0664);
  if (fd[1] < 0)
    {
      DPRINTF(E_DBG, L_STREAMING, "Error opening file: %s\n", strerror(errno));
      return -1;
    }
  fd[0] = -1;
  return 0;
}
#endif

static void
fd_close(int fd[2])
{
  if (fd[0] >= 0)
    close(fd[0]);
  if (fd[1] >= 0)
    close(fd[1]);
}

static void
wanted_free(struct streaming_wanted *w)
{
  if (!w)
    return;
  fd_close(w->fd);
  free(w);
}

static void
wanted_remove(struct streaming_wanted **wanted, struct streaming_wanted *remove)
{
  struct streaming_wanted *prev = NULL;
  struct streaming_wanted *w;

  for (w = *wanted; w; w = w->next)
    {
      if (w == remove)
	break;

      prev = w;
    }

  if (!w)
    return;

  if (!prev)
    *wanted = remove->next;
  else
    prev->next = remove->next;

  wanted_free(remove);
}

static struct streaming_wanted *
wanted_new(enum streaming_formats format, struct media_quality quality)
{
  struct streaming_wanted *w;

  CHECK_NULL(L_STREAMING, w = calloc(1, sizeof(struct streaming_wanted)));
  CHECK_NULL(L_STREAMING, w->encoded_data = evbuffer_new());

  fd_open(w->fd);
  w->quality_out = quality;
  w->format = format;
  return w;
}

static void
wanted_set(struct streaming_wanted **wanted, struct httpd_streaming_clients *clients)
{
  struct httpd_streaming_clients *c;
  struct streaming_wanted *w;
  struct streaming_wanted *next;

  for (c = clients; c; c = c->next)
    {
      for (w = *wanted; w; w = w->next)
	{
	  if (c->format == w->format && quality_is_equal(&c->quality, &w->quality_out))
	    break;
	}

      if (!w)
	{
	  w = wanted_new(c->format, c->quality);
	  w->next = *wanted;
	  *wanted = w;
	}

      w->keep = true;
      httpd_streaming_fd_set(c, w->fd[0]);
    }

  for (w = *wanted; w; w = next)
    {
      next = w->next;
      if (!w->keep)
	wanted_remove(wanted, w);
      else
	w->keep = false;
    }
}


/* ----------------------------- Thread: Worker ----------------------------- */

/*
static void
icy_handle(struct streaming_wanted *w)
{
  len = evbuffer_get_length(w->encoded_data);

  count = w->bytes_since_metablock + len;
  if (count <= streaming_icy_metaint)
    {
      w->bytes_since_metablock += len;
      return;
    }

  overflow = count % streaming_icy_metaint; // TODO can overflow be larger than len??

  evbuffer_remove_buffer(w->encoded_audio, evbuf, len - overflow);
  evbuffer_add(evbuf, meta, metalen);
  evbuffer_add_buffer(w->encoded_audio, evbuf);
  swap_pointers(&evbuf, &w->encoded_audio);
  free(evbuf);

  // Splice the 'icy title' in with the encoded audio data
  splice_buf = streaming_icy_meta_splice(buf, len, len - overflow, &splice_len);
  if (!splice_buf)
    goto out;

  evbuffer_add(evbuf, splice_buf, splice_len);

  free(splice_buf);

  session->bytes_since_metablock = overflow;
}
*/

static int
encode_reset(struct streaming_wanted *w, struct media_quality quality_in)
{
  struct media_quality quality_out = w->quality_out;
  struct decode_ctx *decode_ctx = NULL;

  transcode_encode_cleanup(&w->xcode_ctx);

  if (quality_in.bits_per_sample == 16)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM16, &quality_in);
  else if (quality_in.bits_per_sample == 24)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM24, &quality_in);
  else if (quality_in.bits_per_sample == 32)
    decode_ctx = transcode_decode_setup_raw(XCODE_PCM32, &quality_in);

  if (!decode_ctx)
    {
      DPRINTF(E_LOG, L_STREAMING, "Error setting up decoder for input quality sr %d, bps %d, ch %d, cannot MP3 encode\n",
	quality_in.sample_rate, quality_in.bits_per_sample, quality_in.channels);
      return -1;
    }

  w->quality_in = quality_in;
  w->xcode_ctx = transcode_encode_setup(XCODE_MP3, &quality_out, decode_ctx, NULL, 0, 0);
  if (!w->xcode_ctx)
    {
      DPRINTF(E_LOG, L_STREAMING, "Error setting up encoder for output quality sr %d, bps %d, ch %d, cannot MP3 encode\n",
	quality_out.sample_rate, quality_out.bits_per_sample, quality_out.channels);
      return -1;
    }

  return 0;
}

static int
encode_and_write(struct streaming_wanted *w, struct media_quality quality_in, transcode_frame *frame)
{
  int ret;
  size_t len;

  if (!w->xcode_ctx || !quality_is_equal(&quality_in, &w->quality_in))
    {
      DPRINTF(E_DBG, L_STREAMING, "Resetting transcode context\n");
      if (encode_reset(w, quality_in) < 0)
	return -1;
    }

  ret = transcode_encode(w->encoded_data, w->xcode_ctx, frame, 0);
  if (ret < 0)
    {
      return -1;
    }

  len = evbuffer_get_length(w->encoded_data);
  if (len == 0)
    {
      return 0;
    }

  ret = evbuffer_write(w->encoded_data, w->fd[1]);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_STREAMING, "Error writing to stream pipe %d (format %d)\n", w->fd[1], w->format);
      return -1;
    }

  return 0;
}

static void
encode_data_cb(void *arg)
{
  struct encode_cmdarg *ctx = arg;
  transcode_frame *frame;
  struct streaming_wanted *w;
  struct streaming_wanted *next;
  int ret;

  frame = transcode_frame_new(ctx->buf, ctx->bufsize, ctx->samples, &ctx->quality);
  if (!frame)
    {
      DPRINTF(E_LOG, L_STREAMING, "Could not convert raw PCM to frame\n");
      goto out;
    }

  pthread_mutex_lock(&streaming_wanted_lck);
  for (w = streaming.wanted; w; w = next)
    {
      next = w->next;
      ret = encode_and_write(w, ctx->quality, frame);
      if (ret < 0)
	wanted_remove(&streaming.wanted, w); // This will close the fd, so httpd reader will get an error
    }
  pthread_mutex_unlock(&streaming_wanted_lck);

 out:
  transcode_frame_free(frame);
  free(ctx->buf);
}


/* ----------------------------- Thread: Player ----------------------------- */

static void
encode_worker_invoke(uint8_t *buf, size_t bufsize, int samples, struct media_quality quality)
{
  struct encode_cmdarg ctx;

  if (quality.channels == 0)
    {
      DPRINTF(E_LOG, L_STREAMING, "Streaming quality is zero (%d/%d/%d)\n",
	quality.sample_rate, quality.bits_per_sample, quality.channels);
      return;
    }

  ctx.buf = buf;
  ctx.bufsize = bufsize;
  ctx.samples = samples;
  ctx.quality = quality;

  worker_execute(encode_data_cb, &ctx, sizeof(struct encode_cmdarg), 0);
}

static void
streaming_write(struct output_buffer *obuf)
{
  uint8_t *rawbuf;

  if (!streaming.wanted)
    return;

  // Need to make a copy since it will be passed of to the async worker
  rawbuf = malloc(obuf->data[0].bufsize);
  memcpy(rawbuf, obuf->data[0].buffer, obuf->data[0].bufsize);

  // In case this is the last player write() we want to start streaming silence
  evtimer_add(streaming.silenceev, &streaming.silencetv);

  encode_worker_invoke(rawbuf, obuf->data[0].bufsize, obuf->data[0].samples, obuf->data[0].quality);

  streaming.last_quality = obuf->data[0].quality;
}

static void
silenceev_cb(evutil_socket_t fd, short event, void *arg)
{
  uint8_t *rawbuf;
  size_t bufsize;
  int samples;

  samples = streaming.last_quality.sample_rate / SILENCE_TICKS_PER_SEC;
  bufsize = STOB(samples, streaming.last_quality.bits_per_sample, streaming.last_quality.channels);

  rawbuf = calloc(1, bufsize);

  evtimer_add(streaming.silenceev, &streaming.silencetv);

  encode_worker_invoke(rawbuf, bufsize, samples, streaming.last_quality);
}

static void
clientev_cb(evutil_socket_t fd, short event, void *arg)
{
  struct httpd_streaming_clients *clients;

  clients = httpd_streaming_clientinfo_get();

  // We must produce an output for each unique wanted format + quality
  pthread_mutex_lock(&streaming_wanted_lck);
  wanted_set(&streaming.wanted, clients);
  pthread_mutex_unlock(&streaming_wanted_lck);

  httpd_streaming_clientinfo_free(clients);

  if (!streaming.wanted)
    {
      evtimer_del(streaming.silenceev);
      return;
    }
}


/* ----------------------------- Thread: httpd ------------------------------ */

static void
streaming_listener_cb(short event_mask)
{
  event_active(streaming.clientev, 0, 0);
}

static int
streaming_init(void)
{
  CHECK_NULL(L_STREAMING, streaming.clientev = event_new(evbase_player, -1, 0, clientev_cb, NULL));
  CHECK_NULL(L_STREAMING, streaming.silenceev = event_new(evbase_player, -1, 0, silenceev_cb, NULL));
  CHECK_ERR(L_STREAMING,  mutex_init(&streaming_wanted_lck));

  listener_add(streaming_listener_cb, LISTENER_STREAMING);
  return 0;
}

static void
streaming_deinit(void)
{
  event_free(streaming.clientev);
  event_free(streaming.silenceev);
}

struct output_definition output_streaming =
{
  .name = "mp3 streaming",
  .type = OUTPUT_TYPE_STREAMING,
  .priority = 0,
  .disabled = 0,
  .init = streaming_init,
  .deinit = streaming_deinit,
  .write = streaming_write,
};
