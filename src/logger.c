#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *log_stream;

static const char *process_state_name(process_state_t state)
{
    switch (state) {
    case PROCESS_STATE_NOT_STARTED:
        return "NOT_STARTED";
    case PROCESS_STATE_RUNNING:
        return "RUNNING";
    case PROCESS_STATE_EXITED_NORMALLY:
        return "EXITED_NORMALLY";
    case PROCESS_STATE_EXITED_UNEXPECTEDLY:
        return "EXITED_UNEXPECTEDLY";
    case PROCESS_STATE_RESTART_LIMIT_REACHED:
        return "RESTART_LIMIT_REACHED";
    default:
        return "UNKNOWN";
    }
}

static int make_timestamp(char *buffer, size_t capacity)
{
    time_t now;
    struct tm local_time;

    if (buffer == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    now = time(NULL);
    if (now == (time_t)-1) {
        return -1;
    }

    if (localtime_r(&now, &local_time) == NULL) {
        return -1;
    }

    if (strftime(buffer, capacity, "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    return 0;
}

/* Keep every event on one physical line and escape control characters. */
static int write_field(const char *value)
{
    const unsigned char *cursor;

    if (value == NULL) {
        value = "-";
    }

    for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        switch (*cursor) {
        case '\n':
            if (fputs("\\n", log_stream) == EOF) return -1;
            break;
        case '\r':
            if (fputs("\\r", log_stream) == EOF) return -1;
            break;
        case '\t':
            if (fputs("\\t", log_stream) == EOF) return -1;
            break;
        case '\\':
            if (fputs("\\\\", log_stream) == EOF) return -1;
            break;
        default:
            if (*cursor < 0x20u || *cursor == 0x7fu) {
                if (fprintf(log_stream, "\\x%02X", (unsigned int)*cursor) < 0)
                    return -1;
            } else if (fputc((int)*cursor, log_stream) == EOF) {
                return -1;
            }
            break;
        }
    }

    return 0;
}

static int write_record(const char *event,
                        const char *worker_name,
                        pid_t pid,
                        const char *details)
{
    char timestamp[32];

    if (log_stream == NULL) {
        errno = EBADF;
        return -1;
    }
    if (event == NULL || event[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (make_timestamp(timestamp, sizeof(timestamp)) == -1) {
        return -1;
    }

    if (fprintf(log_stream, "[%s] event=", timestamp) < 0 ||
        write_field(event) == -1 ||
        fputs(" worker=", log_stream) == EOF ||
        write_field(worker_name) == -1 ||
        fprintf(log_stream, " pid=%ld details=", (long)pid) < 0 ||
        write_field(details) == -1 ||
        fputc('\n', log_stream) == EOF ||
        fflush(log_stream) == EOF) {
        return -1;
    }

    return 0;
}

int logger_init(const char *path)
{
    FILE *candidate;
    int descriptor;
    int descriptor_flags;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (log_stream != NULL) {
        errno = EALREADY;
        return -1;
    }

    candidate = fopen(path, "a");
    if (candidate == NULL) {
        return -1;
    }

    /* The worker is exec'd after fork(); do not leak the log descriptor. */
    descriptor = fileno(candidate);
    if (descriptor == -1) {
        int saved_errno = errno;
        (void)fclose(candidate);
        errno = saved_errno;
        return -1;
    }

    descriptor_flags = fcntl(descriptor, F_GETFD);
    if (descriptor_flags == -1 ||
        fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1) {
        int saved_errno = errno;
        (void)fclose(candidate);
        errno = saved_errno;
        return -1;
    }

    log_stream = candidate;
    return 0;
}

int logger_event(const char *event,
                 const char *worker_name,
                 pid_t pid,
                 const char *details)
{
    return write_record(event, worker_name, pid, details);
}

int logger_status(const process_info_t *items, size_t count)
{
    size_t index;

    if (count > 0 && items == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (write_record("STATUS_REPORT_BEGIN", "watchdog", getpid(),
                     "current worker snapshot follows") == -1) {
        return -1;
    }

    for (index = 0; index < count; index++) {
        char details[192];
        int length;

        length = snprintf(details, sizeof(details),
                          "state=%s restart_count=%u last_exit_code=%d last_signal=%d",
                          process_state_name(items[index].state),
                          items[index].restart_count,
                          items[index].last_exit_code,
                          items[index].last_signal);
        if (length < 0 || (size_t)length >= sizeof(details)) {
            errno = EOVERFLOW;
            return -1;
        }

        if (write_record("STATUS_WORKER", items[index].name,
                         items[index].pid, details) == -1) {
            return -1;
        }
    }

    return write_record("STATUS_REPORT_END", "watchdog", getpid(),
                        "current worker snapshot completed");
}

void logger_close(void)
{
    FILE *stream;

    if (log_stream == NULL) {
        return;
    }

    stream = log_stream;
    log_stream = NULL;

    if (fflush(stream) == EOF) {
        int saved_errno = errno;
        (void)fprintf(stderr, "logger: fflush failed: %s\n", strerror(saved_errno));
    }

    if (fclose(stream) == EOF) {
        int saved_errno = errno;
        (void)fprintf(stderr, "logger: fclose failed: %s\n", strerror(saved_errno));
    }
}
