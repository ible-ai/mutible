// LspSessionRemote.cpp — LspSession's servers of a window over SSH, run by
// its host on the other machine (stage 9's second slice on GTK; the Mac's
// Lsp+Remote.mm, docs/notes/lsp.md, "Over SSH"; LspRemote.h, the core's
// SessionLsp.h). Everything above the transport is the session's as for a
// server on this machine (the same Lsp::Client, the same callbacks, wired
// in serverForKey); what differs is starting one and what its end means:
//   - the settings' commands (lsp.<language>, else the defaults) are sent
//     unresolved, and the host finds the first on its own PATH and the
//     places package managers put servers, so nothing is looked for here;
//     the client is initialized once the host says what started
//     (remoteServerStarted), with the folder as the host names it and no
//     parent process;
//   - the folder's answer about running its own code (server_consent) is
//     kept per host and folder (consentRoot), and the host checks what the
//     folder holds on its own disk: it starts rust-analyzer restricted or
//     not at all, and says which file decided;
//   - one not installed there says so, naming the host and the programs
//     looked for, once in the status bar; no install bar (this machine's
//     commands would not put it there);
//   - a dropped connection ends the window's servers there (the host ends
//     them with the connection): each starts again once the window is
//     connected again (remoteHostReconnected), which opens its documents
//     anew;
//   - files changed there come from the host's disk watching (LspFiles) in
//     place of inotify (remoteFilesChanged).
// Not looked for here: the Python interpreter and the folder's environment
// (the server's own, there), Ruff's rules (the second server only when
// lsp.<language>.also names it), the folder's install offer.
#include "LspSessionPrivate.h"

#include "FolderConsent.h"
#include "LspRemote.h"
#include "SessionLsp.h"

using lspgtk::Server;

namespace {

// How often a server not installed there is looked for again unasked.
constexpr gint64 kMissingAskedAgainUs = 30 * G_USEC_PER_SEC;

std::string dirOfPath(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? std::string(slash == 0 ? "/" : ".") : p.substr(0, slash);
}

}  // namespace

std::string LspSession::consentRoot() const { return remote_ ? remote_->consentKey() : root_; }

std::size_t LspSession::remoteStarted() const { return remote_ ? remote_->started : 0; }
std::size_t LspSession::remoteFileBatches() const { return remote_ ? remote_->fileBatches : 0; }

int LspSession::serverPid(const std::string& key) const {
    auto it = servers_.find(key);
    return it == servers_.end() ? 0 : it->second->pid();
}

void LspSession::useRemoteHost(const std::shared_ptr<SessionLink::Link>& link, std::uint64_t window) {
    if (!link || remote_) return;
    // Anything started before belongs to this machine (the window takes its
    // host before its first file, so there should be none).
    const std::string path = path_;
    const bool started = !servers_.empty();
    if (started) {
        closeDocument();
        stopAllServers();
    }
    remote_ = lspgtk::RemoteHost::make(link, window);
    std::weak_ptr<int> alive = life_;
    remote_->onFiles = [this, alive](const std::vector<Lsp::FileChange>& changes) {
        if (!alive.expired()) remoteFilesChanged(changes);
    };
    tools_->consentUsed = FolderConsent::answer(consentRoot());
    if (started && !path.empty()) documentChanged(path);
}

void LspSession::remoteHostReconnected(const std::shared_ptr<SessionLink::Link>& link) {
    if (!remote_) return;
    const std::string path = path_;   // a copy: closeDocument clears path_
    closeDocument();
    stopAllServers();   // any the lost host's drop has not been heard for yet
    remote_->attachLink(link);
    // What a lost or too-old host left as notes is asked again of this one.
    notes_.clear();
    notInstalled_.clear();
    remoteMissing_.clear();
    if (!path.empty()) documentChanged(path);
}

