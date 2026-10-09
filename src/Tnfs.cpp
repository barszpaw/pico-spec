#include "Tnfs.h"

#if !PICO_RP2040 && ZIFI_NET_CLIENT

#include "ZiFiSock.h"
#include "Debug.h"
#include "ff.h"
#include <pico/time.h>
#include <string.h>

// ── Protocol constants (verified against a real tnfsd, both wire format and
// byte-for-byte against a working Python TNFS client/server pair) ──────────
enum {
    TNFS_MOUNT      = 0x00,
    TNFS_UMOUNT     = 0x01,
    TNFS_CLOSEDIR   = 0x12,
    TNFS_RMDIR      = 0x14,
    TNFS_OPENDIRX   = 0x17,
    TNFS_READDIRX   = 0x18,
    TNFS_OPENFILE   = 0x29,
    TNFS_READBLOCK  = 0x21,
    TNFS_WRITEBLOCK = 0x22,
    TNFS_CLOSEFILE  = 0x23,
    TNFS_UNLINKFILE = 0x26,
};
static const uint8_t  TNFS_STATUS_EOF = 0x21;
static const uint16_t TNFS_FLAG_RDONLY = 0x0001;
static const uint16_t TNFS_FLAG_WRONLY = 0x0002;
static const uint16_t TNFS_FLAG_CREAT  = 0x0100;
static const uint16_t TNFS_FLAG_TRUNC  = 0x0200;
static const uint16_t TNFS_DEFAULT_PORT = 16384;
static const size_t   TNFS_MAX_BLOCK = 512;   // historical TNFS block size
// Largest frame either direction: 4-byte header + up to WRITEBLOCK/READBLOCK's
// fd/count + TNFS_MAX_BLOCK data (519 bytes) — rounded up for margin.
static const size_t   TNFS_MAX_PKT = 540;

Tnfs::Tnfs() : connected_(false), link_(-1), port_(0), sid_(0), seq_(0), cur_dir("/") {}
Tnfs::~Tnfs() { disconnect(); }

uint8_t Tnfs::nextSeq() { seq_ = (uint8_t)(seq_ + 1); return seq_; }

std::string Tnfs::absPath(const std::string& name) const {
    if (!name.empty() && name[0] == '/') return name; // already absolute
    if (cur_dir == "/") return "/" + name;
    return cur_dir + "/" + name;
}

bool Tnfs::sendPacket(const uint8_t* pkt, size_t len) {
    return ZiFiSock::sock_send(link_, pkt, len, 5000) == (int)len;
}

bool Tnfs::recvPacket(uint8_t* buf, size_t cap, size_t& outLen, uint32_t timeout_ms) {
    int n = ZiFiSock::sock_recv(link_, buf, cap, timeout_ms);
    if (n <= 0) return false;
    outLen = (size_t)n;
    return true;
}

bool Tnfs::transact(uint8_t cmd, const uint8_t* payload, size_t plen,
                     uint16_t& outSid, uint8_t& outStatus,
                     uint8_t* outBuf, size_t outCap, size_t& outLen) {
    if (link_ < 0) return false;
    uint8_t sq = nextSeq();
    uint8_t pkt[TNFS_MAX_PKT];
    if (4 + plen > sizeof(pkt)) return false;
    pkt[0] = (uint8_t)(sid_ & 0xFF);
    pkt[1] = (uint8_t)((sid_ >> 8) & 0xFF);
    pkt[2] = sq;
    pkt[3] = cmd;
    if (plen) memcpy(pkt + 4, payload, plen);

    // UDP: no delivery guarantee either way. Each attempt (re)sends the exact
    // same packet (same seq — a retransmission, not a new request) and waits
    // up to RETRY_TIMEOUT_MS; within that window, discard anything that isn't
    // a matching cmd+seq reply (a stray duplicate from an earlier retransmit)
    // and keep waiting rather than treating it as this attempt's answer.
    static const int MAX_RETRIES = 5;
    static const uint32_t RETRY_TIMEOUT_MS = 2000;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        if (!sendPacket(pkt, 4 + plen)) return false;
        absolute_time_t deadline = make_timeout_time_ms(RETRY_TIMEOUT_MS);
        for (;;) {
            if (time_reached(deadline)) break; // nothing matching → next attempt (resend)
            uint8_t rbuf[TNFS_MAX_PKT]; size_t rlen;
            if (!recvPacket(rbuf, sizeof(rbuf), rlen, RETRY_TIMEOUT_MS)) break;
            if (rlen < 4) continue;
            uint8_t r_seq = rbuf[2], r_cmd = rbuf[3];
            if (r_cmd != cmd || r_seq != sq) continue; // stray/duplicate — keep waiting
            outSid = (uint16_t)(rbuf[0] | ((uint16_t)rbuf[1] << 8));
            outStatus = (rlen >= 5) ? rbuf[4] : 0;
            size_t dataLen = (rlen > 5) ? rlen - 5 : 0;
            if (dataLen > outCap) dataLen = outCap;
            if (dataLen) memcpy(outBuf, rbuf + 5, dataLen);
            outLen = dataLen;
            return true;
        }
    }
    return false;
}

