CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread -lm

.PHONY: all check clean
all: pb
pb: pb.c pb_stats.h pb_clock.h pb_rates.h pb_history.h pb_io.h pb_format.h pb_terminal.h pb_ui.h pb_ipc.h pb_watch.h pb_monitor.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pb.c $(LDLIBS)

check: pb
	python3 -m unittest discover -s tests -v

clean:
	rm -f pb
