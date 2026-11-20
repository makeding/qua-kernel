KERNEL_VERSION := 7.3.4
BUILD_DIR ?= $(CURDIR)/build
SOURCE := $(BUILD_DIR)/linux-$(KERNEL_VERSION)
OUTPUT := $(BUILD_DIR)/output

.PHONY: source build deb armbian-media armbian-rootfs
source:
	./scripts/prepare-source.sh "$(BUILD_DIR)"

build: source
	./scripts/build-qua.sh "$(SOURCE)" "$(OUTPUT)"

deb: build
	./scripts/build-deb.sh "$(SOURCE)" "$(OUTPUT)"

armbian-media:
	bash media/qua-transcode/build.sh "$(FFMPEG_ARCHIVE)" "$(BUILD_DIR)/media"

armbian-rootfs:
	bash image/build-armbian-rootfs.sh "$(ARMBIAN_ROOTFS)" "$(KERNEL_DEB)" "$(BUILD_DIR)/armbian" "$(BUILD_DIR)/media"