bool Tnfs::connect(const char* host, uint16_t port, const char*, const char*) {
    host_ = host ? host : "";
    port_ = port ? port : TNFS_DEFAULT_PORT;
    if (!ZiFiSock::begin(true)) { Debug::log2SD("Tnfs::connect: ZiFiSock::begin failed"); return false; }
    link_ = ZiFiSock::sock_open_udp(host_.c_str(), port_, 8000);
    if (link_ < 0) { Debug::log2SD("Tnfs::connect: sock_open_udp(%s,%u) failed", host_.c_str(), (unsigned)port_); return false; }

    // MOUNT payload: version(2LE) + retry_ms hint(2LE, 0=server default) +
    // mount path + NUL + username + NUL + password + NUL. Anonymous: empty
    // user/pass (just their NUL terminators).
    uint8_t payload[8];
    size_t n = 0;
    payload[n++] = 0x00; payload[n++] = 0x01; // version 0x0100
    payload[n++] = 0x00; payload[n++] = 0x00; // retry hint
    payload[n++] = '/';  payload[n++] = 0;    // mount path "/"
    payload[n++] = 0;                         // user ""
    payload[n++] = 0;                         // pass ""

    sid_ = 0; seq_ = 0;
    uint16_t rsid; uint8_t status; uint8_t resp[8]; size_t rlen;
    bool ok = transact(TNFS_MOUNT, payload, n, rsid, status, resp, sizeof(resp), rlen);
    if (ok) Debug::log2SD("Tnfs::connect: MOUNT %s link=%d replied status=0x%02x rlen=%u",
                           host_.c_str(), link_, status, (unsigned)rlen);
    else    Debug::log2SD("Tnfs::connect: MOUNT %s link=%d NO REPLY (all retries exhausted)",
                           host_.c_str(), link_);
    if (!ok || status != 0) {
        ZiFiSock::sock_close(link_); link_ = -1; return false;
    }
    sid_ = rsid;
    cur_dir = "/";
    connected_ = true;
    return true;
}

bool Tnfs::cwd(const std::string& path) {
    if (path == "..") {
        if (cur_dir != "/") {
            size_t sl = cur_dir.find_last_of('/');
            cur_dir = (sl == 0) ? "/" : cur_dir.substr(0, sl);
        }
        return true;
    }
    if (path.empty() || path == "/") { cur_dir = "/"; return true; }
    cur_dir = absPath(path);
    return true;
}

