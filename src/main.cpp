// ---------------------------------------------------------------------------
// Modbus TCP gateway server.
//
//   ./build/gateway --port 5020 --units 17,34,51 [--drop 0.05] [--seed 1]
//
// Modbus TCP gateway server.
//
//   ./build/gateway --port 5020 --units 17,34,51 [--serial /dev/ttyUSB0]
//
// One thread per connection: a field gateway serves a handful of SCADA masters
// rather than many thousands of sockets, and the blocking loop is simpler to
// reason about and to check under ThreadSanitizer.
//
// The receive path is a stream assembler. TCP does not preserve message
// boundaries, so a master may pipeline requests into one segment or split one
// across several.
// serves a handful of SCADA masters, not ten thousand sockets, and the
// blocking read loop is far easier to reason about and to prove correct under
// ThreadSanitizer. Scaling limits should be a measured decision, not a reflex.
//
// The receive path is a real stream assembler. TCP does not preserve message
// boundaries: a master may pipeline two requests into one segment or split one
// across three. Treating a recv() buffer as exactly one frame is the standard
// bug in hand-written Modbus TCP, and the decoder here is written against that.
// ---------------------------------------------------------------------------

#include "mbap.hpp"
#include "gateway.hpp"
#include "rtu_transport.hpp"
#include "serial_rtu.hpp"
#include "config.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <thread>
#include <vector>
#include <memory>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};
std::atomic<int>  g_listen_fd{-1};

