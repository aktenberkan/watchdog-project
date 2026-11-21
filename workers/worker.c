#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    if (sigemptyset(&action.sa_mask) == -1) {
        return -1;
    }

    if (sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1) {
        return -1;
    }

    return 0;
}

static int parse_duration(const char *text, unsigned int *duration)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || duration == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX) {
        return -1;
    }

    *duration = (unsigned int)parsed;
    return 0;
}

static int sleep_one_second(void)
{
    struct timespec remaining = {1, 0};

    while (!stop_requested && nanosleep(&remaining, &remaining) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *worker_name = "worker";
    unsigned int duration_seconds = 0;
    unsigned int elapsed_seconds = 0;
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--name") == 0) {
            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                fprintf(stderr, "worker: --name requires a value\n");
                return 2;
            }
            worker_name = argv[++index];
        } else if (strcmp(argv[index], "--duration") == 0) {
            if (index + 1 >= argc ||
                parse_duration(argv[index + 1], &duration_seconds) == -1) {
                fprintf(stderr, "worker: --duration requires a non-negative integer\n");
                return 2;
            }
            index++;
        } else {
            fprintf(stderr, "worker: unknown argument: %s\n", argv[index]);
            return 2;
        }
    }

    if (install_signal_handlers() == -1) {
        perror("worker: sigaction");
        return 1;
    }

    printf("[%s] started (pid=%ld, duration=%u)\n",
           worker_name, (long)getpid(), duration_seconds);
    (void)fflush(stdout);

    /* A zero duration means the worker runs until SIGTERM or SIGINT. */
    while (!stop_requested &&
           (duration_seconds == 0 || elapsed_seconds < duration_seconds)) {
        if (sleep_one_second() == -1) {
            perror("worker: nanosleep");
            return 1;
        }

        if (duration_seconds > 0 && !stop_requested) {
            elapsed_seconds++;
        }
    }

    printf("[%s] stopping (pid=%ld)\n", worker_name, (long)getpid());
    (void)fflush(stdout);
    return 0;
}
