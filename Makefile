.PHONY: docker-shell docker-build-all dist clean clean-purge-cache full-reset

# Local dev uses the same prebaked GHCR image as CI.
# Override IMAGE if you need a pinned tag.
IMAGE ?= ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest

DOCKER_RUN = docker run --rm \
	--user $(shell id -u):$(shell id -g) \
	-e HOME=/tmp/home \
	$(if $(BOARD),-e BOARD=$(BOARD)) \
	$(if $(MP_ALLOW_DIRTY),-e MP_ALLOW_DIRTY=$(MP_ALLOW_DIRTY)) \
	-v $(PWD):/workspace/seedsigner-micropython-builder \
	--tmpfs /tmp/home:uid=$(shell id -u),gid=$(shell id -g) \
	-v $(HOME)/.cache:/tmp/home/.cache \
	-w /workspace/seedsigner-micropython-builder

docker-shell:
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -it $(IMAGE) bash

# One-liner: setup + firmware build + screenshot build inside Docker
docker-build-all:
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -t $(IMAGE) bash -lc './scripts/docker_build_all.sh'

# Package flash-ready binaries into dist/<BOARD>/ for easy flashing.
# Requires a prior docker-build-all (or BOARD= docker-build-all) to produce artifacts.
BOARD ?= WAVESHARE_ESP32_S3_TOUCH_LCD_35B
DIST_DIR = dist/$(BOARD)

dist:
	@if [ ! -f build/$(BOARD)/flash_args ]; then \
		echo "ERROR: No build artifacts for BOARD=$(BOARD). Run: make docker-build-all"; \
		exit 1; \
	fi
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)/bootloader $(DIST_DIR)/partition_table
	cp build/$(BOARD)/flash_args $(DIST_DIR)/
	cp build/$(BOARD)/micropython.bin $(DIST_DIR)/
	cp build/$(BOARD)/bootloader/bootloader.bin $(DIST_DIR)/bootloader/
	cp build/$(BOARD)/partition_table/partition-table.bin $(DIST_DIR)/partition_table/
	@CHIP=$$(case "$(BOARD)" in *ESP32_P4*) echo esp32p4;; *) echo esp32s3;; esac); \
	echo ""; \
	echo "Flash with:"; \
	echo "  cd $(DIST_DIR) && python -m esptool --chip $$CHIP write_flash @flash_args"

# Safe clean: remove generated build outputs only (keeps deps/ working trees)
clean:
	rm -rf \
		build \
		dist \
		logs \
		deps/micropython/upstream/ports/esp32/build* \
		deps/seedsigner-c-modules/tools/screenshot_generator/build

# Destructive reset: removes all generated artifacts and resets submodule working trees.
# Requires explicit confirmation to avoid accidental loss of in-progress work.
full-reset:
	@if [ "$(CONFIRM)" != "YES" ]; then \
		echo "Refusing to run destructive reset."; \
		echo "Run: make full-reset CONFIRM=YES"; \
		exit 1; \
	fi
	rm -rf build logs
	git -C deps/micropython/upstream checkout -- .
	git -C deps/micropython/upstream clean -fd
	git -C deps/micropython/upstream submodule foreach --recursive 'git checkout -- . 2>/dev/null; git clean -fd 2>/dev/null' || true
