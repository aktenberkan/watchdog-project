#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "process_manager.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKER_EXECUTABLE "./worker"
#define SHUTDOWN_POLL_NANOSECONDS 50000000L

static process_info_t processes[PROCESS_MANAGER_MAX_CHILDREN];
static int manager_initialized;
static int shutting_down;

static void log_event(const char *event,
                      const process_info_t *process,
                      pid_t pid,
                      const char *details)
{
    const char *name = process != NULL ? process->name : "-";
    (void)logger_event(event, name, pid, details);
}

static int spawn_worker(size_t index, int is_restart)
{
    process_info_t *process = &processes[index];
    const char *test_duration = getenv("WATCHDOG_WORKER_DURATION");
    pid_t child_pid;
    char details[160];

    if (shutting_down) {
        errno = ECANCELED;
        return -1;
    }

    if (process->pid > 0) {
        errno = EBUSY;
        return -1;
    }

    /* Count restart attempts before fork so a failed fork is still bounded. */
    if (is_restart) {
        process->restart_count++;
    }

    process->state = PROCESS_STATE_NOT_STARTED;
    process->last_exit_code = -1;
    process->last_signal = 0;

    child_pid = fork();
    if (child_pid == -1) {
        int saved_errno = errno;
        process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
        (void)snprintf(details, sizeof(details),
                       "fork failed: %s; restart_count=%u",
                       strerror(saved_errno), process->restart_count);
        log_event("process_start_failed", process, 0, details);

        if (is_restart && process->restart_count >= PROCESS_MANAGER_MAX_RESTARTS) {
            process->state = PROCESS_STATE_RESTART_LIMIT_REACHED;
            log_event("restart_limit_reached", process, 0,
                      "restart limit reached after fork failure");
        }
        errno = saved_errno;
        return -1;
    }

    if (child_pid == 0) {
        (void)fflush(NULL);

        if (test_duration != NULL && test_duration[0] != '\0') {
            execl(WORKER_EXECUTABLE,
                  "worker",
                  "--name", process->name,
                  "--duration", test_duration,
                  (char *)NULL);
        } else {
            execl(WORKER_EXECUTABLE,
                  "worker",
                  "--name", process->name,
                  (char *)NULL);
        }

        {
            static const char exec_error_message[] =
                "watchdog: exec ./worker failed\n";
            (void)write(STDERR_FILENO, exec_error_message,
                        sizeof(exec_error_message) - 1);
        }
        _exit(127);
    }

    process->pid = child_pid;
    process->state = PROCESS_STATE_RUNNING;

    (void)snprintf(details, sizeof(details), "%s; restart_count=%u",
                   is_restart ? "automatic restart" : "initial start",
                   process->restart_count);
    log_event("process_created", process, child_pid, details);

    if (is_restart) {
        log_event("process_restarted", process, child_pid, details);
    }

    return 0;
}

static void handle_child_exit(size_t index, int status)
{
    process_info_t *process = &processes[index];
    const pid_t exited_pid = process->pid;
    char details[160];
    int unexpected = 1;

    process->pid = 0;

    if (WIFEXITED(status)) {
        process->last_exit_code = WEXITSTATUS(status);
        process->last_signal = 0;

        if (WEXITSTATUS(status) == 0) {
            process->state = PROCESS_STATE_EXITED_NORMALLY;
            (void)snprintf(details, sizeof(details), "exit_code=0");
            log_event("process_exited_normally", process, exited_pid, details);
            unexpected = 0;
        } else {
            process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
            (void)snprintf(details, sizeof(details), "non-zero exit_code=%d",
                           WEXITSTATUS(status));
            log_event("process_exited_unexpectedly", process, exited_pid, details);
        }
    } else if (WIFSIGNALED(status)) {
        process->last_exit_code = -1;
        process->last_signal = WTERMSIG(status);
        process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
        (void)snprintf(details, sizeof(details), "terminating_signal=%d",
                       WTERMSIG(status));
        log_event("process_exited_unexpectedly", process, exited_pid, details);
    } else {
        process->last_exit_code = -1;
        process->last_signal = 0;
        process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
        (void)snprintf(details, sizeof(details), "unclassified wait status=%d", status);
        log_event("process_exit_status_unknown", process, exited_pid, details);
    }

    if (!unexpected || shutting_down) {
        return;
    }

    if (process->restart_count < PROCESS_MANAGER_MAX_RESTARTS) {
        (void)spawn_worker(index, 1);
    } else {
        process->state = PROCESS_STATE_RESTART_LIMIT_REACHED;
        log_event("restart_limit_reached", process, exited_pid,
                  "no automatic restart attempts remain");
    }
}

