CC = cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Iinclude
BUILD_DIR = build
SRC_DIR = src
TEST_DIR = tests

APP_SRCS = $(wildcard $(SRC_DIR)/*.c)
CORE_SRCS = $(filter-out $(SRC_DIR)/main.c, $(APP_SRCS))

APP_OBJS = $(APP_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CORE_OBJS = $(CORE_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TEST_OBJS = $(BUILD_DIR)/test_runner.o

APP = $(BUILD_DIR)/sqlproc
TEST_APP = $(BUILD_DIR)/test_runner

.PHONY: all test clean

all: $(APP)

$(APP): $(APP_OBJS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS)

$(TEST_APP): $(CORE_OBJS) $(TEST_OBJS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(TEST_OBJS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_runner.o: $(TEST_DIR)/test_runner.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_APP)
	./$(TEST_APP)

clean:
	rm -rf $(BUILD_DIR)
