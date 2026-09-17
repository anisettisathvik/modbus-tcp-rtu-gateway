# Modbus TCP/RTU Industrial Gateway

A protocol gateway bridging Modbus TCP masters to a Modbus RTU serial segment,
written in C++17 for Linux and cross-compiled for ARM Cortex-A. RTU carries a
CRC and no transaction identifier; TCP carries a transaction identifier and no
CRC, so the gateway must hold that identifier in memory while the request is on
the serial bus and reattach it to the response. Crossing that association would
deliver one master's data to another, which makes transaction routing the
safety-critical property — verified under ThreadSanitizer across 1,280,000
transactions from 64 concurrent masters. The RTU endpoint is not a mock: the
verified slave core from the companion `mb-rtu` project is vendored in
`rtu-core/` and runs behind the gateway.

---

## Repository layout

```
mb-gateway/
├── src/
│   ├── mbap.hpp / mbap.cpp   MBAP header codec, stream decoder
│   ├── gateway.hpp / .cpp    TCP/RTU translation and gateway exceptions
│   ├── rtu_transport.hpp     transport interface, in-process backend,
│   │                         deterministic fault injector
│   ├── serial_rtu.hpp        POSIX serial transport with auto-reconnect
│   ├── config.hpp            key = value configuration file
│   └── main.cpp              multithreaded TCP server, CLI, diagnostics
├── rtu-core/                 vendored Modbus slave core (from mb-rtu)
│   ├── modbus_slave.h / .c
│   └── modbus_crc.h / .c
├── test/
│   └── test_gateway.cpp      codec, translation, routing invariant, faults
├── tools/
│   ├── tcp_master.py         Modbus TCP masters, battery and soak
│   ├── run_soak.sh           100 concurrent masters
│   ├── run_fault_soak.sh     soak with injected bus faults
│   ├── run_three_tier.sh     TCP -> gateway -> serial -> RTU slave
│   ├── test_reconnect.sh     serial device loss and recovery
│   └── capture.sh            pcap capture and tshark decode
├── captures/
│   └── modbus_tcp.pcap       sample capture, decodable by Wireshark
├── .github/workflows/ci.yml
├── gateway.conf
└── Makefile
```

---

## Requirements

```bash
sudo apt install build-essential socat tcpdump tshark \
                 cppcheck g++-arm-linux-gnueabihf
```

`tools/run_three_tier.sh` and `tools/test_reconnect.sh` need the `mb-rtu`
project checked out; point `SLAVE_DIR` at it.

## Build and run

```bash
make verify      # full suite under ThreadSanitizer
make asan        # same suite under AddressSanitizer + UBSan
make coverage    # line coverage of this project's sources
make analyze     # cppcheck static analysis
make gateway     # build the server
make arm         # cross-compile for ARM Cortex-A (armhf)
make demo        # all of the above, transcript to logs/demo.log
```

```bash
./tools/run_soak.sh 100 200
./tools/run_fault_soak.sh 20 300
SLAVE_DIR=../mb-rtu ./tools/run_three_tier.sh
SLAVE_DIR=../mb-rtu ./tools/test_reconnect.sh
sudo ./tools/capture.sh          # tcpdump needs root
```

Running the server directly:

```bash
./build/gateway --config gateway.conf
./build/gateway --port 5020 --serial /dev/ttyUSB0 --baud 19200
kill -USR1 <pid>                 # dump live counters without stopping
```

---

## Verification results

Measured on Ubuntu 26.04 under WSL2 — GCC 13.3.

| Check | Result |
|---|---|
| Unit and integration tests | 40 / 40 pass |
| Routing invariant | 1,280,000 transactions, 64 masters, 0 misrouted |
| ThreadSanitizer | clean across all concurrency tests |
| AddressSanitizer + UBSan | clean |
| Line coverage | 0 uncovered source lines in `gateway.cpp` and `mbap.cpp` |
| Static analysis | cppcheck `--check-level=exhaustive`, clean |
| Live TCP soak | 100 masters, 20,000 transactions, 0 errors |
| Fault soak | 6,000 transactions, 1,700 injected faults, 0 corrupt payloads forwarded |
| Three-tier chain | 7 / 7 over a real serial link |
| Serial reconnect | 5/5 before, 5/5 exception 0x0B during, 8/8 after |
| ARM cross build | `ELF 32-bit LSB, ARM, EABI5`, runs under `qemu-arm` |
| Wireshark capture | decoded by the `mbtcp` dissector |

Throughput is environment-bound: 64,379 txn/s on bare Linux and 6,662 txn/s
under WSL2, both with 100 concurrent masters and zero misrouted responses. WSL2
routes loopback through a virtualised network stack, so TCP syscalls cost
roughly ten times more. The in-process figure is identical on both.

