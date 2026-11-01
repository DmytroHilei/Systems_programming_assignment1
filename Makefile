# SPDX-License-Identifier: MIT

BUILD := build
TARGET := ptop
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)
TEST_OBJS := $(filter-out $(BUILD)/main.o,$(OBJS))


CFLAGS := -std=gnu17 -Wall -Wextra -Wshadow -MMD -MP
OPT := -O2

.PHONY: all debug test format clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OPT) -o $@ $^


$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(OPT) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Sanitizer objects cannot be mixed with normal ones, so they get their own directory
debug:
	$(MAKE) BUILD=build/debug TARGET=build/debug/ptop OPT="-g -O0 -fsanitize=address,undefined"

$(BUILD)/test_proc: tests/test_proc.c $(TEST_OBJS)
	$(CC) $(CFLAGS) $(OPT) -Isrc -o $@ $^

test: $(BUILD)/test_proc
	./$<

format:
	clang-format -i src/*.c src/*.h tests/*.c

clean:
	rm -rf $(BUILD) ptop

-include $(OBJS:.o=.d)

