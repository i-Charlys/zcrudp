CC = gcc
CFLAGS = -Iinclude -Wall -Wextra -O2
BUILD_DIR = build
SRC = src/rudp.c
TESTS = tests/test_rudp.c tests/test_tfv.c
BINS = $(BUILD_DIR)/test_rudp $(BUILD_DIR)/test_tfv

.PHONY: all clean test test_rudp test_tfv

all: $(BUILD_DIR) $(BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_rudp: src/rudp.c tests/test_rudp.c
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_tfv: tests/test_tfv.c
	$(CC) $(CFLAGS) $^ -o $@

test_rudp: $(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_rudp

test_tfv: $(BUILD_DIR)/test_tfv
	./$(BUILD_DIR)/test_tfv

test: all
	./$(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_tfv

clean:
	rm -rf $(BUILD_DIR)
