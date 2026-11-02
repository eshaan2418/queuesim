J ?= $(shell n=$$( (sysctl -n hw.ncpu 2>/dev/null || nproc) ); echo $$(( n/2 > 2 ? n/2 : 2 )))
ITCH ?= $(HOME)/quantfactory/_data/itch/01302019.NASDAQ_ITCH50.gz
.PHONY: build test sanitize bench demo data lint clean
build:
	cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j $(J)
test: build
	ctest --test-dir build -j $(J) --output-on-failure
sanitize:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUEUESIM_SANITIZE=address,undefined
	cmake --build build-asan -j $(J) && ctest --test-dir build-asan -j $(J) --output-on-failure
# Full-day experiment suite (fill models x symbols, latency sweep, imbalance taker, determinism).
bench: build
	ITCH=$(ITCH) bash scripts/run_experiments.sh
data:
	@echo "Uses the NASDAQ ITCH sample from itchbook: https://github.com/eshaan2418/itchbook (make data there)"
	@test -f $(ITCH) || bash scripts/fetch_itch.sh
demo: build
	build/queuesim --itch $(ITCH) --symbol MSFT --strategy as --model all | python3 scripts/demo_fmt.py
lint:
	@command -v clang-tidy >/dev/null && clang-tidy -p build tools/*.cpp || echo "clang-tidy not installed"
clean:
	rm -rf build build-asan
