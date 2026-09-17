CC       := gcc
CXX      := g++
CFLAGS   := -std=c99   -Wall -Wextra -Wpedantic -O1 -g -Irtu-core
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O1 -g -Isrc -Irtu-core
TSAN     := -fsanitize=thread
ASAN     := -fsanitize=address,undefined -fno-omit-frame-pointer

CORE_SRC := rtu-core/modbus_crc.c rtu-core/modbus_slave.c
CXX_SRC  := src/mbap.cpp src/gateway.cpp

THREADS  ?= 16
PER      ?= 5000

.PHONY: all verify tsan asan clean help

help:
	@echo "make verify - build and run with ThreadSanitizer"
	@echo "make asan   - same suite under AddressSanitizer + UBSan"
	@echo "make tsan   - build the TSan binary only"

all: verify

# C sources must be compiled by the C compiler. g++ would treat .c as C++ and
# mangle the names, which the extern \"C\" header then fails to resolve.
build/%.tsan.o: rtu-core/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(TSAN) -c $< -o $@

build/%.asan.o: rtu-core/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(ASAN) -c $< -o $@

build/test_gateway_tsan: $(CXX_SRC) test/test_gateway.cpp \
                         build/modbus_crc.tsan.o build/modbus_slave.tsan.o
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(TSAN) $(CXX_SRC) test/test_gateway.cpp \
	       build/modbus_crc.tsan.o build/modbus_slave.tsan.o -o $@

build/test_gateway_asan: $(CXX_SRC) test/test_gateway.cpp \
                         build/modbus_crc.asan.o build/modbus_slave.asan.o
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(ASAN) $(CXX_SRC) test/test_gateway.cpp \
	       build/modbus_crc.asan.o build/modbus_slave.asan.o -o $@

tsan: build/test_gateway_tsan

verify: build/test_gateway_tsan
	./build/test_gateway_tsan $(THREADS) $(PER)

asan: build/test_gateway_asan
	./build/test_gateway_asan $(THREADS) $(PER)

clean:
	rm -rf build

# ---- the server itself ---------------------------------------------------
build/modbus_crc.o: rtu-core/modbus_crc.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/modbus_slave.o: rtu-core/modbus_slave.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/gateway: $(CXX_SRC) src/main.cpp build/modbus_crc.o build/modbus_slave.o
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -pthread $(CXX_SRC) src/main.cpp \
	       build/modbus_crc.o build/modbus_slave.o -o $@

gateway: build/gateway

# ---- ARM Cortex-A cross build ---------------------------------------------
# Proves the gateway actually builds for the target it claims, rather than
# asserting "cross-compilable" in a README and never trying it.
ARM_CXX := arm-linux-gnueabihf-g++
ARM_CC  := arm-linux-gnueabihf-gcc

build/arm/%.o: rtu-core/%.c
	@mkdir -p build/arm
	$(ARM_CC) $(CFLAGS) -c $< -o $@

build/gateway-armhf: $(CXX_SRC) src/main.cpp \
                     build/arm/modbus_crc.o build/arm/modbus_slave.o
	@mkdir -p build/arm
	$(ARM_CXX) $(CXXFLAGS) -pthread -static $(CXX_SRC) src/main.cpp \
	       build/arm/modbus_crc.o build/arm/modbus_slave.o -o $@
	@file $@ 2>/dev/null || true

arm: build/gateway-armhf
	@echo "cross build OK"

