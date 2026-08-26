/* Atlas - A13: when each repository's scanner was last heard from.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See the contract in `daemon/daemon_internal.h`. The short version: a
 * heartbeat is liveness rather than a durable fact, so it lives here and not in
 * the index — a daemon that has just started has heard from nobody, which is
 * the conservative answer, and a persisted heartbeat would let it trust a
 * scanner that died before it.
 */
#include "daemon/daemon_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Small and fixed. A daemon serves a handful of repositories; when the table is
 * full the oldest entry is reused, and an evicted repository reads as "never
 * heard from" — which only ever subtracts, so the failure direction is the safe
 * one. */
#define SEEN_MAX 64u

typedef struct {
    int64_t repo_id; /* 0 when the slot is unused */
    int64_t at_ms;
} seen_slot;

struct atlas_scanner_seen {
    pthread_mutex_t lock;
    seen_slot slots[SEEN_MAX];
};

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

atlas_scanner_seen *atlas_scanner_seen_new(void) {
    atlas_scanner_seen *s = calloc(1u, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

void atlas_scanner_seen_free(atlas_scanner_seen *s) {
    if (s == NULL) {
        return;
    }
    (void)pthread_mutex_destroy(&s->lock);
    free(s);
}

void atlas_scanner_seen_touch(atlas_scanner_seen *s, int64_t repo_id) {
    if (s == NULL || repo_id <= 0) {
        return;
    }
    int64_t t = now_ms();
    (void)pthread_mutex_lock(&s->lock);
    size_t oldest = 0;
    for (size_t i = 0; i < SEEN_MAX; i++) {
        if (s->slots[i].repo_id == repo_id) {
            s->slots[i].at_ms = t;
            (void)pthread_mutex_unlock(&s->lock);
            return;
        }
        if (s->slots[i].repo_id == 0) {
            s->slots[i].repo_id = repo_id;
            s->slots[i].at_ms = t;
            (void)pthread_mutex_unlock(&s->lock);
            return;
        }
        if (s->slots[i].at_ms < s->slots[oldest].at_ms) {
            oldest = i;
        }
    }
    s->slots[oldest].repo_id = repo_id;
    s->slots[oldest].at_ms = t;
    (void)pthread_mutex_unlock(&s->lock);
}

int64_t atlas_scanner_seen_age_ms(atlas_scanner_seen *s, int64_t repo_id) {
    if (s == NULL || repo_id <= 0) {
        return -1;
    }
    int64_t age = -1;
    int64_t t = now_ms();
    (void)pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < SEEN_MAX; i++) {
        if (s->slots[i].repo_id == repo_id) {
            age = t - s->slots[i].at_ms;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s->lock);
    return age;
}
