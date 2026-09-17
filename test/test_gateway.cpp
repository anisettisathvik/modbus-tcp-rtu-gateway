// ---------------------------------------------------------------------------
// Gateway verification.
//
// Gateway verification: MBAP codec and stream framing, TCP/RTU translation
// against the real slave core, the routing invariant under concurrency, and
// behaviour under injected bus faults.
//
// Built with -fsanitize=thread for the concurrency tests.
//   1. MBAP codec unit tests, including the stream-framing cases that break
//      hand-written Modbus TCP implementations
//   2. Bridge translation tests against the real Project 1 slave
//   3. The routing invariant under concurrency, which is the whole point:
//      no response may ever reach the wrong master
//
// Built with -fsanitize=thread for layer 3.
// ---------------------------------------------------------------------------

#include "mbap.hpp"
#include "gateway.hpp"
#include "rtu_transport.hpp"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <string>

static int g_pass = 0, g_fail = 0;

static void case_check(const std::string& name, bool ok);
static void check(const std::string& name, bool ok)
{
    if (ok) { g_pass++; std::printf("  PASS  %s\n", name.c_str()); }
    else    { g_fail++; std::printf("  FAIL  %s\n", name.c_str()); }
}

// ---------------------------------------------------------------------------

static void test_mbap_codec()
{
    std::printf("\n=== MBAP codec ===\n");

    mb::MbapFrame f;
    f.transaction_id = 0x1234;
    f.unit_id        = 0x11;
    f.pdu            = { 0x03, 0x00, 0x00, 0x00, 0x02 };

    auto wire = mb::encode(f);
    check("encoded length is 7 + pdu", wire.size() == 12);
    check("transaction id big-endian", wire[0] == 0x12 && wire[1] == 0x34);
    check("protocol id is zero",       wire[2] == 0x00 && wire[3] == 0x00);
    // length covers unit_id + pdu = 1 + 5 = 6
    check("length field counts unit+pdu", wire[4] == 0x00 && wire[5] == 0x06);
    check("unit id in byte 6",         wire[6] == 0x11);

    auto r = mb::decode(wire.data(), wire.size());
    check("round trip decodes",        r.frame.has_value());
    check("round trip is identical",   r.frame && *r.frame == f);
    check("consumed the whole frame",  r.consumed == wire.size());

        // Stream framing cases.
    {
        // one byte short
        auto part = wire; part.pop_back();
        auto rp = mb::decode(part.data(), part.size());
        check("partial frame -> need more data",
              !rp.frame && rp.error == mb::DecodeError::NeedMoreData);
    }
    {
        // header not yet complete
        auto rp = mb::decode(wire.data(), 3);
        check("3-byte prefix -> need more data",
              !rp.frame && rp.error == mb::DecodeError::NeedMoreData);
    }
    {
                // Two requests in one TCP segment.
        std::vector<std::uint8_t> two = wire;
        mb::MbapFrame g = f; g.transaction_id = 0x5678;
        auto w2 = mb::encode(g);
        two.insert(two.end(), w2.begin(), w2.end());

        auto r1 = mb::decode(two.data(), two.size());
        check("first of two frames decodes", r1.frame && r1.consumed == wire.size());
        auto r2 = mb::decode(two.data() + r1.consumed, two.size() - r1.consumed);
        check("second frame decodes too",
              r2.frame && r2.frame->transaction_id == 0x5678);
    }
    {
                // Trailing garbage must not corrupt a valid leading frame.
        auto plus = wire;
        plus.insert(plus.end(), { 0xFF, 0xFF, 0xFF });
        auto rp = mb::decode(plus.data(), plus.size());
        check("valid frame ahead of garbage still decodes",
              rp.frame && rp.consumed == wire.size());
    }
    {
        auto bad = wire; bad[2] = 0x01;      // protocol id != 0
        auto rp = mb::decode(bad.data(), bad.size());
        check("nonzero protocol id rejected",
              !rp.frame && rp.error == mb::DecodeError::BadProtocolId);
    }
    {
        auto bad = wire; bad[4] = 0x00; bad[5] = 0x01;   // length = 1
        auto rp = mb::decode(bad.data(), bad.size());
        check("length below minimum rejected",
              !rp.frame && rp.error == mb::DecodeError::LengthTooSmall);
    }
    {
        auto bad = wire; bad[4] = 0xFF; bad[5] = 0xFF;   // length = 65535
        auto rp = mb::decode(bad.data(), bad.size());
        check("length above maximum rejected",
              !rp.frame && rp.error == mb::DecodeError::LengthTooLarge);
    }
}

