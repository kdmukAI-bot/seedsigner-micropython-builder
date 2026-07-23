.PHONY: docker-shell stage-app docker-build-all dist clean clean-purge-cache full-reset

# Local dev uses the same prebaked GHCR image as CI.
# Override IMAGE if you need a pinned tag.
IMAGE ?= ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest

DOCKER_RUN = docker run --rm \
	--user $(shell id -u):$(shell id -g) \
	-e HOME=/tmp/home \
	$(if $(BOARD),-e BOARD=$(BOARD)) \
	$(if $(MP_ALLOW_DIRTY),-e MP_ALLOW_DIRTY=$(MP_ALLOW_DIRTY)) \
	$(if $(PROFILE),-e PROFILE=$(PROFILE)) \
	$(if $(MP_DISABLE_NETWORK),-e MP_DISABLE_NETWORK=$(MP_DISABLE_NETWORK)) \
	-v $(PWD):/workspace/seedsigner-micropython-builder \
	--tmpfs /tmp/home:uid=$(shell id -u),gid=$(shell id -g) \
	-v $(HOME)/.cache:/tmp/home/.cache \
	-w /workspace/seedsigner-micropython-builder

docker-shell:
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -it $(IMAGE) bash

# Host-side: stage the SeedSigner app + embit into frozen_app/ for the frozen
# build (tools/stage_frozen_app.py). Runs on the host because the Docker build
# mounts only this repo, so the app/embit siblings must be mirrored in first.
# Version override: SEEDSIGNER_VERSION=v9.9.9 make stage-app
# Source override:  SS_APP_DIR=... SS_EMBIT_DIR=... (or .env) -- defaults to the
# sibling checkouts; CI points these at the deps/seedsigner + deps/embit submodules.
stage-app:
	python3 tools/stage_frozen_app.py --board $(BOARD)

# One-liner: setup + firmware build + screenshot build inside Docker.
# Depends on stage-app so `make docker-build-all` auto-stages the frozen app tree.
docker-build-all: stage-app
	@mkdir -p $(HOME)/.cache
	$(DOCKER_RUN) -t $(IMAGE) bash -lc './scripts/docker_build_all.sh'

# Package flash-ready binaries into dist/<BOARD>/ for easy flashing.
# Requires a prior docker-build-all (or BOARD= docker-build-all) to produce artifacts.
BOARD ?= WAVESHARE_ESP32_S3_TOUCH_LCD_35B
DIST_DIR = dist/$(BOARD)

# BAKE_LAUNCHER=0 skips the /main.py launcher bake (firmware-only dist that boots to
# the REPL until a launcher is written; only useful for an app-less build).
BAKE_LAUNCHER ?= 1

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
	@# Bake the frozen-app launcher into a littlefs2 image for the auto-vfs partition so a
	@# fresh flash of this dist boots the app instead of the REPL. Geometry is derived from
	@# the built partition table + flash_args; the vfs offset is appended to flash_args.
	@if [ "$(BAKE_LAUNCHER)" = "1" ]; then \
		python3 tools/build_launcher_fs.py --board $(BOARD) \
			--build-dir build/$(BOARD) --dist-dir $(DIST_DIR); \
	else \
		echo "[dist] BAKE_LAUNCHER=0 -> firmware-only dist (no /main.py; boots to REPL)"; \
	fi
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
		deps/seedsigner-lvgl-screens/tools/screenshot_generator/build

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