bool Tnfs::listStream(const std::string& path, RemoteListCb cb, void* ctx) {
    if (!connected_) return false;
    if (!path.empty() && path != cur_dir) { if (!cwd(path)) return false; }

    // OPENDIRX/READDIRX (not the plain OPENDIR/READDIR+STATFILE-per-entry dance)
    // — type/size come back INLINE, batched, so a directory with N entries costs
    // ~2-4 round trips total instead of 2N+2. The old per-entry STATFILE design
    // was fine in isolation, but over the slow ESP-UART bridge, and especially
    // since F5's "reopen last location" silently reconnects+relists on every F5
    // press (not just when the user picks this remote again), N+1 round trips
    // per listing read as a dead hang rather than a slow directory.
    uint8_t opayload[4 + 1 + 256];
    size_t n = 0;
    opayload[n++] = 0; // diropts: default (show folders, skip hidden)
    opayload[n++] = 0; // sortopts: default (name, case-insensitive, folders first)
    opayload[n++] = 0; opayload[n++] = 0; // maxresults = 0 (no limit)
    opayload[n++] = 0; // pattern = "" (match everything)
    if (n + cur_dir.size() + 1 > sizeof(opayload)) return false;
    memcpy(opayload + n, cur_dir.c_str(), cur_dir.size() + 1); n += cur_dir.size() + 1;

    uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
    if (!transact(TNFS_OPENDIRX, opayload, n, sid, status, resp, sizeof(resp), rlen) ||
        status != 0 || rlen < 1)
        return false;
    uint8_t handle = resp[0];

    bool eof = false;
    while (!eof) {
        uint8_t rpayload[2] = { handle, 0 }; // req_count=0 → as many as fit in one response
        uint8_t rbuf[516]; size_t rrlen; uint8_t rstatus; uint16_t rsid;
        if (!transact(TNFS_READDIRX, rpayload, 2, rsid, rstatus, rbuf, sizeof(rbuf), rrlen))
            break;
        if (rstatus == TNFS_STATUS_EOF) break;
        if (rstatus != 0 || rrlen < 4) break;
        uint8_t count = rbuf[0];
        eof = (rbuf[1] & 0x01) != 0;
        size_t off = 4;
        for (uint8_t i = 0; i < count; i++) {
            if (off + 13 >= rrlen) break; // truncated entry — stop safely, don't misparse
            uint8_t flags = rbuf[off];
            uint32_t size = (uint32_t)rbuf[off + 1] | ((uint32_t)rbuf[off + 2] << 8) |
                            ((uint32_t)rbuf[off + 3] << 16) | ((uint32_t)rbuf[off + 4] << 24);
            off += 13; // flags(1) + size(4) + mtime(4) + ctime(4)
            const char* name = (const char*)rbuf + off;
            size_t avail = rrlen - off;
            size_t nlen = 0;
            while (nlen < avail && name[nlen] != '\0') nlen++;
            if (nlen >= avail) break; // no NUL terminator in the buffer — malformed, stop safely
            off += nlen + 1;
            if (name[0] && strcmp(name, ".") && strcmp(name, "..")) {
                cb(ctx, name, (flags & 0x01) != 0, size); // bit0 = TNFS_DIRENTRY_DIR
            }
        }
    }
    uint16_t csid; uint8_t cstatus; uint8_t cresp[4]; size_t clen;
    transact(TNFS_CLOSEDIR, &handle, 1, csid, cstatus, cresp, sizeof(cresp), clen); // best-effort
    return true;
}

void Tnfs::closeHandle(uint8_t fd) {
    uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
    transact(TNFS_CLOSEFILE, &fd, 1, sid, status, resp, sizeof(resp), rlen); // best-effort
}

