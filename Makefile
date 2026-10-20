CC ?= cc
.DEFAULT_GOAL := all
CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -O2 -UNDEBUG
ASAN_CFLAGS = -Iinclude -Wall -Wextra -std=c11 -pedantic -Werror -g -fsanitize=address,undefined -UNDEBUG
BUILD_DIR = build
SRC = src/codec.c src/reliability.c src/session.c src/session_tx.c src/session_rx.c src/rx.c src/io.c
PROFILES_SRC = src/stream.c src/scalar.c src/motion.c
BINS = $(BUILD_DIR)/test_rudp $(BUILD_DIR)/test_tfv $(BUILD_DIR)/test_window_min $(BUILD_DIR)/test_window_max $(BUILD_DIR)/test_phase4

.PHONY: all clean test test_rudp test_tfv test_window asan demo bench bench-report test-tools

HEADERS = $(wildcard include/*.h src/*_internal.h)
BENCH_ITERATIONS ?= 1000000
COMPARE_PYTHON ?= python3
VISUAL_PYTHON ?= python3

.PHONY: visual-trace visual-report visual-gif
visual-trace:
	$(VISUAL_PYTHON) bench/render_visual.py

visual-gif:
	$(VISUAL_PYTHON) bench/generate_terminal_gif.py

visual-report: visual-trace visual-gif

.PHONY: compare compare-smoke test-compare
compare:
	$(COMPARE_PYTHON) bench/run_comparison.py

compare-smoke:
	$(COMPARE_PYTHON) bench/run_comparison.py --messages 200 --seeds 1 --output $(BUILD_DIR)/comparison-smoke --no-plots

test-compare: compare-smoke
	$(COMPARE_PYTHON) tests/test_comparison.py

.PHONY: test-recovery-stress
test-recovery-stress: compare-smoke
	RECOVERY_SEEDS=100 $(COMPARE_PYTHON) tests/test_comparison.py

.PHONY: test-cmake
test-cmake:
	python3 tests/test_cmake.py

$(BUILD_DIR)/demo_loss: examples/demo_loss.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) examples/demo_loss.c -o $@

$(BUILD_DIR)/bench_rudp: bench/bench_rudp.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fno-lto $(SRC) bench/bench_rudp.c -o $@

demo: $(BUILD_DIR)/demo_loss

bench: $(BUILD_DIR)/bench_rudp
	./$(BUILD_DIR)/bench_rudp --iterations $(BENCH_ITERATIONS)

bench-report: $(BUILD_DIR)/bench_rudp
	mkdir -p docs/bench
	./$(BUILD_DIR)/bench_rudp --iterations $(BENCH_ITERATIONS) --csv docs/bench/codec.csv --svg $(BUILD_DIR)/codec.svg > docs/bench/environment.txt

$(BUILD_DIR)/test_demo_queue: tests/test_demo_queue.c examples/demo_loss.c $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) tests/test_demo_queue.c -o $@

test-tools: $(BUILD_DIR)/demo_loss $(BUILD_DIR)/bench_rudp $(BUILD_DIR)/test_demo_queue
	./$(BUILD_DIR)/test_demo_queue
	python3 tests/test_tools.py

all: $(BUILD_DIR) $(BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_rudp: $(SRC) tests/test_rudp.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) tests/test_rudp.c -o $@

$(BUILD_DIR)/test_phase4: $(PROFILES_SRC) tests/test_phase4.c include/protocol_profiles.h $(SRC) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) $(PROFILES_SRC) tests/test_phase4.c -o $@

$(BUILD_DIR)/test_tfv: tests/test_tfv.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_tfv.c -o $@

$(BUILD_DIR)/test_window_min: $(SRC) tests/test_window.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=2 $(SRC) tests/test_window.c -o $@

$(BUILD_DIR)/test_window_max: $(SRC) tests/test_window.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DRUDP_WINDOW_SIZE=32768 $(SRC) tests/test_window.c -o $@

test_rudp: $(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_rudp

test_tfv: $(BUILD_DIR)/test_tfv
	./$(BUILD_DIR)/test_tfv

test_window: $(BUILD_DIR)/test_window_min $(BUILD_DIR)/test_window_max
	./$(BUILD_DIR)/test_window_min
	./$(BUILD_DIR)/test_window_max

test: all
	./$(BUILD_DIR)/test_phase4
	./$(BUILD_DIR)/test_rudp
	./$(BUILD_DIR)/test_tfv
	./$(BUILD_DIR)/test_window_min
	./$(BUILD_DIR)/test_window_max

asan: | $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) $(SRC) $(PROFILES_SRC) tests/test_phase4.c -o $(BUILD_DIR)/test_phase4_asan
	./$(BUILD_DIR)/test_phase4_asan
	$(CC) $(ASAN_CFLAGS) $(SRC) tests/test_rudp.c -o $(BUILD_DIR)/test_rudp_asan
	$(CC) $(ASAN_CFLAGS) tests/test_tfv.c -o $(BUILD_DIR)/test_tfv_asan
	$(CC) $(ASAN_CFLAGS) -DRUDP_WINDOW_SIZE=2 $(SRC) tests/test_window.c -o $(BUILD_DIR)/test_window_min_asan
	$(CC) $(ASAN_CFLAGS) -DRUDP_WINDOW_SIZE=32768 $(SRC) tests/test_window.c -o $(BUILD_DIR)/test_window_max_asan
	./$(BUILD_DIR)/test_rudp_asan
	./$(BUILD_DIR)/test_tfv_asan
	./$(BUILD_DIR)/test_window_min_asan
	./$(BUILD_DIR)/test_window_max_asan

clean:
	rm -rf $(BUILD_DIR)

.PHONY: test-modules test-external-rx
test-modules:
	python3 tests/test_modules.py

test-external-rx: | $(BUILD_DIR)
	$(CC) $(ASAN_CFLAGS) -DRUDP_EXTERNAL_RX_STORAGE=1 $(SRC) tests/test_external_rx.c -o $(BUILD_DIR)/test_external_rx_asan
	./$(BUILD_DIR)/test_external_rx_asan
