CC = gcc
CFLAGS = -std=c99 -O3 -Wall -Wextra -Iinclude
LDFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests
BENCH_DIR = benchmarks

TARGET = $(BIN_DIR)/lsm_engine
TEST_TARGET = $(BIN_DIR)/run_tests
BENCH_TARGET = $(BIN_DIR)/run_bench

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.c)

.PHONY: all build test benchmark clean directories

all: directories build test

directories:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

build: directories $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: directories $(TEST_TARGET)
	@if [ -f $(TEST_TARGET) ]; then \
		./$(TEST_TARGET); \
	else \
		echo "No tests found."; \
	fi

$(TEST_TARGET): $(OBJS) $(TEST_SRCS)
	@if [ -n "$(TEST_SRCS)" ]; then \
		$(CC) $(CFLAGS) $(TEST_SRCS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) -o $@ $(LDFLAGS); \
	fi

benchmark: directories $(BENCH_TARGET)
	@if [ -f $(BENCH_TARGET) ]; then \
		./$(BENCH_TARGET); \
	else \
		echo "No benchmarks found."; \
	fi

$(BENCH_TARGET): $(OBJS) $(BENCH_SRCS)
	@if [ -n "$(BENCH_SRCS)" ]; then \
		$(CC) $(CFLAGS) $(BENCH_SRCS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) -o $@ $(LDFLAGS); \
	fi

clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR) *.db *.log