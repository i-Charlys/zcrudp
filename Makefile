CC = gcc
CFLAGS = -Iinclude -Wall -Wextra -O2
BUILD_DIR = build
SRC = src/rudp.c
TESTS = tests/test_rudp.c tests/test_tlv.c
BINS = $(BUILD_DIR)/test_rudp $(BUILD_DIR)/test_tlv

.PHONY: all clean test

all: $(BUILD_DIR) $(BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_rudp: src/rudp.c tests/test_rudp.c
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_tlv: tests/test_tlv.c
	$(CC) $(CFLAGS) $^ -o $@

test: all
	./$(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_tlv

clean:
	rm -rf $(BUILD_DIR)
