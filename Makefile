.PHONY: docker-build docker-shell

docker-build:
	docker build -f Dockerfile.dev -t seedsigner-micropython-builder-dev .

docker-shell:
	docker run --rm -it \
		-v $(PWD):/workspace/seedsigner-micropython-builder \
		-w /workspace/seedsigner-micropython-builder \
		seedsigner-micropython-builder-dev bash