// ---------------------------------------------------------------------------

static void test_translation()
{
    std::printf("\n=== TCP <-> RTU translation ===\n");

    mb::MbapFrame f;
    f.transaction_id = 0xABCD;
    f.unit_id        = 0x11;
    f.pdu            = { 0x03, 0x00, 0x00, 0x00, 0x02 };

    auto rtu = mb::tcp_to_rtu(f);
    check("rtu frame is unit + pdu + crc", rtu.size() == 1 + f.pdu.size() + 2);
    check("unit id became the slave address", rtu[0] == 0x11);
    check("rtu crc is self-consistent",
          mb_crc16(rtu.data(), static_cast<std::uint16_t>(rtu.size())) == 0);

    auto back = mb::rtu_to_tcp(rtu, 0xABCD);
    check("rtu converts back to tcp", back.has_value());
    check("transaction id is reattached", back && back->transaction_id == 0xABCD);
    check("pdu survives the round trip",  back && back->pdu == f.pdu);

    {   // A corrupted RTU response must never be forwarded: TCP carries no
        // checksum, so the master would accept the bad payload as valid.
        auto bad = rtu; bad[2] ^= 0x01;
        check("corrupted rtu response is rejected",
              !mb::rtu_to_tcp(bad, 0xABCD).has_value());
    }
}

// ---------------------------------------------------------------------------

static void test_gateway_paths()
{
    std::printf("\n=== gateway behaviour ===\n");

    mb::GatewayStats stats;
    mb::InProcessRtu rtu({ 0x11, 0x22 }, 64);
    mb::Gateway gw(rtu, stats);

    {   // normal read against the real Project 1 slave
        mb::MbapFrame req;
        req.transaction_id = 1; req.unit_id = 0x11;
        req.pdu = { 0x03, 0x00, 0x00, 0x00, 0x02 };
        auto rsp = gw.handle(req);
        check("read returns a normal response",
              rsp.pdu.size() >= 2 && (rsp.pdu[0] & 0x80) == 0);
        check("response echoes the transaction id", rsp.transaction_id == 1);
        check("byte count is 4 for two registers", rsp.pdu[1] == 4);
    }
    {   // the slave itself raises an exception, and it must pass through
        mb::MbapFrame req;
        req.transaction_id = 2; req.unit_id = 0x11;
        req.pdu = { 0x03, 0x00, 0x00, 0xFF, 0xFF };   // absurd quantity
        auto rsp = gw.handle(req);
        check("slave exception is forwarded unchanged",
              rsp.pdu.size() == 2 && rsp.pdu[0] == 0x83 && rsp.pdu[1] == 0x03);
    }
    {   // Empty PDU -> exception 0x0A, gateway path unavailable.
        mb::MbapFrame req;
        req.transaction_id = 10; req.unit_id = 0x11;
        req.pdu.clear();
        auto rsp = gw.handle(req);
        case_check("empty pdu -> exception 0x0A",
              rsp.pdu.size() == 2 &&
              rsp.pdu[1] == mb::EX_GATEWAY_PATH_UNAVAILABLE);
        case_check("empty pdu still echoes the transaction id",
              rsp.transaction_id == 10);
    }
    {   // Decode error strings: used by the server's framing-error log path,
        // so the test binary never reached them.
        bool named = true;
        const mb::DecodeError all[] = {
            mb::DecodeError::NeedMoreData, mb::DecodeError::BadProtocolId,
            mb::DecodeError::LengthTooSmall, mb::DecodeError::LengthTooLarge };
        for (auto e : all) {
            const char* n = mb::to_string(e);
            if (!n || n[0] == '\0') named = false;
        }
        case_check("every decode error has a message", named);
                // The trailing return after an exhaustive switch guards against a
        // value that is not any declared enumerator.
        // value that is not any declared enumerator - what a corrupted stack
        // or a future enumerator would produce. Unreachable through the API,
        // which is why it needs a test rather than an exemption. Same
        // reasoning as the controller's corrupted-state test.
        case_check("unknown decode error is guarded",
              std::string(mb::to_string(static_cast<mb::DecodeError>(99)))
                  == "unknown");
    }
    {   // no such device on the bus
        mb::MbapFrame req;
        req.transaction_id = 3; req.unit_id = 0x77;
        req.pdu = { 0x03, 0x00, 0x00, 0x00, 0x01 };
        auto rsp = gw.handle(req);
        check("absent device -> exception 0x0B",
              rsp.pdu.size() == 2 &&
              rsp.pdu[1] == mb::EX_GATEWAY_TARGET_NO_RESPONSE);
        check("gateway never goes silent", rsp.transaction_id == 3);
    }
}

