#include "folio_online_guard.h"
#include <errno.h>
#include <sys/socket.h>
#include "esp_timer.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "lwip/sys.h"
#endif

#define GUARD_POLL_MS 50u

// Caller holds the lifecycle mutex. Retain the fd on transient errors such as
// ENOMEM so the next guard tick retries instead of silently losing protection.
static void interrupt_socket(folio_online_guard_t* guard) {
    if (shutdown(guard->socket_fd, SHUT_RDWR) == 0 || errno == ENOTCONN) guard->socket_fd = -1;
}
static void guard_task(void* context) {
    folio_online_guard_t* guard = context;
#ifdef ESP_PLATFORM
    // Reserve lwIP's per-thread semaphore BEFORE the worker allocates TLS.
    // This SDK's lazy netconn path dereferences a NULL syssem on allocation
    // failure; never discover that failure for the first time during shutdown.
    bool ready = sys_thread_sem_get() != NULL;
#else
    bool ready = true;
#endif
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    guard->ready = ready;
    xSemaphoreGive(guard->mutex);
    xSemaphoreGive(guard->done); // Startup ACK; no HTTP exists before this.
    if (!ready) { vTaskDelete(NULL); return; } // No guard access after failed ACK.
    for (;;) {
        // The predicate is a cancellation flag only, never application hooks.
        bool interrupt = (guard->cancelled && guard->cancelled(guard->context)) ||
            esp_timer_get_time() >= guard->deadline_us;
        xSemaphoreTake(guard->mutex, portMAX_DELAY);
        if (guard->stop) { xSemaphoreGive(guard->mutex); break; }
        if (interrupt) guard->interrupted = true;
        if (guard->interrupted && guard->socket_fd >= 0) {
            // Keep the mutex through shutdown. Worker detach/close cannot race
            // with fd reuse. No HTTP handle, TLS object or storage is touched.
            interrupt_socket(guard);
        }
        xSemaphoreGive(guard->mutex);
        vTaskDelay(pdMS_TO_TICKS(GUARD_POLL_MS));
    }
#ifdef ESP_PLATFORM
    sys_thread_sem_deinit();
#endif
    xSemaphoreGive(guard->done); // Exit ACK; no guard access after this.
    vTaskDelete(NULL);
}
bool folio_online_guard_begin(folio_online_guard_t* guard, int64_t deadline_us,
    bool (*cancelled)(void*), void* context) {
    if (!guard || guard->running) return false;
    if (!guard->mutex) guard->mutex = xSemaphoreCreateMutex();
    if (!guard->done) guard->done = xSemaphoreCreateBinary();
    if (!guard->mutex || !guard->done) return false;
    guard->stop = guard->interrupted = guard->ready = false;
    guard->socket_fd = -1;
    guard->deadline_us = deadline_us; guard->cancelled = cancelled; guard->context = context;
    guard->running = true;
    if (xTaskCreate(guard_task, "folio_http_guard", 3072, guard, 5, NULL) != pdPASS) {
        guard->running = false; return false;
    }
    xSemaphoreTake(guard->done, portMAX_DELAY); // Join startup reservation.
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    bool ready = guard->ready;
    xSemaphoreGive(guard->mutex);
    if (!ready) { guard->running = false; return false; }
    return true;
}
bool folio_online_guard_bind(folio_online_guard_t* guard, int socket_fd) {
    if (!guard || !guard->running || socket_fd < 0) return false;
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    guard->socket_fd = socket_fd;
    // A cancel/deadline may have happened during pre-socket DNS/handshake.
    if (guard->interrupted) {
        interrupt_socket(guard);
    }
    bool okay = !guard->interrupted;
    xSemaphoreGive(guard->mutex);
    return okay;
}
void folio_online_guard_detach(folio_online_guard_t* guard) {
    if (!guard || !guard->running) return;
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    guard->socket_fd = -1;
    xSemaphoreGive(guard->mutex);
}
bool folio_online_guard_interrupted(folio_online_guard_t* guard) {
    if (!guard || !guard->running) return false;
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    bool interrupted = guard->interrupted;
    xSemaphoreGive(guard->mutex);
    return interrupted;
}
void folio_online_guard_stop(folio_online_guard_t* guard) {
    if (!guard || !guard->running) return;
    xSemaphoreTake(guard->mutex, portMAX_DELAY);
    guard->socket_fd = -1; guard->stop = true;
    xSemaphoreGive(guard->mutex);
    xSemaphoreTake(guard->done, portMAX_DELAY);
    guard->running = false;
}
