#pragma once
// ============================================================================
// udp_publisher.hpp — Institutional ITCH-style UDP Multicast Feed
//
// Replicates genuine HFT network topology. Bypasses the uWebSockets TCP 
// stack and completely drops the concept of connection management to 
// blast pure binary out to the local network at sub-microsecond speeds.
//
// Zero allocations. Zero TCP ACKs. Non-blocking.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <span>

#include "network/wire_protocol.hpp"
#include "network/protocol.hpp"

namespace optirisk::network {

class UdpPublisher {
private:
    int sock_ = -1;
    struct sockaddr_in mcast_addr_{};
    bool valid_ = false;

public:
    UdpPublisher(const char* mcast_ip = "239.255.0.1", uint16_t port = 9090) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            std::fprintf(stderr, "[warn] Failed to create UDP Multicast socket\n");
            return;
        }

        std::memset(&mcast_addr_, 0, sizeof(mcast_addr_));
        mcast_addr_.sin_family = AF_INET;
        mcast_addr_.sin_port = htons(port);
        
        if (inet_pton(AF_INET, mcast_ip, &mcast_addr_.sin_addr) <= 0) {
            std::fprintf(stderr, "[warn] Invalid UDP Multicast IP: %s\n", mcast_ip);
            close(sock_);
            sock_ = -1;
            return;
        }

        // Optimize TTL to restrict packets strictly to the local subnet (low hop limit)
        uint8_t ttl = 1;
        if (setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
             std::fprintf(stderr, "[warn] Failed to set multicast TTL, but proceeding.\n");
        }

        // Set socket to Non-Blocking to guarantee Publisher thread never stalls
        int flags = fcntl(sock_, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
        }

        valid_ = true;
        std::printf("[udp] Mcast publisher bound to %s:%u (Non-blocking ITCH feed)\n", mcast_ip, port);
    }

    ~UdpPublisher() {
        if (sock_ >= 0) {
            close(sock_);
        }
    }

    // Explicitly delete copy semantic to prevent socket duplication
    UdpPublisher(const UdpPublisher&) = delete;
    UdpPublisher& operator=(const UdpPublisher&) = delete;

    __attribute__((always_inline))
    inline void broadcast_tick(const TickDelta& tick) noexcept {
        if (!valid_) [[unlikely]] return;

        // No packed attribute: TickDelta is already #pragma pack(1) so its
        // alignment is 1 and no padding can be inserted. GCC rejected the
        // attribute anyway (MessageHeader has a default member initializer,
        // so it is not trivial and the attribute was silently ignored). The
        // static_assert is the guarantee that actually matters.
        struct Frame { MessageHeader hdr; TickDelta payload; };
        static_assert(sizeof(Frame) == sizeof(MessageHeader) + sizeof(TickDelta),
                      "TickDelta UDP frame must have no padding between header and payload");
        Frame buf;
        buf.hdr.msg_type    = MsgType::TickDelta;
        buf.hdr._reserved   = 0;
        // payload_len was never assigned here, so every TickDelta multicast
        // frame carried an indeterminate length field. A receiver using it to
        // size the read would consume the wrong number of bytes.
        buf.hdr.payload_len = static_cast<uint16_t>(sizeof(TickDelta));
        buf.payload = tick;

        sendto(sock_, &buf, sizeof(buf), 0, 
              reinterpret_cast<struct sockaddr*>(&mcast_addr_), sizeof(mcast_addr_));
    }

    __attribute__((always_inline))
    inline void broadcast_var(const VaRReport& report) noexcept {
        if (!valid_) [[unlikely]] return;

        struct Frame { MessageHeader hdr; VaRReport payload; };
        static_assert(sizeof(Frame) == sizeof(MessageHeader) + sizeof(VaRReport),
                      "VaRReport UDP frame must have no padding between header and payload");
        Frame buf;
        buf.hdr.msg_type    = MsgType::VaRReport;
        buf.hdr._reserved   = 0;
        buf.hdr.payload_len = static_cast<uint16_t>(sizeof(VaRReport));
        buf.payload = report;

        sendto(sock_, &buf, sizeof(buf), 0, 
              reinterpret_cast<struct sockaddr*>(&mcast_addr_), sizeof(mcast_addr_));
    }

    __attribute__((always_inline))
    inline void broadcast_bbo(std::span<optirisk::network::BboUpdate> deltas) noexcept {
        if (!valid_ || deltas.empty()) [[unlikely]] return;

        MessageHeader hdr;
        hdr.msg_type = MsgType::BboChange;
        hdr._reserved = 0;
        hdr.payload_len = static_cast<uint16_t>(deltas.size() * sizeof(optirisk::network::BboUpdate));

        struct iovec iov[2];
        iov[0].iov_base = &hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = deltas.data();
        iov[1].iov_len = hdr.payload_len;

        struct msghdr msg{};
        msg.msg_name = &mcast_addr_;
        msg.msg_namelen = sizeof(mcast_addr_);
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;

        sendmsg(sock_, &msg, 0);
    }
};

} // namespace optirisk::network
