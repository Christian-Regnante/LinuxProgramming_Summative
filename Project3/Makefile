# Project 3 - convenience wrapper around setup.py (the required build configuration)
PYTHON ?= python3

.PHONY: all build test demo bench sanitize evidence clean

all: build

build:
	$(PYTHON) setup.py build_ext --inplace

test: build
	$(PYTHON) test_sensor_processor.py --tests

demo: build
	$(PYTHON) test_sensor_processor.py --demo

bench: build
	$(PYTHON) benchmark.py

sanitize:
	./sanitizer_check.sh

evidence: build
	./capture_evidence.sh

clean:
	rm -rf build *.egg-info __pycache__ sensor_processor*.so
