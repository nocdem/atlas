/* Atlas - bounded worker pool.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#define _GNU_SOURCE 1

#include "atlas/workers.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/limits.h"

struct atlas_workers {
    pthread_t *threads;
    size_t count;

    pthread_mutex_t lock;
    pthread_cond_t work_ready; /* a batch was posted, or shutdown was requested */
    pthread_cond_t batch_done; /* the last job of a batch finished */

    /* --- all fields below are guarded by `lock` --- */
    bool stopping;
    atlas_worker_fn fn;
    void *ud;
    size_t next;      /* next index to claim */
    size_t total;     /* jobs in the current batch */
    size_t remaining; /* jobs neither claimed-and-finished nor outstanding */
};

static void *worker_main(void *arg) {
    atlas_workers *w = (atlas_workers *)arg;
    (void)pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->stopping && w->next >= w->total) {
            (void)pthread_cond_wait(&w->work_ready, &w->lock);
        }
        if (w->stopping && w->next >= w->total) {
            break;
        }
        size_t i = w->next++;
        atlas_worker_fn fn = w->fn;
        void *ud = w->ud;
        (void)pthread_mutex_unlock(&w->lock);

        /* The job runs with no lock held: it is the expensive part, and holding
         * the pool lock across it would serialise the very thing the pool exists
         * to parallelise. The job writes only to its own slot. */
        if (fn != NULL) {
            fn(i, ud);
        }

        (void)pthread_mutex_lock(&w->lock);
        if (--w->remaining == 0) {
            (void)pthread_cond_broadcast(&w->batch_done);
        }
    }
    (void)pthread_mutex_unlock(&w->lock);
    return NULL;
}

atlas_status atlas_workers_start(size_t count, atlas_workers **out, atlas_err *err) {
    *out = NULL;
    if (count == 0) {
        count = ATLAS_WORKER_COUNT_DEFAULT;
    }
    /* Clamped against the machine as well as against the constant: more threads
     * than cores turns parallel hashing into scheduler churn, and the point of
     * the bound is that resource use is predictable. */
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0 && (size_t)online < count) {
        count = (size_t)online;
    }
    if (count > ATLAS_WORKER_COUNT_MAX) {
        count = ATLAS_WORKER_COUNT_MAX;
    }
    if (count < 1u) {
        count = 1u;
    }

    atlas_workers *w = calloc(1u, sizeof(*w));
    if (w == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the worker pool");
    }
    w->threads = calloc(count, sizeof(*w->threads));
    if (w->threads == NULL) {
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the worker pool");
    }
    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        free(w->threads);
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the worker pool mutex");
    }
    if (pthread_cond_init(&w->work_ready, NULL) != 0 ||
        pthread_cond_init(&w->batch_done, NULL) != 0) {
        (void)pthread_mutex_destroy(&w->lock);
        free(w->threads);
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the worker pool conditions");
    }

    for (size_t i = 0; i < count; i++) {
        if (pthread_create(&w->threads[i], NULL, worker_main, w) != 0) {
            /* A partially started pool is still a usable pool as long as at
             * least one thread came up; fewer threads means slower, not wrong.
             * Zero threads is a real failure. */
            w->count = i;
            if (i == 0) {
                atlas_workers_stop(w);
                return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create a worker thread");
            }
            *out = w;
            return ATLAS_OK;
        }
        w->count = i + 1u;
    }
    *out = w;
    return ATLAS_OK;
}

void atlas_workers_stop(atlas_workers *w) {
    if (w == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&w->lock);
    w->stopping = true;
    (void)pthread_cond_broadcast(&w->work_ready);
    (void)pthread_mutex_unlock(&w->lock);

    for (size_t i = 0; i < w->count; i++) {
        (void)pthread_join(w->threads[i], NULL);
    }
    (void)pthread_cond_destroy(&w->work_ready);
    (void)pthread_cond_destroy(&w->batch_done);
    (void)pthread_mutex_destroy(&w->lock);
    free(w->threads);
    free(w);
}

size_t atlas_workers_count(const atlas_workers *w) {
    return w != NULL ? w->count : 0u;
}

atlas_status atlas_workers_for_each(atlas_workers *w, size_t n, atlas_worker_fn fn, void *ud,
                                    atlas_err *err) {
    if (n == 0) {
        return ATLAS_OK;
    }
    if (fn == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no worker function given");
    }
    /* Serial fallback. A one-shot CLI invocation has no pool, and the tests use
     * this path so that job ordering is deterministic. */
    if (w == NULL || w->count == 0) {
        for (size_t i = 0; i < n; i++) {
            fn(i, ud);
        }
        return ATLAS_OK;
    }

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping) {
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the worker pool is shutting down");
    }
    /* One batch at a time. The pool is owned by the single serial stage that
     * drives a reconciliation pass, so a second concurrent batch would mean two
     * passes were running, which the daemon does not allow. Asserting it here
     * turns that design rule into a check. */
    if (w->total != w->next) {
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a worker batch is already in flight; batches are not reentrant");
    }
    w->fn = fn;
    w->ud = ud;
    w->next = 0;
    w->total = n;
    w->remaining = n;
    (void)pthread_cond_broadcast(&w->work_ready);

    while (w->remaining > 0) {
        (void)pthread_cond_wait(&w->batch_done, &w->lock);
    }
    /* Leave the batch counters equal so the reentrancy check above holds, and
     * drop the borrowed pointers so a stopped pool cannot dereference them. */
    w->fn = NULL;
    w->ud = NULL;
    w->next = 0;
    w->total = 0;
    (void)pthread_mutex_unlock(&w->lock);
    return ATLAS_OK;
}
