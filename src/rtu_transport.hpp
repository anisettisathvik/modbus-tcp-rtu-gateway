#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <random>
#include <mutex>

extern "C" {
#include "modbus_slave.h"
#include "modbus_crc.h"
}

namespace mb {

// ---------------------------------------------------------------------------
// The serial side of the gateway.
//
// The serial side of the gateway.
//
// An interface rather than a concrete port, so a fault injector can sit between
// the gateway and the bus and produce timeouts, corruption and delay
// deterministically by seed.
// in verification: it lets a fault injector sit between the gateway and the
// bus and produce timeouts, corruption and delay on demand, deterministically,
// by seed. On real RS-485 you get whatever faults the day gives you.
// ---------------------------------------------------------------------------

struct RtuStats {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> replies{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> crc_errors{0};
};

class IRtuTransport {
public:
    virtual ~IRtuTransport() = default;
    // One request/response exchange on the bus. Returns false on timeout.
    // req and rsp are complete RTU ADUs including CRC.
    virtual bool transact(const std::vector<std::uint8_t>& req,
                          std::vector<std::uint8_t>&       rsp) = 0;
};

// ---------------------------------------------------------------------------
// In-process backend: drives the exact modbus_slave.c from Project 1.
//
// In-process backend: drives the verified Modbus slave core directly, without
// a serial link. Used where verification speed matters more than realism.
// standalone demo. The RTU endpoint is not a mock; it is the verified slave
// core, running behind the gateway.
// ---------------------------------------------------------------------------
class InProcessRtu : public IRtuTransport {
public:
    explicit InProcessRtu(const std::vector<std::uint8_t>& unit_ids, std::uint16_t regs = 64)
        : regs_per_slave_(regs)
    {
        for (auto id : unit_ids) {
            auto s = std::make_unique<Slave>();
            s->hold.resize(regs);
            mb_slave_init(&s->ctx, id, s->hold.data(), regs);
            for (std::uint16_t i = 0; i < regs; ++i)
                s->hold[i] = static_cast<std::uint16_t>((id << 8) | i);
            slaves_.push_back(std::move(s));
        }
    }

    bool transact(const std::vector<std::uint8_t>& req,
                  std::vector<std::uint8_t>&       rsp) override
    {
        std::lock_guard<std::mutex> lk(bus_);   // RS-485 is one shared bus
        if (req.empty()) return false;

        std::uint8_t resp_buf[MB_ADU_MAX];
        std::uint16_t resp_len = 0;

        for (auto& s : slaves_) {
            const auto r = mb_slave_handle(&s->ctx, req.data(),
                                           static_cast<std::uint16_t>(req.size()),
                                           resp_buf, &resp_len);
            if (r == MB_RESULT_RESPOND) {
                rsp.assign(resp_buf, resp_buf + resp_len);
                return true;
            }
        }
        return false;   // No responder; a real bus would time out here.
    }

    std::uint16_t regs_per_slave() const { return regs_per_slave_; }

private:
    struct Slave {
        mb_slave_t                 ctx{};
        std::vector<std::uint16_t> hold;
    };
    std::vector<std::unique_ptr<Slave>> slaves_;
    std::uint16_t                       regs_per_slave_;
    std::mutex                          bus_;
};

// ---------------------------------------------------------------------------
// Deterministic fault injection.
//
// Deterministic fault injection. Seeded, so any failure replays exactly.
// cannot match: "drop the response to the 4,197th request" is a one-line
// configuration here and essentially unreproducible on hardware.
// ---------------------------------------------------------------------------
struct FaultConfig {
    double   drop_rate    = 0.0;   // response never arrives -> gateway timeout
    double   corrupt_rate = 0.0;   // response arrives with a flipped bit
    unsigned seed         = 1;
};

class FaultyRtu : public IRtuTransport {
public:
    FaultyRtu(std::unique_ptr<IRtuTransport> inner, FaultConfig cfg)
        : inner_(std::move(inner)), cfg_(cfg), rng_(cfg.seed) {}

    bool transact(const std::vector<std::uint8_t>& req,
                  std::vector<std::uint8_t>&       rsp) override
    {
        std::lock_guard<std::mutex> lk(m_);
        std::uniform_real_distribution<double> u(0.0, 1.0);

        if (u(rng_) < cfg_.drop_rate) { dropped_++; return false; }
        if (!inner_->transact(req, rsp)) return false;

        if (!rsp.empty() && u(rng_) < cfg_.corrupt_rate) {
            std::uniform_int_distribution<std::size_t> pick(0, rsp.size() - 1);
            rsp[pick(rng_)] ^= 0x01;   // Single bit flip; the CRC must catch it.
            corrupted_++;
        }
        return true;
    }

    std::uint64_t dropped()   const { return dropped_; }
    std::uint64_t corrupted() const { return corrupted_; }

private:
    std::unique_ptr<IRtuTransport> inner_;
    FaultConfig                    cfg_;
    std::mt19937                   rng_;
    std::mutex                     m_;
    std::uint64_t                  dropped_{0};
    std::uint64_t                  corrupted_{0};
};

} // namespace mb
