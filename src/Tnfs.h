#pragma once

// TNFS (Trivial Network File System, FujiNet/8-bit-retro ecosystem) client,
// implementing RemoteFs. Uses TNFS's native UDP transport — a TCP-framed
// variant was tried first (zero ZiFiSock changes needed) but proved unreliable
// against a real public tnfsd (fujinet.pl): one request succeeded, every
// later one timed out, while raw UDP answered correctly 3/3 tries including
// a correct sequence-number echo. UDP is the protocol's real, documented
// transport — so a dropped/garbled packet is normal, not fatal: transact()
// resends the SAME packet (same session id + sequence number — a genuine
// retransmission, not a new request) up to a bounded number of times.
//
// One subtlety from using ZiFiSock's byte-stream sock_recv() for datagrams:
// it has no notion of a UDP frame boundary (+IPD framing is demuxed into a
// plain per-link ring, same as for TCP). In practice this is fine as long as
// at most one reply is ever pending at a time, which holds here — strictly
// one request in flight, no pipelining — except for the one case a
// retransmission inherently creates: the original (merely slow, not lost)
// reply arriving after we've already given up and resent. The response
// parser handles that by matching cmd+seq and discarding anything else
// instead of assuming the next bytes received are necessarily ours.
//
// Unlike Gopher, TNFS paths are genuinely hierarchical and the server is
// stateless about "current directory" — every OPENDIR/STATFILE/OPENFILE
// request carries a full absolute path. cwd()/cwdPath() are therefore plain
// client-side string bookkeeping (like Ftp/Sftp), not a navigation stack.
//
// RP2350 only, behind ZIFI_NET_CLIENT. All calls from the OSD/main thread.

#if !PICO_RP2040 && ZIFI_NET_CLIENT

#include "RemoteFs.h"
#include <string>

class Tnfs : public RemoteFs {
public:
    Tnfs();
    ~Tnfs() override;

    // Opens a UDP "connection" and MOUNTs "/" anonymously. user/pass are ignored
    // in v1 (every public TNFS test server is anonymous-only; wiring real
    // credentials through the MOUNT payload is a trivial follow-up once a
    // server that needs them shows up).
    bool connect(const char* host, uint16_t port, const char* user, const char* pass);

    bool listStream(const std::string& path, RemoteListCb cb, void* ctx) override;
    std::string cacheId() const override { return "tnfs_" + host_; }
    bool cwd(const std::string& path) override;
    std::string cwdPath() const override { return cur_dir; }
    std::string schemeLabel() const override { return "TNFS:"; }
    bool get(const std::string& remote, const std::string& localSdPath, XferProgressCb cb) override;
    bool put(const std::string& localSdPath, const std::string& remote, XferProgressCb cb) override;
    bool remove(const std::string& name, bool isDir) override;
    void disconnect() override;

private:
    bool connected_;
    int link_;              // ZiFiSock link id, -1 when closed
    std::string host_;      // for cacheId() only
    uint16_t port_;
    uint16_t sid_;          // TNFS session id, assigned by MOUNT
    uint8_t seq_;           // packet sequence number, wraps at 256
    std::string cur_dir;    // absolute current directory, always starts with '/'

    uint8_t nextSeq();
    std::string absPath(const std::string& name) const;
    void closeHandle(uint8_t fd);

    // Send one UDP datagram (the whole TNFS packet — no length prefix needed,
    // the datagram boundary IS the packet boundary).
    bool sendPacket(const uint8_t* pkt, size_t len);
    // Receive one UDP datagram into buf (cap bytes max).
    bool recvPacket(uint8_t* buf, size_t cap, size_t& outLen, uint32_t timeout_ms);

    // Build a request (sid_ + seq + cmd + payload) and send/resend it until a
    // reply with matching cmd+seq arrives or the retry budget is exhausted.
    // outSid receives the response header's session id (only meaningful for
    // MOUNT, which assigns a fresh one); outStatus is the TNFS status byte;
    // the response's data bytes (after the 5-byte header) land in outBuf,
    // truncated to outCap.
    bool transact(uint8_t cmd, const uint8_t* payload, size_t plen,
                  uint16_t& outSid, uint8_t& outStatus,
                  uint8_t* outBuf, size_t outCap, size_t& outLen);
};

#endif // !PICO_RP2040 && ZIFI_NET_CLIENT