static void reap_one_process(size_t index)
{
    process_info_t *process = &processes[index];
    int status;
    pid_t result;

    if (process->pid <= 0) {
        return;
    }

    do {
        result = waitpid(process->pid, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);

    if (result == process->pid) {
        handle_child_exit(index, status);
    } else if (result == -1 && errno == ECHILD) {
        const pid_t lost_pid = process->pid;
        process->pid = 0;
        process->last_exit_code = -1;
        process->last_signal = 0;
        process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
        log_event("process_reaping_error", process, lost_pid,
                  "waitpid reported ECHILD; child status is unavailable");

        if (!shutting_down) {
            if (process->restart_count < PROCESS_MANAGER_MAX_RESTARTS) {
                (void)spawn_worker(index, 1);
            } else {
                process->state = PROCESS_STATE_RESTART_LIMIT_REACHED;
                log_event("restart_limit_reached", process, lost_pid,
                          "restart limit reached after ECHILD");
            }
        }
    } else if (result == -1) {
        char details[128];
        int saved_errno = errno;
        (void)snprintf(details, sizeof(details), "waitpid failed: %s",
                       strerror(saved_errno));
        log_event("waitpid_error", process, process->pid, details);
    }
}

int process_manager_init(void)
{
    size_t index;

    memset(processes, 0, sizeof(processes));
    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        (void)snprintf(processes[index].name, sizeof(processes[index].name),
                       "worker-%zu", index + 1);
        processes[index].state = PROCESS_STATE_NOT_STARTED;
        processes[index].last_exit_code = -1;
    }

    shutting_down = 0;
    manager_initialized = 1;
    return 0;
}

int process_manager_start_all(void)
{
    size_t index;
    int failures = 0;

    if (!manager_initialized) {
        errno = EINVAL;
        return -1;
    }
    if (shutting_down) {
        errno = ECANCELED;
        return -1;
    }

    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        if (processes[index].pid == 0 &&
            processes[index].state == PROCESS_STATE_NOT_STARTED &&
            spawn_worker(index, 0) == -1) {
            failures++;
        }
    }

    return failures == 0 ? 0 : -1;
}

void process_manager_reap_terminated(void)
{
    size_t index;

    if (!manager_initialized) {
        return;
    }

    /* Check each PID owned by this module; do not reap unrelated children. */
    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        reap_one_process(index);
    }
}

void process_manager_periodic_check(void)
{
    process_manager_reap_terminated();
}

size_t process_manager_get_status(process_info_t *out, size_t capacity)
{
    size_t count;

    if (!manager_initialized || out == NULL || capacity == 0) {
        return 0;
    }

    count = capacity < PROCESS_MANAGER_MAX_CHILDREN
                ? capacity
                : PROCESS_MANAGER_MAX_CHILDREN;
    memcpy(out, processes, count * sizeof(process_info_t));
    return count;
}

int process_manager_any_running(void)
{
    size_t index;

    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        if (processes[index].pid > 0 &&
            processes[index].state == PROCESS_STATE_RUNNING) {
            return 1;
        }
    }

    return 0;
}

