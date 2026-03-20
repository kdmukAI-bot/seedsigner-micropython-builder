.PHONY: docker-shell docker-build-all clean clean-purge-ccache full-reset

# Local dev uses the same prebaked GHCR image as CI.
# Override IMAGE if you need a pinned tag.
IMAGE ?= ghcr.io/kdmukai-bot/seedsigner-micropython-builder-base:latest

docker-shell:
	docker run --rm -it \
		--user $(shell id -u):$(shell id -g) \
		-e HOME=/tmp/home \
		-e XDG_CACHE_HOME=/tmp/home/.cache \
		-v $(PWD):/workspace/seedsigner-micropython-builder \
		-v $(PWD)/.ccache:/tmp/home/.cache \
		-w /workspace/seedsigner-micropython-builder \
		$(IMAGE) bash

# One-liner: setup + firmware build + screenshot build inside Docker
docker-build-all:
	docker run --rm -t \
		--user $(shell id -u):$(shell id -g) \
		-e HOME=/tmp/home \
		-e XDG_CACHE_HOME=/tmp/home/.cache \
		-v $(PWD):/workspace/seedsigner-micropython-builder \
		-v $(PWD)/.ccache:/tmp/home/.cache \
		-w /workspace/seedsigner-micropython-builder \
		$(IMAGE) bash -lc './scripts/docker_build_all.sh'

# Safe clean: remove generated build outputs only (keeps deps/ working trees)
clean:
	rm -rf \
		build \
		logs \
		deps/micropython/upstream/ports/esp32/build* \
		deps/seedsigner-c-modules/tools/screenshot_generator/build

# Deeper clean: clean + purge local ccache
clean-purge-ccache: clean
	rm -rf .ccache

# Destructive reset: removes deps/ ephemeral clones and all generated artifacts.
# Requires explicit confirmation to avoid accidental loss of in-progress work.
full-reset:
	@if [ "$(CONFIRM)" != "YES" ]; then \
		echo "Refusing to run destructive reset."; \
		echo "Run: make full-reset CONFIRM=YES"; \
		exit 1; \
	fi
	rm -rf deps/micropython/upstream build logs .ccache
