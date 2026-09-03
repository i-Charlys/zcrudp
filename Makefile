CC = gcc
CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -O2 -UNDEBUG
ASAN_CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -g -fsanitize=address,undefined -UNDEBUG
BUILD_DIR = build
SRC = src/rudp.c
TESTS = tests/test_rudp.c tests/test_tfv.c tests/test_window.c
BINS = $(BUILD_DIR)/test_rudp $(BUILD_DIR)/test_tfv $(BUILD_DIR)/test_window_min $(BUILD_DIR)/test_window_max

.PHONY: all clean test test_rudp test_tfv test_window asan

all: $(BUILD_DIR) $(BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_rudp: src/rudp.c tests/test_rudp.c
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_tfv: tests/test_tfv.c
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_window_min: src/rudp.c tests/test_window.c
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=2 $^ -o $@

$(BUILD_DIR)/test_window_max: src/rudp.c tests/test_window.c
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=32768 $^ -o $@

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