### The routing invariant

> For every response delivered, `(client, transaction_id, unit_id)` must equal
> the request that produced it, under any amount of concurrency.

The check is not that a response arrived. `InProcessRtu` seeds register *i* of
slave *u* to `(u<<8)|i`, so the returned value proves the request reached the
right device at the right address. Multi-master routing bugs are data races,
which are timing-dependent and invisible on a logic analyzer; ThreadSanitizer
finds them deterministically.

### Fault injection

`FaultyRtu` is a seeded decorator between the gateway and the bus, so a specific
fault sequence replays exactly. At 15% response loss and 15% bit corruption over
6,000 transactions:

```
requests=6000  responses=4300  timeouts=913  crc_failures=787
misrouted=0    corrupt payloads accepted by any master = 0
```

Every injected fault became an explicit gateway exception.

**Gateway exception codes:** `0x0B` (target device failed to respond) when no
device answered or the response failed CRC, and `0x0A` (gateway path
unavailable) for a malformed request. Returning `0x0B` rather than going silent
lets the master distinguish a dead field device from a dead gateway.

### Stream framing

TCP does not preserve message boundaries, so treating one `recv()` as one frame
is a common failure. The decoder is a stream assembler, tested against a frame
one byte short, a 3-byte prefix, two complete frames in one segment, one frame
split across two segments, a valid frame followed by garbage, and each of the
three header validation failures. A framing error closes the connection: the
length field is how the next boundary is found, so once it is untrustworthy the
stream cannot be resynchronised.

### Wireshark capture

Modbus TCP over loopback is real TCP, and Wireshark's `mbtcp` dissector decodes
it as it would on a plant network.

```
frame  trans_id        unit_id     func  exception
10     256,257,258     17,17,17    3,3,3      <- three requests, ONE segment
19     512             17          3          <- request split across 2 segments
21     768             99          3     11   <- 0x0B, no such device
23     1024            17          3     3    <- slave's own exception
```

---

## Design notes

**Thread per connection, not epoll.** A field gateway serves a handful of SCADA
masters rather than many thousands of sockets, and the blocking loop is simpler
to reason about and to check under ThreadSanitizer.

**`TCP_NODELAY` on every client socket.** Nagle would coalesce these 12-byte
responses and add tens of milliseconds to request latency.

**One mutex owns the bus.** RS-485 is physically one shared pair, so requests
must serialise onto it; the mutex models the constraint rather than working
around it.

**Graceful shutdown joins every worker.** Killing sockets mid-transaction leaves
masters waiting for their own timeout, which is indistinguishable from a dead
gateway.

**C sources are compiled with `gcc`, C++ with `g++`.** `g++` treats `.c` as C++
and mangles the names, which the `extern "C"` header then fails to resolve.

**Configuration is plain `key = value`.** A field gateway is configured by a
technician over a serial console; every key has a command-line override for
commissioning.

---

## Limitations and known gaps

**No physical Ethernet PHY and no RS-485 transceiver.** `IRtuTransport` is the
seam and both implementations exist: `InProcessRtu` for verification speed and
`SerialRtu` for a real tty. Pointing the latter at `/dev/ttyUSB0` instead of a
pty is a configuration change, not a code change.

- **A pty does not enforce parity.** An 8E1 gateway talking to an 8N1 slave
  works in the three-tier test and would fail completely on real hardware. This
  is exactly the class of fault only a transceiver finds.
- Not verified: PHY-level Ethernet behaviour, real RS-485 electrical timing,
  failsafe biasing, termination, EMI.
- The ARM build is verified as a correct `armhf` binary and runs under
  `qemu-arm`; it has not run on a physical Cortex-A board.

**Coverage percentages read below 100 by design.** `gcov` reports 97.62% and
96.67%, but the uncovered lines are compiler-emitted exception-cleanup code at
`-O0` with no source line to test. `make coverage` therefore counts uncovered
*source* lines, which is 0 for both files. Branch coverage is deliberately not
quoted for C++: every STL call generates exception-unwind branches that never
fire without a throw, so the figure would measure unexercised library code
rather than unexercised logic.

**CI runs on every push.** The workflow in `.github/workflows/ci.yml` runs the
full suite on a clean Ubuntu runner: the routing invariant under
ThreadSanitizer, the same suite under AddressSanitizer and UBSan, line
coverage, static analysis, the ARM Cortex-A cross build, the 100-master soak
and the fault soak. It passes on a runner that has never seen the development
machine, so the results above are independently reproducible rather than
self-reported.
