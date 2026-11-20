KERNEL_VERSION := 7.3.4
BUILD_DIR ?= $(CURDIR)/build
SOURCE := $(BUILD_DIR)/linux-$(KERNEL_VERSION)
OUTPUT := $(BUILD_DIR)/output

.PHONY: source build deb
source:
	./scripts/prepare-source.sh "$(BUILD_DIR)"

build: source
	./scripts/build-qua.sh "$(SOURCE)" "$(OUTPUT)"

deb: build
	./scripts/build-deb.sh "$(SOURCE)" "$(OUTPUT)"
