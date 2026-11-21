#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <stddef.h>
#include <sys/types.h>

#define PROCESS_MANAGER_MAX_CHILDREN 2
#define PROCESS_MANAGER_MAX_RESTARTS 3
#define PROCESS_MANAGER_DEFAULT_SHUTDOWN_TIMEOUT 5

typedef enum {
    PROCESS_STATE_NOT_STARTED = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_EXITED_NORMALLY,
    PROCESS_STATE_EXITED_UNEXPECTEDLY,
    PROCESS_STATE_RESTART_LIMIT_REACHED
} process_state_t;

typedef struct {
    char name[32];
    pid_t pid;
    unsigned int restart_count;
    process_state_t state;
    int last_exit_code;
    int last_signal;
} process_info_t;

int process_manager_init(void);
int process_manager_start_all(void);
void process_manager_reap_terminated(void);
void process_manager_periodic_check(void);
size_t process_manager_get_status(process_info_t *out, size_t capacity);
int process_manager_any_running(void);
int process_manager_start_periodic_timer(unsigned int interval_seconds);
void process_manager_stop_periodic_timer(void);
void process_manager_shutdown(unsigned int timeout_seconds);

#endif
