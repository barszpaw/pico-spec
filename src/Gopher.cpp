#include "Gopher.h"

#if !PICO_RP2040 && ZIFI_NET_CLIENT

#include "ZiFiSock.h"
#include "Debug.h"
#include "ff.h"
#include <string.h>
#include <stdlib.h>

Gopher::Gopher() : connected(false) {}
Gopher::~Gopher() { disconnect(); }

// A gopher menu line is "TChar Display \t Selector \t Host \t Port [\t Gopher+]".
// Informational ('i') and error ('3') lines have no real selector/host/port and
// are never selectable — skip them wherever this is parsed.
struct GopherLine {
    char type;
    std::string display, selector, host;
    uint16_t port;
};
static bool parseGopherLine(const char* line, GopherLine& out) {
    if (!line[0]) return false;
    out.type = line[0];
    const char* p = line + 1;
    const char* t1 = strchr(p, '\t');
    if (!t1) { out.display = p; return out.type == 'i'; } // info line w/o tabs: still skippable
    out.display.assign(p, (size_t)(t1 - p));
    const char* t2 = strchr(t1 + 1, '\t');
    if (!t2) return false;
    out.selector.assign(t1 + 1, (size_t)(t2 - (t1 + 1)));
    const char* t3 = strchr(t2 + 1, '\t');
    if (t3) { out.host.assign(t2 + 1, (size_t)(t3 - (t2 + 1))); out.port = (uint16_t)atoi(t3 + 1); }
    else    { out.host.assign(t2 + 1); out.port = 70; }
    return true;
}

bool Gopher::fetchMenu(const std::string& h, uint16_t p, const std::string& s,
                        LineCb lineCb, void* arg) {
    if (!ZiFiSock::begin(true)) return false;
    int id = ZiFiSock::sock_open(h.c_str(), p ? p : 70, false, 12000);
    if (id < 0) return false;
    std::string req = s + "\r\n";
#if ZIFI_TRACE
    Debug::log("GOPHER > %s:%u sel=%s", h.c_str(), (unsigned)p, s.c_str());
#endif
    if (ZiFiSock::sock_send(id, (const uint8_t*)req.data(), req.size(), 8000) < 0) {
        ZiFiSock::sock_close(id);
        return false;
    }
    char line[512];
    while (ZiFiSock::sock_recv_line(id, line, sizeof(line), 10000)) {
        if (line[0] == '.' && line[1] == '\0') break; // RFC 1436 menu/text terminator
        lineCb(line, arg);
    }
    ZiFiSock::sock_close(id);
    return true;
}

bool Gopher::connect(const char* host, uint16_t port, const char*, const char*) {
    host_ = host ? host : "";
    stack_.clear();
    Frame root;
    root.host = host_;
    root.port = port ? port : 70;
    root.selector = "";
    // Validate the server actually responds before committing to "connected".
    struct { bool any; } seen = { false };
    auto probe = [](const char*, void* arg) { ((decltype(seen)*)arg)->any = true; };
    if (!fetchMenu(root.host, root.port, root.selector, probe, &seen)) return false;
    stack_.push_back(root);
    connected = true;
    return true;
}

static void listLineCb(const char* line, void* arg) {
    struct Pair { RemoteListCb cb; void* ctx; };
    Pair* pr = (Pair*)arg;
    GopherLine gl;
    if (!parseGopherLine(line, gl)) return;
    if (gl.type == 'i' || gl.type == '3' || gl.display.empty()) return; // not selectable
    bool isDir = (gl.type == '1');
    pr->cb(pr->ctx, gl.display.c_str(), isDir, 0); // gopher carries no size info
}

bool Gopher::listStream(const std::string&, RemoteListCb cb, void* ctx) {
    if (!connected || stack_.empty()) return false;
    const Frame& f = stack_.back();
    struct Pair { RemoteListCb cb; void* ctx; } pr = { cb, ctx };
    return fetchMenu(f.host, f.port, f.selector, listLineCb, &pr);
}

