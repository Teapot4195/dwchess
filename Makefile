BUILD_DIR := build
BUILD_TYPE ?= Release

.PHONY: all configure build clean

all: build

configure:
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR)
	cp build/dwchess $(EXE)

clean:
	rm -rf $(BUILD_DIR)