void on_signal(int) {
    g_running = false;
    const int fd = g_listen_fd.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

struct Options {
    std::uint16_t             port = 5020;
    std::vector<std::uint8_t> units{ 0x11, 0x22, 0x33 };
    double                    drop = 0.0;
    double                    corrupt = 0.0;
    unsigned                  seed = 1;
    bool                      quiet = false;
    std::string               config_file;
    std::string               serial_port;      // empty = in-process backend
    unsigned                  baud = 19200;
    unsigned                  resp_timeout_ms = 300;
};

// SIGUSR1 dumps live counters without stopping the gateway: a field unit
// cannot be restarted to answer whether it is reaching the bus.
// cannot be restarted to answer "is it talking to the bus?", so diagnostics
// have to be available while it runs.
std::atomic<bool> g_dump_stats{false};
void on_sigusr1(int) { g_dump_stats = true; }

std::vector<std::uint8_t> parse_units(const std::string& s) {
    std::vector<std::uint8_t> v;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        const auto piece = s.substr(start, comma == std::string::npos
                                           ? std::string::npos : comma - start);
        if (!piece.empty()) v.push_back(static_cast<std::uint8_t>(std::stoi(piece)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return v;
}

void serve_client(int fd, mb::Gateway& gw, mb::GatewayStats& stats, bool quiet)
{
        // TCP_NODELAY: Nagle would coalesce these 12-byte responses and add tens
    // of milliseconds to request latency.
    // milliseconds to request latency on a protocol whose frames are 12 bytes.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::vector<std::uint8_t> buf;      // the stream assembler
    std::uint8_t chunk[1024];

    while (g_running) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;              // peer closed, or error
        buf.insert(buf.end(), chunk, chunk + n);

                // Drain every complete frame in the buffer: a pipelining master may
        // have placed several here at once.
        // master can put several here at once.
        for (;;) {
            auto r = mb::decode(buf.data(), buf.size());

            if (!r.frame) {
                if (r.error == mb::DecodeError::NeedMoreData) break;
                                // A framing error is unrecoverable: the length field is how
                // the next boundary is found, so the stream cannot be
                // resynchronised. Close the connection.
                // find the next frame boundary, so once it is untrustworthy
                // the stream cannot be resynchronised. Close the connection.
                stats.malformed++;
                if (!quiet)
                    std::fprintf(stderr, "  framing error: %s, closing\n",
                                 mb::to_string(r.error));
                ::close(fd);
                return;
            }

            const auto rsp = gw.handle(*r.frame);
            const auto out = mb::encode(rsp);

            std::size_t sent = 0;
            while (sent < out.size()) {
                const ssize_t w = ::send(fd, out.data() + sent,
                                         out.size() - sent, MSG_NOSIGNAL);
                if (w <= 0) { ::close(fd); return; }
                sent += static_cast<std::size_t>(w);
            }
            buf.erase(buf.begin(),
                      buf.begin() + static_cast<std::ptrdiff_t>(r.consumed));
        }
    }
    ::close(fd);
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--port")    opt.port = static_cast<std::uint16_t>(std::stoi(next()));
        else if (a == "--units")   opt.units = parse_units(next());
        else if (a == "--drop")    opt.drop = std::stod(next());
        else if (a == "--corrupt") opt.corrupt = std::stod(next());
        else if (a == "--seed")    opt.seed = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--quiet")   opt.quiet = true;
        else if (a == "--config")  opt.config_file = next();
        else if (a == "--serial")  opt.serial_port = next();
        else if (a == "--baud")    opt.baud = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--timeout") opt.resp_timeout_ms = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--help") {
            std::printf(
              "usage: gateway [options]\n"
              "  --config FILE     load settings from a config file\n"
              "  --port N          TCP listen port (default 5020)\n"
              "  --serial DEV      drive a real serial bus instead of in-process\n"
              "  --baud N          serial baud (default 19200, 8E1)\n"
              "  --timeout MS      RTU response timeout (default 300)\n"
              "  --units A,B,C     in-process slave unit ids\n"
              "  --drop F          inject response loss, 0.0-1.0\n"
              "  --corrupt F       inject bit corruption, 0.0-1.0\n"
              "  --seed N          fault injector seed (replayable)\n"
              "  --quiet           suppress per-connection logging\n"
              "\ndiagnostics: kill -USR1 <pid> dumps live counters\n");
            return 0;
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

        // Config file supplies defaults; a command-line value differing from the
    // built-in default overrides it, so commissioning needs no file edit.
    // that differs from the built-in default wins, so commissioning overrides
    // do not require editing the file.
    if (!opt.config_file.empty()) {
        mb::Config cfg;
        std::string err;
        if (!cfg.load(opt.config_file, err)) {
            std::fprintf(stderr, "config: %s\n", err.c_str());
            return 2;
        }
        if (opt.port == 5020)
            opt.port = static_cast<std::uint16_t>(cfg.num("tcp_port", 5020));
        if (opt.serial_port.empty())
            opt.serial_port = cfg.str("serial_port", "");
        if (opt.baud == 19200)
            opt.baud = cfg.num("baud", 19200);
        if (opt.resp_timeout_ms == 300)
            opt.resp_timeout_ms = cfg.num("response_timeout_ms", 300);
        if (opt.drop == 0.0)    opt.drop    = cfg.real("inject_drop", 0.0);
        if (opt.corrupt == 0.0) opt.corrupt = cfg.real("inject_corrupt", 0.0);
        if (opt.seed == 1)      opt.seed    = cfg.num("inject_seed", 1);
        opt.units = cfg.units("units", opt.units);
        std::fprintf(stderr, "config: loaded %zu settings from %s\n",
                     cfg.all().size(), opt.config_file.c_str());
    }

    std::signal(SIGUSR1, on_sigusr1);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    mb::GatewayStats stats;

    const mb::SerialRtu* serial_ptr = nullptr;
    std::unique_ptr<mb::IRtuTransport> rtu;

    if (!opt.serial_port.empty()) {
        mb::SerialConfig sc;
        sc.port                = opt.serial_port;
        sc.baud                = opt.baud;
        sc.response_timeout_ms = opt.resp_timeout_ms;
        auto s2 = std::make_unique<mb::SerialRtu>(sc);
        serial_ptr = s2.get();
        rtu = std::move(s2);
    } else {
        rtu = std::make_unique<mb::InProcessRtu>(opt.units, 64);
    }
    if (opt.drop > 0.0 || opt.corrupt > 0.0) {
        mb::FaultConfig fc;
        fc.drop_rate = opt.drop; fc.corrupt_rate = opt.corrupt; fc.seed = opt.seed;
        rtu = std::make_unique<mb::FaultyRtu>(std::move(rtu), fc);
    }
    mb::Gateway gw(*rtu, stats);

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { std::perror("socket"); return 1; }
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(opt.port);

    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    if (::listen(lfd, 64) < 0) { std::perror("listen"); return 1; }
    g_listen_fd = lfd;

    std::fprintf(stderr, "gateway on 127.0.0.1:%u", opt.port);
    if (serial_ptr) {
        std::fprintf(stderr, ", serial %s @ %u 8E1", opt.serial_port.c_str(), opt.baud);
    } else {
        std::fprintf(stderr, ", in-process units:");
        for (auto u : opt.units) std::fprintf(stderr, " %u", u);
    }
    if (opt.drop > 0.0 || opt.corrupt > 0.0)
        std::fprintf(stderr, "  [faults drop=%.2f corrupt=%.2f seed=%u]",
                     opt.drop, opt.corrupt, opt.seed);
    std::fprintf(stderr, "\n");

    std::vector<std::thread> workers;
    while (g_running) {
        const int cfd = ::accept(lfd, nullptr, nullptr);

        if (g_dump_stats.exchange(false)) {
            std::fprintf(stderr,
                "\n--- diagnostics ---\n"
                "  tcp   requests=%llu responses=%llu timeouts=%llu "
                "crc_failures=%llu malformed=%llu\n",
                static_cast<unsigned long long>(stats.requests.load()),
                static_cast<unsigned long long>(stats.responses.load()),
                static_cast<unsigned long long>(stats.timeouts.load()),
                static_cast<unsigned long long>(stats.crc_failures.load()),
                static_cast<unsigned long long>(stats.malformed.load()));
            if (serial_ptr) {
                std::fprintf(stderr,
                    "  bus   connected=%s requests=%llu replies=%llu "
                    "timeouts=%llu io_errors=%llu reconnects=%llu\n",
                    serial_ptr->connected() ? "yes" : "no",
                    static_cast<unsigned long long>(serial_ptr->requests()),
                    static_cast<unsigned long long>(serial_ptr->replies()),
                    static_cast<unsigned long long>(serial_ptr->timeouts()),
                    static_cast<unsigned long long>(serial_ptr->io_errors()),
                    static_cast<unsigned long long>(serial_ptr->reconnects()));
            }
            std::fprintf(stderr, "-------------------\n");
        }

        if (cfd < 0) { if (g_running) continue; break; }
        workers.emplace_back(serve_client, cfd, std::ref(gw), std::ref(stats),
                             opt.quiet);
    }

        // Graceful shutdown: in-flight transactions are allowed to finish. Killing
    // sockets mid-transaction leaves masters waiting for their own timeout,
    // which is indistinguishable from a dead gateway.
    // Killing sockets mid-transaction leaves masters waiting for their own
    // timeout, which in a plant looks identical to a dead gateway.
    for (auto& t : workers) if (t.joinable()) t.join();
    if (g_listen_fd >= 0) ::close(lfd);

    std::fprintf(stderr,
        "\nshutdown\n  requests=%llu responses=%llu timeouts=%llu "
        "crc_failures=%llu malformed=%llu\n",
        static_cast<unsigned long long>(stats.requests.load()),
        static_cast<unsigned long long>(stats.responses.load()),
        static_cast<unsigned long long>(stats.timeouts.load()),
        static_cast<unsigned long long>(stats.crc_failures.load()),
        static_cast<unsigned long long>(stats.malformed.load()));
    if (serial_ptr) {
        std::fprintf(stderr,
            "  bus: requests=%llu replies=%llu timeouts=%llu "
            "io_errors=%llu reconnects=%llu\n",
            static_cast<unsigned long long>(serial_ptr->requests()),
            static_cast<unsigned long long>(serial_ptr->replies()),
            static_cast<unsigned long long>(serial_ptr->timeouts()),
            static_cast<unsigned long long>(serial_ptr->io_errors()),
            static_cast<unsigned long long>(serial_ptr->reconnects()));
    }
    return 0;
}
