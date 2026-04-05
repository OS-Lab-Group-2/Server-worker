# =============================================================================
# Makefile  —  Distributed Task Execution System
#
# Targets:
#   make all        — build machine_a and machine_b
#   make machine_a  — build only the coordinator
#   make machine_b  — build only the worker
#   make clean      — remove build artefacts
#   make install    — install binaries to /usr/local/bin (requires sudo)
#   make help       — show this message
# =============================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=gnu11 \
            -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS :=

# All targets depend on the shared header
DEPS    := protocol.h

.PHONY: all clean install help

all: server worker

server: server.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  [OK] Built $@"

worker: worker.c $(DEPS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  [OK] Built $@"

clean:
	@rm -f server worker
	@echo "  [OK] Cleaned build artefacts"

install: all
	install -m 0755 server /usr/local/bin/dte_coordinator
	install -m 0755 worker /usr/local/bin/dte_worker
	@echo "  [OK] Installed to /usr/local/bin/"

help:
	@echo "Distributed Task Execution System — Makefile targets:"
	@echo "  all        build both server and worker"
	@echo "  server     build the coordinator only"
	@echo "  worker     build the worker only"
	@echo "  clean      remove compiled binaries"
	@echo "  install    copy binaries to /usr/local/bin (needs sudo)"
