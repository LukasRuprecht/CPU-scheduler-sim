CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -pthread -Iinclude
HEADERS  := $(wildcard include/wss/*.hpp)

.PHONY: all test tsan asan bench clean

all: build/tests build/bench

build:
	mkdir -p build

build/tests: tests/test_main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O2 -g $< -o $@

build/tests_tsan: tests/test_main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -DWSS_TSAN $< -o $@

build/tests_asan: tests/test_main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $< -o $@

build/bench: bench/bench.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $< -o $@

test: build/tests
	./build/tests

tsan: build/tests_tsan
	./build/tests_tsan

asan: build/tests_asan
	./build/tests_asan

bench: build/bench
	./build/bench --csv results/bench.csv

clean:
	rm -rf build