// ---------------------------------------------------------------------------
// Routing invariant: no response may reach the wrong master.
// ---------------------------------------------------------------------------

static void test_routing_invariant(int n_threads, int per_thread)
{
    std::printf("\n=== routing invariant under concurrency ===\n");
    std::printf("  %d masters x %d transactions each\n", n_threads, per_thread);

    mb::GatewayStats stats;
    mb::InProcessRtu rtu({ 0x11, 0x22, 0x33, 0x44 }, 64);
    mb::Gateway gw(rtu, stats);

    std::atomic<std::uint64_t> misrouted{0};
    std::atomic<std::uint64_t> wrong_unit{0};
    std::atomic<std::uint64_t> wrong_data{0};
    std::atomic<std::uint64_t> ok{0};

    const std::uint8_t units[4] = { 0x11, 0x22, 0x33, 0x44 };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n_threads));

    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 13u);
            for (int i = 0; i < per_thread; ++i) {
                                // Transaction id unique per master, so a crossed response is
                // detectable rather than merely suspicious.
                // detectable rather than merely suspicious
                const std::uint16_t txn =
                    static_cast<std::uint16_t>((t << 10) | (i & 0x3FF));
                const std::uint8_t  unit = units[rng() % 4];
                const std::uint16_t reg  = static_cast<std::uint16_t>(rng() % 60);

                mb::MbapFrame req;
                req.transaction_id = txn;
                req.unit_id        = unit;
                req.pdu = { 0x03,
                            static_cast<std::uint8_t>(reg >> 8),
                            static_cast<std::uint8_t>(reg & 0xFF),
                            0x00, 0x01 };

                auto rsp = gw.handle(req);

                if (rsp.transaction_id != txn) { misrouted++; continue; }
                if (rsp.unit_id != unit)       { wrong_unit++; continue; }

                                // InProcessRtu seeds register i of slave u to (u<<8)|i, so the
                // returned value proves the request reached the right device at
                // the right address, not merely that something replied.
                // returned value proves the request reached the right device
                // AND the right address, not merely that something replied.
                if (rsp.pdu.size() == 4 && (rsp.pdu[0] & 0x80) == 0) {
                    const std::uint16_t got =
                        static_cast<std::uint16_t>((rsp.pdu[2] << 8) | rsp.pdu[3]);
                    const std::uint16_t want =
                        static_cast<std::uint16_t>((unit << 8) | reg);
                    if (got != want) { wrong_data++; continue; }
                }
                ok++;
            }
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t total =
        static_cast<std::uint64_t>(n_threads) * static_cast<std::uint64_t>(per_thread);

    std::printf("  transactions ........... %llu\n",
                static_cast<unsigned long long>(total));
    std::printf("  correct ................ %llu\n",
                static_cast<unsigned long long>(ok.load()));
    std::printf("  wrong transaction id ... %llu\n",
                static_cast<unsigned long long>(misrouted.load()));
    std::printf("  wrong unit id .......... %llu\n",
                static_cast<unsigned long long>(wrong_unit.load()));
    std::printf("  wrong register data .... %llu\n",
                static_cast<unsigned long long>(wrong_data.load()));

    check("no response reached the wrong master", misrouted == 0);
    check("no response carried the wrong unit id", wrong_unit == 0);
    check("no response carried another device's data", wrong_data == 0);
    check("every transaction accounted for", ok.load() == total);
}

