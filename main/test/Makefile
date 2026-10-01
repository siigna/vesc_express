# Host tests for firmware pieces with no hardware dependencies.
#
# Compiled straight from the firmware source, not copied, so the tests cannot
# drift from what ships. LOG_RING_HOST_TEST stubs the FreeRTOS spinlock and
# the esp_log hook; everything else is the shipping code.

CFLAGS += -O1 -g -std=c11 -Wall -Wextra -Wconversion -Wsign-conversion
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all run clean
all: run

test_log_ring: test_log_ring.c ../log_ring.c ../log_ring.h
	$(CC) $(CFLAGS) $(SAN) -DLOG_RING_HOST_TEST -o $@ test_log_ring.c ../log_ring.c

run: test_log_ring
	./test_log_ring

clean:
	rm -f test_log_ring
