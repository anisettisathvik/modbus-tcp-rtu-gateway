#pragma once
#include "rtu_transport.hpp"

#include <string>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

namespace mb {

// ---------------------------------------------------------------------------
// The real serial side of the gateway.
//
// Serial transport for the RTU side of the gateway.
//
// Drives a real POSIX serial line, so the gateway can front a Modbus RTU slave
// running as a separate process. Handles three conditions a field gateway must
// survive: the serial device disappearing and returning, a device answering
// after the master has given up, and a partial frame followed by silence.
// the gateway can sit in front of the Project 1 slave running as a separate
// process on the other end of a tty. Swapping one for the other is a
// constructor argument — that is what the IRtuTransport seam was for.
//
// Three things a field gateway must survive and a demo usually ignores:
//   * the serial device disappearing (USB adapter unplugged) and coming back
//   * a device answering late, after the master has already given up
//   * a partial frame arriving and then silence
// ---------------------------------------------------------------------------

struct SerialConfig {
    std::string port          = "/dev/ttyUSB0";
    unsigned    baud          = 19200;
    unsigned    response_timeout_ms = 300;  // how long to wait for a reply
    unsigned    frame_gap_ms        = 5;    // inactivity that ends a frame
    unsigned    reconnect_backoff_ms = 500;
};

class SerialRtu : public IRtuTransport {
public:
    explicit SerialRtu(SerialConfig cfg) : cfg_(std::move(cfg)) {}
    ~SerialRtu() override { close_port(); }

    bool transact(const std::vector<std::uint8_t>& req,
                  std::vector<std::uint8_t>&       rsp) override
    {
        std::lock_guard<std::mutex> lk(bus_);
        if (!ensure_open()) return false;

        // Discard stale input. A late reply to the previous request would
        // otherwise be read as the answer to this one.
        ::tcflush(fd_, TCIFLUSH);

        std::size_t written = 0;
        while (written < req.size()) {
            const ssize_t w = ::write(fd_, req.data() + written, req.size() - written);
            if (w <= 0) { io_errors_++; close_port(); return false; }
            written += static_cast<std::size_t>(w);
        }
        ::tcdrain(fd_);          // block until the last bit is on the wire
        requests_++;

        rsp.clear();
        std::uint8_t chunk[512];
        bool got_any = false;

        for (;;) {
                        // Full response timeout before the first byte; afterwards a short
            // gap ends the frame -- the software stand-in for the 3.5-character
            // rule, which needs a hardware timer to measure properly.
            // it, a short gap means the frame is complete — the software
            // stand-in for the 3.5-character rule.
            const int wait_ms = got_any ? static_cast<int>(cfg_.frame_gap_ms)
                                        : static_cast<int>(cfg_.response_timeout_ms);
            struct pollfd p { fd_, POLLIN, 0 };
            const int pr = ::poll(&p, 1, wait_ms);

            if (pr < 0) {
                if (errno == EINTR) continue;
                io_errors_++; close_port(); return false;
            }
            if (pr == 0) break;                       // timeout or frame gap

            const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
            if (n < 0) { io_errors_++; close_port(); return false; }
            if (n == 0) break;

            rsp.insert(rsp.end(), chunk, chunk + n);
            got_any = true;
            if (rsp.size() >= MB_ADU_MAX) break;
        }

        if (rsp.empty()) { timeouts_++; return false; }
        replies_++;
        return true;
    }

    std::uint64_t requests()    const { return requests_; }
    std::uint64_t replies()     const { return replies_; }
    std::uint64_t timeouts()    const { return timeouts_; }
    std::uint64_t io_errors()   const { return io_errors_; }
    std::uint64_t reconnects()  const { return reconnects_; }
    bool          connected()   const { return fd_ >= 0; }

private:
    static speed_t to_speed(unsigned baud)
    {
        switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B19200;
        }
    }

    bool ensure_open()
    {
        if (fd_ >= 0) return true;

                // Back off between reconnect attempts: without this an unplugged
        // adapter becomes a busy loop that starves the rest of the gateway.
        // this, an unplugged adapter turns into a busy loop that starves the
        // rest of the gateway.
        const auto now = std::chrono::steady_clock::now();
        if (last_attempt_.time_since_epoch().count() != 0) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - last_attempt_).count();
            if (since < static_cast<long>(cfg_.reconnect_backoff_ms)) return false;
        }
        last_attempt_ = now;

        const int fd = ::open(cfg_.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) return false;

        struct termios tio {};
        if (::tcgetattr(fd, &tio) != 0) { ::close(fd); return false; }
        ::cfmakeraw(&tio);
        ::cfsetispeed(&tio, to_speed(cfg_.baud));
        ::cfsetospeed(&tio, to_speed(cfg_.baud));

                // Modbus RTU's default framing is 8E1; 8N1 is legal only with two stop
        // bits. A mismatch leaves the line apparently alive with every frame
        // failing CRC.
        // two stop bits. Getting this wrong is the most common interop failure
        // on a real bus — the line looks alive and every frame fails CRC.
        tio.c_cflag |= (CLOCAL | CREAD | PARENB);
        tio.c_cflag &= ~static_cast<tcflag_t>(PARODD);   // even parity
        tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);   // one stop bit
        tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
        tio.c_cflag |= CS8;
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;                             // poll() owns timing

        if (::tcsetattr(fd, TCSANOW, &tio) != 0) { ::close(fd); return false; }

                // poll() provides the timeout control, so blocking mode is fine here.
        // the timeout control we need without it.
        const int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

        fd_ = fd;
        reconnects_++;
        return true;
    }

    void close_port()
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    SerialConfig cfg_;
    int          fd_ = -1;
    std::mutex   bus_;
    std::chrono::steady_clock::time_point last_attempt_{};

    std::atomic<std::uint64_t> requests_{0}, replies_{0},
                               timeouts_{0}, io_errors_{0}, reconnects_{0};
};

} // namespace mb
