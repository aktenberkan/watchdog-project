# C standardı sabitlendi: GCC 11 varsayılanı gnu17, GCC 15 varsayılanı gnu23.
# -std=c11 sayesinde iki derleyici de kodu aynı kurallarla derler.
# _POSIX_C_SOURCE: -std=c11 ile sigaction, kill, fork gibi POSIX fonksiyonlarını açar.
CC      = gcc
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
CFLAGS  += -Iinclude
LDFLAGS ?=

WATCHDOG_SRC := $(wildcard src/*.c)
WORKER_SRC   := $(wildcard workers/*.c)

WATCHDOG_OBJ := $(WATCHDOG_SRC:.c=.o)
WORKER_OBJ   := $(WORKER_SRC:.c=.o)

TARGETS :=
ifneq ($(WATCHDOG_SRC),)
TARGETS += watchdog
endif
ifneq ($(WORKER_SRC),)
TARGETS += worker
endif

.PHONY: all clean

all: $(TARGETS)

watchdog: $(WATCHDOG_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

worker: $(WORKER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c $(wildcard include/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(WATCHDOG_OBJ) $(WORKER_OBJ) watchdog worker
