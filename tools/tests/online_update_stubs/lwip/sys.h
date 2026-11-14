#pragma once
// Allocation seam for compiling the production ESP_PLATFORM guard branch.
// Networking in the guard regression still uses real localhost POSIX sockets.
typedef int sys_sem_t;
sys_sem_t* sys_thread_sem_get(void);
void sys_thread_sem_deinit(void);