struct FindCtx { std::string want; bool found; GopherLine result; };
static void findLineCb(const char* line, void* arg) {
    FindCtx* fc = (FindCtx*)arg;
    if (fc->found) return;
    GopherLine gl;
    if (!parseGopherLine(line, gl)) return;
    if (gl.type == 'i' || gl.type == '3') return;
    if (gl.display == fc->want) { fc->result = gl; fc->found = true; }
}

bool Gopher::cwd(const std::string& path) {
    if (!connected || stack_.empty()) return false;
    if (path == "..") {
        if (stack_.size() > 1) stack_.pop_back();
        return true;
    }
    if (path.empty() || path == "/") {
        stack_.resize(1);
        return true;
    }
    const Frame cur = stack_.back(); // copy: fetchMenu below may reallocate stack_ via caller re-entrancy
    FindCtx fc; fc.want = path; fc.found = false;
    if (!fetchMenu(cur.host, cur.port, cur.selector, findLineCb, &fc)) return false;
    if (!fc.found || fc.result.type != '1') return false;
    Frame nf;
    nf.host = fc.result.host; nf.port = fc.result.port; nf.selector = fc.result.selector;
    nf.dispName = path;
    stack_.push_back(nf);
    return true;
}

std::string Gopher::cwdPath() const {
    if (stack_.size() <= 1) return "/";
    std::string p;
    for (size_t i = 1; i < stack_.size(); i++) p += "/" + stack_[i].dispName;
    return p;
}

bool Gopher::get(const std::string& remote, const std::string& localSdPath, XferProgressCb cb) {
    if (!connected || stack_.empty()) return false;
    const Frame cur = stack_.back();
    FindCtx fc; fc.want = remote; fc.found = false;
    if (!fetchMenu(cur.host, cur.port, cur.selector, findLineCb, &fc)) return false;
    if (!fc.found) return false;

    if (!ZiFiSock::begin(true)) return false;
    int id = ZiFiSock::sock_open(fc.result.host.c_str(), fc.result.port ? fc.result.port : 70,
                                  false, 12000);
    if (id < 0) return false;
    std::string req = fc.result.selector + "\r\n";
    if (ZiFiSock::sock_send(id, (const uint8_t*)req.data(), req.size(), 8000) < 0) {
        ZiFiSock::sock_close(id);
        return false;
    }

    FIL* f = fopen2(localSdPath.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (!f) { ZiFiSock::sock_close(id); return false; }

    static const size_t BUF_SZ = 1024;
    uint8_t buf[BUF_SZ];
    uint32_t total = 0;
    bool ok = true;
    for (;;) {
        int n = ZiFiSock::sock_recv(id, buf, BUF_SZ, 8000);
        if (n < 0) { ok = false; break; }
        if (n == 0) break; // EOF (peer CLOSED), per ZiFiSock::sock_recv's documented contract
        UINT bw = 0;
        if (f_write(f, buf, (UINT)n, &bw) != FR_OK || bw != (UINT)n) { ok = false; break; }
        total += bw;
        if (cb && !cb(total, 0)) { ok = false; break; } // 0 = unknown total; user aborted
    }
    fclose2(f);
    ZiFiSock::sock_close(id);
    return ok && total > 0;
}

std::string Gopher::downloadBasename(const std::string& displayName) {
    if (!connected || stack_.empty()) return displayName;
    const Frame& cur = stack_.back();
    FindCtx fc; fc.want = displayName; fc.found = false;
    if (!fetchMenu(cur.host, cur.port, cur.selector, findLineCb, &fc)) return displayName;
    if (!fc.found) return displayName;
    std::string sel = fc.result.selector;
    size_t sl = sel.find_last_of('/');
    std::string fname = (sl == std::string::npos) ? sel : sel.substr(sl + 1);
    return fname.empty() ? displayName : fname;
}

void Gopher::disconnect() {
    connected = false;
    stack_.clear();
}

#endif // !PICO_RP2040 && ZIFI_NET_CLIENT
