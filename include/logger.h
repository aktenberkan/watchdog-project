#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>
#include <sys/types.h>

#include "process_manager.h"

int logger_init(const char *path);
int logger_event(const char *event,
                 const char *worker_name,
                 pid_t pid,
                 const char *details);
int logger_status(const process_info_t *items, size_t count);
void logger_close(void);

#endif /* LOGGER_H */
