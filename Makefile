CC = gcc
.DEFAULT_GOAL := all
CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -O2 -UNDEBUG
ASAN_CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -g -fsanitize=address,undefined -UNDEBUG
BUILD_DIR = build
SRC = src/rudp.c
TESTS = tests/test_rudp.c tests/test_tfv.c tests/test_window.c
BINS = $(BUILD_DIR)/test_rudp $(BUILD_DIR)/test_tfv $(BUILD_DIR)/test_window_min $(BUILD_DIR)/test_window_max

.PHONY: all clean test test_rudp test_tfv test_window asan demo bench bench-report test-tools

HEADERS = include/protocol_rudp.h include/protocol_tfv.h
BENCH_ITERATIONS ?= 1000000

$(BUILD_DIR)/demo_loss: examples/demo_loss.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) examples/demo_loss.c -o $@

$(BUILD_DIR)/bench_rudp: bench/bench_rudp.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fno-lto $(SRC) bench/bench_rudp.c -o $@

demo: $(BUILD_DIR)/demo_loss

bench: $(BUILD_DIR)/bench_rudp
	./$(BUILD_DIR)/bench_rudp --iterations $(BENCH_ITERATIONS)

bench-report: $(BUILD_DIR)/bench_rudp
	mkdir -p docs/bench
	./$(BUILD_DIR)/bench_rudp --iterations $(BENCH_ITERATIONS) --csv docs/bench/codec.csv --svg docs/bench/codec.svg > docs/bench/environment.txt

$(BUILD_DIR)/test_demo_queue: tests/test_demo_queue.c examples/demo_loss.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) tests/test_demo_queue.c -o $@

test-tools: $(BUILD_DIR)/demo_loss $(BUILD_DIR)/bench_rudp $(BUILD_DIR)/test_demo_queue
	./$(BUILD_DIR)/test_demo_queue
	python3 tests/test_tools.py

all: $(BUILD_DIR) $(BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_rudp: src/rudp.c tests/test_rudp.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/rudp.c tests/test_rudp.c -o $@

$(BUILD_DIR)/test_tfv: tests/test_tfv.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_tfv.c -o $@

$(BUILD_DIR)/test_window_min: src/rudp.c tests/test_window.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=2 src/rudp.c tests/test_window.c -o $@

$(BUILD_DIR)/test_window_max: src/rudp.c tests/test_window.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=32768 src/rudp.c tests/test_window.c -o $@

test_rudp: $(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_rudp

test_tfv: $(BUILD_DIR)/test_tfv
	./$(BUILD_DIR)/test_tfv

test_window: $(BUILD_DIR)/test_window_min $(BUILD_DIR)/test_window_max
	./$(BUILD_DIR)/test_window_min
	./$(BUILD_DIR)/test_window_max

test: all
	./$(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_tfv
	./$(BUILD_DIR)/test_window_min
	./$(BUILD_DIR)/test_window_max

asan: clean $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) src/rudp.c tests/test_rudp.c -o $(BUILD_DIR)/test_rudp_asan
	$(CC) $(ASAN_CFLAGS) tests/test_tfv.c -o $(BUILD_DIR)/test_tfv_asan
	$(CC) $(ASAN_CFLAGS) -DRUDP_WINDOW_SIZE=2 src/rudp.c tests/test_window.c -o $(BUILD_DIR)/test_window_min_asan
	$(CC) $(ASAN_CFLAGS) -DRUDP_WINDOW_SIZE=32768 src/rudp.c tests/test_window.c -o $(BUILD_DIR)/test_window_max_asan
	./$(BUILD_DIR)/test_rudp_asan
	./$(BUILD_DIR)/test_tfv_asan
	./$(BUILD_DIR)/test_window_min_asan
	./$(BUILD_DIR)/test_window_max_asan

clean:
	rm -rf $(BUILD_DIR)