# ---- structural coverage --------------------------------------------------
# Reported for this project's own translation units only.
#
# Note on the numbers: branch coverage in C++ is not comparable to the C
# figure in the Modbus project. Every STL call generates exception-unwind
# branches that never fire in a run without throws, so a "branch taken"
# percentage here measures how much of libstdc++ you didn't exercise, not how
# much of your logic you did. Line coverage of the project's own files is the
# honest metric at this layer; the exhaustive proofs live in the C core.
coverage:
	@mkdir -p build/cov
	@rm -f build/cov/*.gcda build/cov/*.gcno
	$(CC) -std=c99 -O0 -g --coverage -Irtu-core -c rtu-core/modbus_crc.c   -o build/cov/crc.o
	$(CC) -std=c99 -O0 -g --coverage -Irtu-core -c rtu-core/modbus_slave.c -o build/cov/slave.o
	$(CXX) -std=c++17 -O0 -g --coverage -Isrc -Irtu-core -pthread \
	       src/mbap.cpp src/gateway.cpp test/test_gateway.cpp \
	       build/cov/crc.o build/cov/slave.o -o build/cov/cov_gw
	cd build/cov && ./cov_gw 4 500 > /dev/null
	@echo ""
	@echo "line coverage, this project's sources:"
	@rm -f *.gcov
	@for f in mbap gateway; do \
	   gcov -s . build/cov/cov_gw-$$f.gcda 2>/dev/null | \
	     grep -A1 "File 'src/$$f.cpp'" | grep -E "^File|^Lines" ; \
	   n=$$(awk -F: '$$1 ~ /#####/' $$f.cpp.gcov 2>/dev/null | wc -l); \
	   echo "  uncovered SOURCE lines: $$n"; \
	   if [ "$$n" != "0" ]; then awk -F: '$$1 ~ /#####/ {print "    line "$$2":"$$3}' $$f.cpp.gcov; fi; \
	 done
	@echo ""
	@echo "  The percentages sit below 100 because gcov counts compiler-emitted"
	@echo "  exception-cleanup code at -O0 that has no source line to test."
	@echo "  Uncovered SOURCE lines is the number that means something here."
	@echo ""
	@echo "reused Modbus core (C, exhaustively verified in mb-rtu):"
	@gcov -b -s . build/cov/modbus_slave.gcda 2>/dev/null | \
	   grep -E "^File 'rtu-core/modbus_slave|^Lines|^Taken" || true

# ---- static analysis ------------------------------------------------------
analyze:
	cppcheck --enable=all --inconclusive --check-level=exhaustive \
	         --std=c++17 --error-exitcode=1 \
	         --suppress=missingIncludeSystem --suppress=unusedFunction \
	         --suppress=checkersReport \
	         -Isrc -Irtu-core src/ 2>&1 | tail -12
	@echo "cppcheck: clean"

.PHONY: coverage analyze demo

# ---- one-command demo -----------------------------------------------------
# A committed transcript instead of a screen recording: readable in the repo,
# regenerated by CI, and it cannot rot the way a video link does.
demo:
	@mkdir -p logs
	@{ \
	  echo "########################################################"; \
	  echo "#  Modbus TCP/RTU Gateway - full run                    #"; \
	  echo "########################################################"; \
	  echo ""; echo "### 1. unit + concurrency tests under ThreadSanitizer ###"; \
	  $(MAKE) -s verify THREADS=16 PER=5000; \
	  echo ""; echo "### 2. routing invariant at scale ###"; \
	  ./build/test_gateway_tsan 64 20000 2>&1 | sed -n '/routing invariant/,/every transaction/p'; \
	  echo ""; echo "### 3. AddressSanitizer + UBSan ###"; $(MAKE) -s asan THREADS=8 PER=2000 | tail -4; \
	  echo ""; echo "### 4. line coverage ###"; $(MAKE) -s coverage; \
	  echo ""; echo "### 5. static analysis ###"; $(MAKE) -s analyze; \
	  echo ""; echo "### 6. ARM Cortex-A cross build ###"; \
	  if command -v arm-linux-gnueabihf-g++ >/dev/null; then \
	    $(MAKE) -s arm && file build/gateway-armhf; \
	  else echo "SKIPPED: arm-linux-gnueabihf-g++ not installed"; fi; \
	  echo ""; echo "### 6b. native server build ###"; $(MAKE) -s gateway; \
	  echo ""; echo "### 7. 100 concurrent TCP masters ###"; ./tools/run_soak.sh 100 200; \
	  echo ""; echo "### 8. fault soak, 15% drop + 15% corrupt ###"; ./tools/run_fault_soak.sh 20 300; \
	  echo ""; echo "### 9. three-tier: TCP -> gateway -> serial -> RTU slave ###"; \
	  SLAVE_DIR=$${SLAVE_DIR:-$$HOME/mb-rtu} ./tools/run_three_tier.sh; \
	  echo ""; echo "### 10. serial device unplug and reconnect ###"; \
	  SLAVE_DIR=$${SLAVE_DIR:-$$HOME/mb-rtu} ./tools/test_reconnect.sh; \
	  echo ""; echo "### 11. live Wireshark capture ###"; ./tools/capture.sh; \
	} 2>&1 | tee logs/demo.log
	@echo ""; echo "transcript written to logs/demo.log"
