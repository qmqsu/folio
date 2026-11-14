#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Internal socket-only deadline guard. The HTTP worker owns every TLS object.
// Static lifetime: semaphores remain allocated across request attempts.
typedef struct {
    SemaphoreHandle_t mutex, done;
    bool running, stop, interrupted, ready;
    int socket_fd;
    int64_t deadline_us;
    bool (*cancelled)(void*);
    void* context;
} folio_online_guard_t;

bool folio_online_guard_begin(folio_online_guard_t* guard, int64_t deadline_us,
    bool (*cancelled)(void*), void* context);
// Worker only: bind at ON_CONNECTED; detach at ON_DISCONNECTED AND before any
// explicit close/cleanup. Both serialize with shutdown to prevent fd reuse.
bool folio_online_guard_bind(folio_online_guard_t* guard, int socket_fd);
void folio_online_guard_detach(folio_online_guard_t* guard);
bool folio_online_guard_interrupted(folio_online_guard_t* guard);
// Detaches, joins the guard's last socket use, then lets worker close/free TLS.
void folio_online_guard_stop(folio_online_guard_t* guard);
