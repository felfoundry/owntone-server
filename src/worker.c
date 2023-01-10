/*
 * Copyright (C) 2014 Espen Jürgensen <espenjurgensen@gmail.com>
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
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <event2/event.h>

#include "db.h"
#include "logger.h"
#include "worker.h"
#include "misc.h"

#include "worker_evthr.h"

#define THREADPOOL_NTHREADS 4

struct worker_arg
{
  void (*cb)(void *);
  void *cb_arg;
  int delay;
  struct event *timer;
};


/* --- Globals --- */

static evthr_pool_t *worker_threadpool;


/* ---------------------------- CALLBACK EXECUTION ------------------------- */
/*                                Thread: worker                             */

static void
execute_cb(int fd, short what, void *arg)
{
  struct worker_arg *cmdarg = arg;

  cmdarg->cb(cmdarg->cb_arg);

  event_free(cmdarg->timer);
  free(cmdarg->cb_arg);
  free(cmdarg);
}

static void
execute(evthr_t *thr, void *arg, void *shared)
{
  struct worker_arg *cmdarg = arg;
  struct timeval tv = { cmdarg->delay, 0 };
  struct event_base *evbase;

  if (cmdarg->delay)
    {
      evbase = evthr_get_base(thr);
      cmdarg->timer = evtimer_new(evbase, execute_cb, cmdarg);
      evtimer_add(cmdarg->timer, &tv);
      return;
    }

  cmdarg->cb(cmdarg->cb_arg);
  free(cmdarg->cb_arg);
  free(cmdarg);
}


/* ---------------------------- Our worker API  --------------------------- */

void
worker_execute(void (*cb)(void *), void *cb_arg, size_t arg_size, int delay)
{
  struct worker_arg *cmdarg;
  void *argcpy;

  cmdarg = calloc(1, sizeof(struct worker_arg));
  if (!cmdarg)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not allocate worker_arg\n");
      return;
    }

  if (arg_size > 0)
    {
      argcpy = malloc(arg_size);
      if (!argcpy)
	{
	  DPRINTF(E_LOG, L_MAIN, "Out of memory\n");
	  free(cmdarg);
	  return;
	}

      memcpy(argcpy, cb_arg, arg_size);
    }
  else
    argcpy = NULL;

  cmdarg->cb = cb;
  cmdarg->cb_arg = argcpy;
  cmdarg->delay = delay;

  evthr_pool_defer(worker_threadpool, execute, cmdarg);
}

static void
init_cb(evthr_t *thr, void *shared)
{
  int ret;

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Error: DB init failed (worker thread)\n");
      // TODO error!
    }

  thread_setname(pthread_self(), "worker");
}

static void
exit_cb(evthr_t *thr, void *shared)
{
  db_perthread_deinit();
}

int
worker_init(void)
{
  int ret;

  worker_threadpool = evthr_pool_wexit_new(THREADPOOL_NTHREADS, init_cb, exit_cb, NULL);
  if (!worker_threadpool)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not create worker thread pool: %s\n", strerror(errno));
      goto error;
    }

  ret = evthr_pool_start(worker_threadpool);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not spawn worker threads: %s\n", strerror(errno));
      goto error;
    }

  return 0;
  
 error:
  worker_deinit();
  return -1;
}

void
worker_deinit(void)
{
  evthr_pool_stop(worker_threadpool);
  evthr_pool_free(worker_threadpool);
}
