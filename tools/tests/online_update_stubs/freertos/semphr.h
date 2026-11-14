#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool binary;
    unsigned count;
} online_test_semaphore_t;
typedef online_test_semaphore_t* SemaphoreHandle_t;
static inline SemaphoreHandle_t online_test_semaphore(bool binary) {
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    assert(pthread_mutex_init(&s->mutex, NULL) == 0 && pthread_cond_init(&s->condition, NULL) == 0);
    s->binary = binary; return s;
}
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return online_test_semaphore(false); }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return online_test_semaphore(true); }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) {
    (void)timeout; assert(s && pthread_mutex_lock(&s->mutex) == 0);
    if (s->binary) {
        while (!s->count) assert(pthread_cond_wait(&s->condition, &s->mutex) == 0);
        --s->count; assert(pthread_mutex_unlock(&s->mutex) == 0);
    }
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    assert(s);
    if (s->binary) {
        assert(pthread_mutex_lock(&s->mutex) == 0); ++s->count;
        assert(pthread_cond_signal(&s->condition) == 0);
    }
    assert(pthread_mutex_unlock(&s->mutex) == 0); return 1;
}