int process_manager_start_periodic_timer(unsigned int interval_seconds)
{
    struct itimerval timer;

    if (interval_seconds == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_sec = (time_t)interval_seconds;
    timer.it_interval.tv_sec = (time_t)interval_seconds;

    return setitimer(ITIMER_REAL, &timer, NULL);
}

void process_manager_stop_periodic_timer(void)
{
    struct itimerval timer;

    memset(&timer, 0, sizeof(timer));
    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        log_event("timer_stop_failed", NULL, 0, strerror(errno));
    }
}

static int timeout_elapsed(const struct timespec *start,
                           const struct timespec *now,
                           unsigned int timeout_seconds)
{
    time_t seconds = now->tv_sec - start->tv_sec;
    long nanoseconds = now->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
    }

    return seconds >= (time_t)timeout_seconds;
}

void process_manager_shutdown(unsigned int timeout_seconds)
{
    size_t index;
    struct timespec start_time;
    struct timespec current_time;
    time_t wall_start = time(NULL);
    int monotonic_clock_available;
    const struct timespec poll_delay = {0, SHUTDOWN_POLL_NANOSECONDS};

    if (!manager_initialized || shutting_down) {
        return;
    }

    shutting_down = 1;
    process_manager_stop_periodic_timer();
    log_event("process_manager_shutdown_started", NULL, 0,
              "sending SIGTERM to active workers");

    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        process_info_t *process = &processes[index];

        if (process->pid > 0 && kill(process->pid, SIGTERM) == -1 && errno != ESRCH) {
            char details[128];
            int saved_errno = errno;
            (void)snprintf(details, sizeof(details), "SIGTERM failed: %s",
                           strerror(saved_errno));
            log_event("worker_sigterm_failed", process, process->pid, details);
        }
    }

    monotonic_clock_available = (clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);

    while (process_manager_any_running()) {
        process_manager_reap_terminated();
        if (!process_manager_any_running()) {
            break;
        }

        if (monotonic_clock_available) {
            if (clock_gettime(CLOCK_MONOTONIC, &current_time) == 0) {
                if (timeout_elapsed(&start_time, &current_time, timeout_seconds)) {
                    break;
                }
            } else {
                monotonic_clock_available = 0;
                wall_start = time(NULL);
            }
        } else if ((unsigned int)(time(NULL) - wall_start) >= timeout_seconds) {
            break;
        }

        (void)nanosleep(&poll_delay, NULL);
    }

    /* Escalate only processes that are still recorded as running. */
    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        process_info_t *process = &processes[index];

        if (process->pid > 0) {
            log_event("worker_sigkill_escalation", process, process->pid,
                      "worker did not stop before shutdown timeout");
            if (kill(process->pid, SIGKILL) == -1 && errno != ESRCH) {
                char details[128];
                int saved_errno = errno;
                (void)snprintf(details, sizeof(details), "SIGKILL failed: %s",
                               strerror(saved_errno));
                log_event("worker_sigkill_failed", process, process->pid, details);
            }
        }
    }

    /* Reap every remaining child; shutting_down prevents any restart. */
    for (index = 0; index < PROCESS_MANAGER_MAX_CHILDREN; index++) {
        process_info_t *process = &processes[index];
        int status;
        pid_t result;

        if (process->pid <= 0) {
            continue;
        }

        do {
            result = waitpid(process->pid, &status, 0);
        } while (result == -1 && errno == EINTR);

        if (result == process->pid) {
            handle_child_exit(index, status);
        } else if (result == -1 && errno == ECHILD) {
            const pid_t lost_pid = process->pid;
            process->pid = 0;
            process->state = PROCESS_STATE_EXITED_UNEXPECTEDLY;
            log_event("process_reaping_error", process, lost_pid,
                      "ECHILD during shutdown; child status is unavailable");
        } else if (result == -1) {
            char details[128];
            int saved_errno = errno;
            (void)snprintf(details, sizeof(details), "blocking waitpid failed: %s",
                           strerror(saved_errno));
            log_event("waitpid_error", process, process->pid, details);
        }
    }

    log_event("process_manager_shutdown_completed", NULL, 0,
              "all managed worker processes have been handled");
}