// ---------------------------------------------------------------------------

static void test_fault_injection()
{
    std::printf("\n=== deterministic fault injection ===\n");

    mb::GatewayStats stats;
    auto inner = std::make_unique<mb::InProcessRtu>(
        std::vector<std::uint8_t>{ 0x11 }, 64);
    mb::FaultConfig cfg;
    cfg.drop_rate    = 0.10;   // one response in ten never arrives
    cfg.corrupt_rate = 0.10;   // one in ten arrives with a flipped bit
    cfg.seed         = 0xC0FFEE;

    auto faulty = std::make_unique<mb::FaultyRtu>(std::move(inner), cfg);
    auto* fp = faulty.get();
    mb::Gateway gw(*faulty, stats);

    const int N = 20000;
    std::uint64_t normal = 0, gw_exception = 0, forwarded_bad = 0;

    for (int i = 0; i < N; ++i) {
        mb::MbapFrame req;
        req.transaction_id = static_cast<std::uint16_t>(i);
        req.unit_id        = 0x11;
        req.pdu = { 0x03, 0x00, 0x00, 0x00, 0x01 };

        auto rsp = gw.handle(req);
        if (rsp.transaction_id != req.transaction_id) { forwarded_bad++; continue; }

        if (rsp.pdu.size() >= 2 && (rsp.pdu[0] & 0x80)) {
            if (rsp.pdu[1] == mb::EX_GATEWAY_TARGET_NO_RESPONSE) gw_exception++;
        } else {
                        // A normal response must carry the correct seeded value.
            if (rsp.pdu.size() == 4) {
                const std::uint16_t got =
                    static_cast<std::uint16_t>((rsp.pdu[2] << 8) | rsp.pdu[3]);
                if (got == 0x1100) normal++;
                else forwarded_bad++;
            } else {
                forwarded_bad++;
            }
        }
    }

    std::printf("  transactions ........... %d\n", N);
    std::printf("  injected drops ......... %llu\n",
                static_cast<unsigned long long>(fp->dropped()));
    std::printf("  injected corruptions ... %llu\n",
                static_cast<unsigned long long>(fp->corrupted()));
    std::printf("  clean responses ........ %llu\n",
                static_cast<unsigned long long>(normal));
    std::printf("  gateway exceptions ..... %llu\n",
                static_cast<unsigned long long>(gw_exception));
    std::printf("  corrupt data forwarded . %llu\n",
                static_cast<unsigned long long>(forwarded_bad));
    std::printf("  stats: timeouts=%llu crc_failures=%llu\n",
                static_cast<unsigned long long>(stats.timeouts.load()),
                static_cast<unsigned long long>(stats.crc_failures.load()));

        // Safety property: under injected loss and corruption, no bad payload may
    // reach a master. Every fault must become an explicit exception.
    // payload may reach a master. Every fault must become an exception.
    check("no corrupted payload was ever forwarded", forwarded_bad == 0);
    check("every fault became a gateway exception",
          gw_exception == fp->dropped() + fp->corrupted());
    check("all transactions accounted for",
          normal + gw_exception == static_cast<std::uint64_t>(N));
}

// ---------------------------------------------------------------------------

static void case_check(const std::string& name, bool ok) { check(name, ok); }

int main(int argc, char** argv)
{
    int threads = 16, per = 5000;
    if (argc >= 2) threads = std::atoi(argv[1]);
    if (argc >= 3) per     = std::atoi(argv[2]);

    std::printf("\n==============================================\n");
    std::printf(" Modbus TCP/RTU gateway verification\n");
    std::printf("==============================================\n");

    test_mbap_codec();
    test_translation();
    test_gateway_paths();
    test_routing_invariant(threads, per);
    test_fault_injection();

    std::printf("\n---------------------------------------\n");
    std::printf("  %d passed, %d failed\n", g_pass, g_fail);
    std::printf("---------------------------------------\n\n");
    return g_fail == 0 ? 0 : 1;
}
