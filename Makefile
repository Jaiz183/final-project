# Build for the CSC443/CSC2234 baseline key-value store.
#
#   make            build the library, the tests, the examples and the benchmark
#   make test       build and run the whole test suite
#   make example    build and run examples/example.cpp, a tour of the API
#   make clean      remove everything under build/
#
# Useful overrides:
#   make CXX=clang++
#   make BUILD_TYPE=debug      # -O0 -g3, assertions on
#   make SANITIZE=address      # or 'undefined', or 'address,undefined'

CXX      ?= g++
BUILD_TYPE ?= release
SANITIZE ?=

WARNINGS := -Wall -Wextra -Wpedantic
# _GNU_SOURCE is what exposes O_DIRECT in glibc's <fcntl.h>.
COMMON   := -std=c++17 -D_GNU_SOURCE -Iinclude $(WARNINGS) -MMD -MP

ifeq ($(BUILD_TYPE),debug)
  COMMON += -O0 -g3
else
  COMMON += -O2 -g -DNDEBUG
endif

ifneq ($(SANITIZE),)
  COMMON  += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=$(SANITIZE)
endif

CXXFLAGS := $(COMMON) $(EXTRA_CXXFLAGS)

BUILD := build
LIB   := $(BUILD)/libkvstore.a

LIB_SRCS  := $(wildcard src/*.cpp)
LIB_OBJS  := $(patsubst src/%.cpp,$(BUILD)/src/%.o,$(LIB_SRCS))

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRCS))
TEST_BIN  := $(BUILD)/kv_tests

BENCH_SRCS := $(wildcard tools/*.cpp)
BENCH_BIN  := $(BUILD)/kv_bench

# Every .cpp in examples/ becomes its own binary, build/<name>.
EXAMPLE_SRCS := $(wildcard examples/*.cpp)
EXAMPLE_BINS := $(patsubst examples/%.cpp,$(BUILD)/%,$(EXAMPLE_SRCS))

.PHONY: all test example clean

all: $(LIB) $(TEST_BIN) $(BENCH_BIN) $(EXAMPLE_BINS)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itests -c $< -o $@

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(TEST_OBJS) $(LIB) $(LDFLAGS) -o $@

$(BENCH_BIN): $(BENCH_SRCS) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) $(LIB) $(LDFLAGS) -o $@

# A static pattern rule, so it applies only to the example binaries and cannot be
# matched against anything else under build/.
$(EXAMPLE_BINS): $(BUILD)/%: examples/%.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

example: $(BUILD)/example
	./$(BUILD)/example

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