bool Tnfs::get(const std::string& remote, const std::string& localSdPath, XferProgressCb cb) {
    if (!connected_) return false;
    std::string full = absPath(remote);

    uint8_t payload[4 + 256];
    size_t n = 0;
    payload[n++] = (uint8_t)(TNFS_FLAG_RDONLY & 0xFF); payload[n++] = (uint8_t)((TNFS_FLAG_RDONLY >> 8) & 0xFF);
    payload[n++] = 0; payload[n++] = 0; // mode, unused for read
    if (n + full.size() + 1 > sizeof(payload)) return false;
    memcpy(payload + n, full.c_str(), full.size() + 1); n += full.size() + 1;

    uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
    if (!transact(TNFS_OPENFILE, payload, n, sid, status, resp, sizeof(resp), rlen) || status != 0 || rlen < 1)
        return false;
    uint8_t fd = resp[0];

    FIL* f = fopen2(localSdPath.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (!f) { closeHandle(fd); return false; }

    uint32_t total = 0; bool ok = true;
    for (;;) {
        uint8_t rpayload[3] = { fd, (uint8_t)(TNFS_MAX_BLOCK & 0xFF), (uint8_t)((TNFS_MAX_BLOCK >> 8) & 0xFF) };
        uint8_t rbuf[2 + TNFS_MAX_BLOCK]; size_t rrlen; uint8_t rstatus; uint16_t rsid;
        if (!transact(TNFS_READBLOCK, rpayload, sizeof(rpayload), rsid, rstatus, rbuf, sizeof(rbuf), rrlen)) {
            ok = false; break;
        }
        if (rstatus == TNFS_STATUS_EOF) break;
        if (rstatus != 0 || rrlen < 2) { ok = false; break; }
        uint16_t got = (uint16_t)(rbuf[0] | (rbuf[1] << 8));
        if (got == 0) break;
        UINT bw = 0;
        if (f_write(f, rbuf + 2, got, &bw) != FR_OK || bw != got) { ok = false; break; }
        total += bw;
        if (cb && !cb(total, 0)) { ok = false; break; } // 0 = unknown total; user aborted
    }
    fclose2(f);
    closeHandle(fd);
    return ok && total > 0;
}

bool Tnfs::put(const std::string& localSdPath, const std::string& remote, XferProgressCb cb) {
    if (!connected_) return false;
    std::string full = absPath(remote);

    uint16_t flags = (uint16_t)(TNFS_FLAG_WRONLY | TNFS_FLAG_CREAT | TNFS_FLAG_TRUNC);
    uint8_t payload[4 + 256];
    size_t n = 0;
    payload[n++] = (uint8_t)(flags & 0xFF); payload[n++] = (uint8_t)((flags >> 8) & 0xFF);
    payload[n++] = (uint8_t)(0644 & 0xFF);  payload[n++] = (uint8_t)((0644 >> 8) & 0xFF);
    if (n + full.size() + 1 > sizeof(payload)) return false;
    memcpy(payload + n, full.c_str(), full.size() + 1); n += full.size() + 1;

    uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
    if (!transact(TNFS_OPENFILE, payload, n, sid, status, resp, sizeof(resp), rlen) || status != 0 || rlen < 1)
        return false;
    uint8_t fd = resp[0];

    FIL* f = fopen2(localSdPath.c_str(), FA_READ);
    if (!f) { closeHandle(fd); return false; }

    uint8_t buf[TNFS_MAX_BLOCK];
    uint32_t total = 0; bool ok = true;
    for (;;) {
        UINT br = 0;
        if (f_read(f, buf, sizeof(buf), &br) != FR_OK) { ok = false; break; }
        if (br == 0) break;
        uint8_t wpayload[3 + TNFS_MAX_BLOCK];
        wpayload[0] = fd; wpayload[1] = (uint8_t)(br & 0xFF); wpayload[2] = (uint8_t)((br >> 8) & 0xFF);
        memcpy(wpayload + 3, buf, br);
        uint8_t wstatus; uint16_t wsid; uint8_t wresp[2]; size_t wrlen;
        if (!transact(TNFS_WRITEBLOCK, wpayload, 3 + br, wsid, wstatus, wresp, sizeof(wresp), wrlen) ||
            wstatus != 0) {
            ok = false; break;
        }
        total += br;
        if (cb && !cb(total, 0)) { ok = false; break; }
    }
    fclose2(f);
    closeHandle(fd);
    return ok;
}

bool Tnfs::remove(const std::string& name, bool isDir) {
    if (!connected_) return false;
    std::string full = absPath(name);
    uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
    uint8_t cmd = isDir ? TNFS_RMDIR : TNFS_UNLINKFILE;
    return transact(cmd, (const uint8_t*)full.c_str(), full.size() + 1, sid, status, resp, sizeof(resp), rlen) &&
           status == 0;
}

void Tnfs::disconnect() {
    if (connected_) {
        uint16_t sid; uint8_t status; uint8_t resp[4]; size_t rlen;
        transact(TNFS_UMOUNT, nullptr, 0, sid, status, resp, sizeof(resp), rlen); // best-effort
    }
    if (link_ >= 0) ZiFiSock::sock_close(link_);
    link_ = -1;
    connected_ = false;
}

#endif // !PICO_RP2040 && ZIFI_NET_CLIENT
