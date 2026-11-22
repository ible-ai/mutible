// LspRemote.cpp — see LspRemote.h.
#include "LspRemote.h"

#include <glib.h>

#include "LspProcess.h"
#include "SessionLink.h"
#include "SessionLsp.h"

namespace lspgtk {

// The client's listener for this window's servers: each message to the
// RemoteHost, which finds the server by its id. Main thread (the link
// dispatches there).
struct RemoteHost::Relay : Shared::Client::ServerListener {
    RemoteHost* owner = nullptr;
    std::shared_ptr<Server> find(std::uint64_t id) const {
        auto it = owner->servers_.find(id);
        return it == owner->servers_.end() ? nullptr : it->second.lock();
    }
    void serverStarted(std::uint64_t id, int pid, const Json& started) override {
        if (std::shared_ptr<Server> s = find(id)) s->remoteStarted(pid, started);
    }
    void serverOutput(std::uint64_t id, const std::string& bytes) override {
        if (std::shared_ptr<Server> s = find(id)) s->remoteWrote(bytes);
    }
    void serverExited(std::uint64_t id, int status, const std::string& why, const Json& info) override {
        std::shared_ptr<Server> s = find(id);
        owner->servers_.erase(id);
        owner->ids_.erase(id);
        if (s) s->remoteExited(status, why, info);
    }
};

std::string RemoteHost::consentKeyOf(const SessionLink::Link& link) { return link.machine() + link.folder(); }

RemoteHost::RemoteHost(std::uint64_t window) : window_(window) {
    relay_ = std::make_unique<Relay>();
    relay_->owner = this;
}

std::shared_ptr<RemoteHost> RemoteHost::make(const std::shared_ptr<SessionLink::Link>& link, std::uint64_t window) {
    std::shared_ptr<RemoteHost> h(new RemoteHost(window));
    h->attachLink(link);
    return h;
}

RemoteHost::~RemoteHost() { detach(); }

std::shared_ptr<SessionLink::Link> RemoteHost::live() const {
    return link_ && !link_->lost() ? link_ : nullptr;
}

bool RemoteHost::connected() const { return live() != nullptr; }

bool RemoteHost::runsServers() const {
    std::shared_ptr<SessionLink::Link> l = live();
    return l && l->runsServers();
}

void RemoteHost::releaseLink() {
    if (link_ && filesToken_) link_->removeFilesListener(filesToken_);
    filesToken_ = 0;
}

namespace {
struct FilesTurn {
    std::weak_ptr<RemoteHost> host;
    std::vector<Lsp::FileChange> changes;
};
}  // namespace

void RemoteHost::attachLink(const std::shared_ptr<SessionLink::Link>& link) {
    releaseLink();
    // Servers on the old link heard "lost" when it went; any it still names
    // are let go of there.
    forgetAll();
    link_ = link;
    hostWindow_ = 0;
    if (!link_) return;
    label_ = link_->label();
    machine_ = link_->target().host;
    if (machine_.empty()) machine_ = label_;
    consentKey_ = consentKeyOf(*link_);
    std::weak_ptr<RemoteHost> weak = weak_from_this();
    filesToken_ = link_->addFilesListener([this, weak](std::uint64_t w, const Json& files) {
        if (hostWindow_ && w && w != hostWindow_) return;
        std::vector<Lsp::FileChange> changes = Shared::filesFrom(files);
        if (changes.empty()) return;
        // Told inside the link's dispatch, which may be inside a wait: the
        // servers hear it on the next turn.
        auto* turn = new FilesTurn{weak, std::move(changes)};
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
            FilesTurn* t = static_cast<FilesTurn*>(p);
            if (std::shared_ptr<RemoteHost> h = t->host.lock()) {
                h->fileBatches++;
                if (h->onFiles) h->onFiles(t->changes);
            }
            return G_SOURCE_REMOVE;
        }, turn, [](gpointer p) { delete static_cast<FilesTurn*>(p); });
    });
}

// Every server of this window let go of on the link it was started on
// (stopped there when it still runs), nothing more told.
void RemoteHost::forgetAll() {
    if (link_)
        for (std::uint64_t id : ids_) link_->client().forgetServer(id);
    ids_.clear();
    servers_.clear();
}

void RemoteHost::detach() {
    forgetAll();
    releaseLink();
    link_.reset();
    onFiles = nullptr;
}

std::uint64_t RemoteHost::start(const std::shared_ptr<Server>& server, Json request) {
    std::shared_ptr<SessionLink::Link> l = live();
    if (!l || !l->runsServers() || !server) return 0;
    const std::uint64_t id = l->client().newId(2000);
    if (!id) return 0;
    if (!hostWindow_) hostWindow_ = l->hostWindowId(window_);
    servers_[id] = server;
    if (!l->client().startServer(id, hostWindow_, std::move(request), relay_.get())) {
        servers_.erase(id);
        return 0;
    }
    ids_.insert(id);
    started++;
    return id;
}

void RemoteHost::send(std::uint64_t id, std::string bytes) {
    if (std::shared_ptr<SessionLink::Link> l = live()) l->client().serverInput(id, std::move(bytes));
}

void RemoteHost::stop(std::uint64_t id) {
    if (std::shared_ptr<SessionLink::Link> l = live()) l->client().stopServer(id);
}

}  // namespace lspgtk
