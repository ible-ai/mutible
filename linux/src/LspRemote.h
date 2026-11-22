// LspRemote.h — a window over SSH's language servers run by its host (stage
// 9's second slice on GTK, the Mac's LspRemote.{h,mm}; docs/notes/lsp.md,
// "Over SSH"; the core's SessionLsp.h). RemoteHost is the window's host as
// the place its servers run: an lspgtk::Server made with Server::startRemote
// sends its client's bytes as LspInput through it and hears LspStarted,
// LspOutput and LspExited from it, so the Lsp::Client above (completion,
// hover, diagnostics, rename, semantic tokens, formatting) is the same as
// for a server on this machine. The host also says which files changed on
// its disk (LspFiles), which the session passes to its servers' watchers in
// place of inotify. Its link is the window's, moved by Reconnect
// (attachLink). Main thread only (the link dispatches there).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Json.h"
#include "LspClient.h"

namespace SessionLink {
class Link;
}

namespace lspgtk {

class Server;

class RemoteHost : public std::enable_shared_from_this<RemoteHost> {
public:
    // `window`: the window's id in the app's session (the host's own id for
    // it is made by the link).
    // Made here, not by a constructor: the files listener holds the host
    // weakly, which a constructor cannot.
    static std::shared_ptr<RemoteHost> make(const std::shared_ptr<SessionLink::Link>& link, std::uint64_t window);
    ~RemoteHost();
    RemoteHost(const RemoteHost&) = delete;
    RemoteHost& operator=(const RemoteHost&) = delete;

    // The window's link again after Reconnect (servers on the old one heard
    // "lost" when it went).
    void attachLink(const std::shared_ptr<SessionLink::Link>& link);
    // The window closes: its servers are stopped there, nothing more is heard.
    void detach();

    // The window's machine ("standin"), and the title's "host:path".
    const std::string& machine() const { return machine_; }
    const std::string& label() const { return label_; }
    // The folder's key for the answers kept per folder (server_consent):
    // the link's ssh:// machine and folder, never a path of this machine's.
    const std::string& consentKey() const { return consentKey_; }
    // Connected now, and whether that host runs language servers (Welcome
    // flag 512).
    bool connected() const;
    bool runsServers() const;

    // LspStart for `server` (`request`: SessionLsp.h's serverRequestJson):
    // its id, or 0, sending nothing, when the host does not or is gone.
    std::uint64_t start(const std::shared_ptr<Server>& server, Json request);
    void send(std::uint64_t id, std::string bytes);
    void stop(std::uint64_t id);

    // Files the host saw change (absolute paths there), on a turn of the
    // main loop after the notice.
    std::function<void(const std::vector<Lsp::FileChange>&)> onFiles;
    // For checks: servers started through it, and LspFiles batches heard.
    std::size_t started = 0, fileBatches = 0;

    // The consent answers' key of a window's folder on `link`.
    static std::string consentKeyOf(const SessionLink::Link& link);

private:
    explicit RemoteHost(std::uint64_t window);
    struct Relay;
    void forgetAll();
    void releaseLink();
    std::shared_ptr<SessionLink::Link> live() const;

    std::shared_ptr<SessionLink::Link> link_;
    std::uint64_t window_ = 0, hostWindow_ = 0;
    int filesToken_ = 0;
    std::unique_ptr<Relay> relay_;
    std::string machine_, label_, consentKey_;
    // Each server by its id on the current link, held weakly: one gone is
    // simply not told.
    std::map<std::uint64_t, std::weak_ptr<Server>> servers_;
    std::set<std::uint64_t> ids_;
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};

}  // namespace lspgtk