std::shared_ptr<Server> LspSession::launchRemoteServer(const std::string& key, std::string* note) {
    const std::string machine = remote_->machine().empty() ? std::string("the other machine") : remote_->machine();
    Shared::ServerRequest r;
    r.key = key;
    for (const std::string& c : commandsForKey(key)) {
        std::vector<std::string> words = Lsp::splitCommand(c);
        if (!words.empty()) r.commands.push_back(std::move(words));
    }
    if (r.commands.empty()) {
        *note = "No " + Lsp::serverDisplayName(key) + " language server found";
        return nullptr;
    }
    if (!remote_->connected()) {
        *note = Shared::serversLostText(machine);
        return nullptr;
    }
    if (!remote_->runsServers()) {
        *note = Shared::serversTooOldText(remote_->label().empty() ? machine : remote_->label());
        return nullptr;
    }
    // The folder's answer, and the file's folder (cargo reads its
    // configuration from where the crate is, up); the host checks the rest.
    r.allowed = FolderConsent::answer(consentRoot()) == "allow";
    Lsp::Language lang;
    if (!path_.empty() && languageOf(path_, lang) && lang.server == key) r.fileDir = dirOfPath(path_);
    std::shared_ptr<Server> server = Server::startRemote(key, r.commands.front(), remote_, r);
    if (!server) {
        *note = machine + " could not be asked to start it";
        return nullptr;
    }
    notInstalled_.erase(key);
    return server;
}

// The host started it: what it found, and the client's handshake.
void LspSession::remoteServerStarted(Server* s, const Shared::ServerStarted& started) {
    if (s->stopping()) return;
    const std::string key = s->key();
    Lsp::Client& c = s->client();
    tools_->consentUsed = FolderConsent::answer(consentRoot());
    tools_->projectCode[key] = started.code;   // for the bar's question
    if (key == "python") {
        // No interpreter of this machine's: the server finds its own there
        // (basedpyright reads a root .venv by itself).
        const Lsp::PythonServer kind = Lsp::pythonServerKind(started.argv);
        c.setInitializationOptions(Lsp::pythonInitializationOptions(kind, ""));
        c.setSettings(Lsp::pythonSettings(kind, ""));
    }
    if (started.code.verdict == Lsp::ProjectCode::Restricted) {
        c.setInitializationOptions(started.code.initializationOptions);
        if (!started.code.settings.isNull()) c.setSettings(started.code.settings);
    }
    // The folder as the host names it (its real path: the window's folder),
    // and no parent process: a pid of this machine's names nothing there.
    c.initialize(root_, 0);
    updateStatus();
}

// What a server the host refused or lost leaves in the status bar ("" when it
// ran and ended by itself: a crash, as for a local one).
std::string LspSession::remoteRefusalNote(Server* s) {
    const std::string& why = s->refusal();
    if (why.empty()) return "";
    const std::string key = s->key();
    const std::string machine = remote_ && !remote_->machine().empty() ? remote_->machine() : "the other machine";
    const Json& info = s->refusalInfo();
    if (why == "lost") return Shared::serversLostText(machine);
    if (why == "missing") {
        std::vector<std::string> tried;
        for (const Json& t : info["tried"].elements())
            if (t.isString()) tried.push_back(t.asString());
        remoteMissing_[key] = g_get_monotonic_time();
        return Shared::serverMissingText(Lsp::serverDisplayName(key), machine, tried);
    }
    if (why == "waits") {
        Lsp::ProjectCodeCheck pc;
        pc.verdict = Lsp::ProjectCode::Waits;
        pc.file = info["file"].asString();
        tools_->projectCode[key] = pc;
        return "The " + Lsp::serverDisplayName(key) +
               " language server waits for your answer about this folder's tools";
    }
    std::string note = s->name() + " did not start (" + why + ")";
    // Its last line of standard error says why.
    std::string err = s->errTail();
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    const size_t nl = err.find_last_of('\n');
    if (!err.empty()) note += ": " + (nl == std::string::npos ? err : err.substr(nl + 1));
    return note;
}

// Files the host saw change on its disk: to each server's watchers, as a
// batch from the disk would go (matched here: a remote batch is small).
void LspSession::remoteFilesChanged(const std::vector<Lsp::FileChange>& changes) {
    if (changes.empty()) return;
    filesChangedOnDisk(changes, std::vector<Lsp::WatchedBatch>());
}

// A server not installed there is looked for again after a while, as the
// Mac's does: on the window becoming active and when a terminal command
// finishes, a program put there meanwhile is found.
void LspSession::remoteLookAgain() {
    if (!remote_ || path_.empty() || remoteMissing_.empty()) return;
    const gint64 now = g_get_monotonic_time();
    bool any = false;
    for (auto it = remoteMissing_.begin(); it != remoteMissing_.end();) {
        if (now - it->second < kMissingAskedAgainUs) {
            ++it;
            continue;
        }
        notes_.erase(it->first);
        it = remoteMissing_.erase(it);
        any = true;
    }
    if (!any) return;
    const std::string path = path_;
    documentChanged(path);
}
