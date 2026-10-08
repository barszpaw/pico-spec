#pragma once

// RFC 1436 Gopher client over ZiFiSock (ESP-01S AT firmware), implementing RemoteFs.
// Plain TCP, no TLS, no auth — the simplest of the three remote protocols. Port
// defaults to 70. RP2350 only, behind ZIFI_NET_CLIENT. All calls from the OSD/main
// thread.
//
// Gopher selectors are opaque per-entry strings assigned by the server (not
// necessarily hierarchical paths), and a single menu can link to entries on a
// DIFFERENT host/port than the one you're browsing. So unlike Ftp/Sftp, Gopher
// keeps an in-memory stack of (host, port, selector) frames — one per directory
// level descended into — rather than deriving a parent from a path string. ".."
// pops the stack; cwd(name)/get(name) re-fetch the current directory's menu to
// resolve `name` back to its real (host, port, selector) before following it.
// This means a restored "last location" can only reopen at the SERVER ROOT
// (selector ""), not the exact folder — a deliberate v1 simplification.

#if !PICO_RP2040 && ZIFI_NET_CLIENT

#include "RemoteFs.h"
#include <vector>

class Gopher : public RemoteFs {
public:
    Gopher();
    ~Gopher() override;

    // Validate host/port by fetching the root menu once. user/pass are ignored
    // (gopher has no authentication).
    bool connect(const char* host, uint16_t port, const char* user, const char* pass);

    bool listStream(const std::string& path, RemoteListCb cb, void* ctx) override;
    std::string cacheId() const override { return "gopher_" + host_; }
    bool cwd(const std::string& path) override;
    std::string cwdPath() const override;
    std::string schemeLabel() const override { return "GPHR:"; }
    bool get(const std::string& remote, const std::string& localSdPath, XferProgressCb cb) override;
    // Gopher display text is a human description, not necessarily a filename
    // (e.g. "Some Game (tap)") — the real name with extension lives in the
    // entry's selector. Needed so Alt+Enter "download and run" can tell a .tap
    // from a .trd; without this override every gopher file looks extension-less.
    std::string downloadBasename(const std::string& displayName) override;
    bool put(const std::string&, const std::string&, XferProgressCb) override { return false; } // read-only
    bool remove(const std::string&, bool) override { return false; }                            // read-only
    bool readOnly() const override { return true; }
    void disconnect() override;

private:
    struct Frame {
        std::string host, selector, dispName;
        uint16_t port;
    };
    bool connected;
    std::string host_;          // initial connect host, for cacheId() only
    std::vector<Frame> stack_;  // stack_[0] = server root; stack_.back() = current dir

    // Open a fresh connection to h:p, send sel+CRLF, invoke lineCb(line, arg) for
    // every response line up to (not including) a lone "." terminator or EOF.
    // Returns false on connection/send failure.
    typedef void (*LineCb)(const char* line, void* arg);
    bool fetchMenu(const std::string& h, uint16_t p, const std::string& s,
                   LineCb lineCb, void* arg);
};

#endif // !PICO_RP2040 && ZIFI_NET_CLIENT
