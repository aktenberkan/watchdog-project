CC = gcc

CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?=

WATCHDOG_SOURCES := src/main.c src/signal_manager.c src/process_manager.c src/logger.c
WORKER_SOURCES := workers/worker.c

WATCHDOG_OBJECTS := $(WATCHDOG_SOURCES:.c=.o)
WORKER_OBJECTS := $(WORKER_SOURCES:.c=.o)

.PHONY: all clean test

all: watchdog worker

watchdog: $(WATCHDOG_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

worker: $(WORKER_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(WATCHDOG_OBJECTS): $(wildcard include/*.h)

clean:
	rm -f $(WATCHDOG_OBJECTS) $(WORKER_OBJECTS) watchdog worker

test: all
	bash tests/test_scenarios.sh
