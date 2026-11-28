// SessionLink.cpp — see SessionLink.h.
#include "SessionLink.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>

#include <atomic>
#include <cerrno>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ControlPaths.h"
#include "EditorTabs.h"
#include "EnvLock.h"
#include "SessionHost.h"
#include "SessionFiles.h"
#include "SessionProtocol.h"
#include "SessionServer.h"

extern char** environ;

namespace SessionLink {

// What the client's wake reaches: the link's dispatch on the main loop, once
// per batch. Shared with the idle sources it queues, so a link that went
// meanwhile is simply not dispatched; the host's or reader's thread never
// holds the link itself (its last reference must go on the main thread).
struct Link::Wake {
    std::atomic<bool> queued{false};
    std::weak_ptr<Link> link;   // the main thread's only
};

struct Link::Viewers {
    std::mutex mu;
    std::map<Shared::ClientId, Shared::Client*> clients;
};

namespace {

// Every link, by folder, while something holds it.
std::map<std::string, std::weak_ptr<Link>>& registry() {
    static auto* r = new std::map<std::string, std::weak_ptr<Link>>();
    return *r;
}

std::string envString(const char* name) {
    EnvLock::EnvRead r;
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Where a headless host started from here writes what it says:
// $XDG_STATE_HOME/mutible/host-<hash>.log, made anew by each start.
std::string hostLogPath(const std::string& folder) {
    const std::string dir = std::string(g_get_user_state_dir()) + "/mutible";
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string name = Shared::hostSocketName(folder);
    const std::string sock = ".sock";
    if (name.size() > sock.size()) name.resize(name.size() - sock.size());
    return dir + "/" + name + ".log";
}

// `mutible host --folder <folder>` started as this program (the GTK binary
// takes the CLI's subcommands), fully detached: its own session, through a
// second fork so that init (or the nearest subreaper) is its parent and
// reaps it, never this app; stdin from /dev/null, stdout and stderr to a log
// under the state folder; nothing of this process's open files. Its pid,
// or -1 with why.
pid_t startHostProcess(const std::string& folder, std::string* why) {
    std::vector<std::string> args = {"mutible", "host", "--folder", folder};
    const std::string idle = envString("MUTIBLE_HOST_IDLE_MINUTES");
    if (!idle.empty()) {
        args.push_back("--idle-minutes");
        args.push_back(idle);
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // The environment copied now, under the lock (EnvLock.h): after the
    // fork only async-signal-safe calls run.
    std::vector<std::string> env;
    {
        EnvLock::EnvRead r;
        for (char** e = environ; e && *e; e++) env.push_back(*e);
    }
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    const std::string logPath = hostLogPath(folder);
    const int devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0) log = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int pipeFds[2] = {-1, -1};
    if (devNull < 0 || log < 0 || pipe2(pipeFds, O_CLOEXEC) != 0) {
        *why = std::string("could not prepare its start: ") + std::strerror(errno);
        for (int fd : {devNull, log, pipeFds[0], pipeFds[1]})
            if (fd >= 0) close(fd);
        return -1;
    }
    const pid_t child = fork();
    if (child == 0) {
        setsid();
        const pid_t grandchild = fork();
        if (grandchild == 0) {
            // Signals as a fresh program has them: none blocked, the ones
            // the host waits for or reaps with at their defaults.
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            struct sigaction dfl;
            std::memset(&dfl, 0, sizeof dfl);
            dfl.sa_handler = SIG_DFL;
            for (int s : {SIGCHLD, SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT})
                sigaction(s, &dfl, nullptr);
            dup2(devNull, 0);
            dup2(log, 1);
            dup2(log, 2);
            // Nothing else of the app's: its display, its sockets, its pipes.
            if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
                for (int fd = 3; fd < 4096; fd++) close(fd);
            execve("/proc/self/exe", argv.data(), envp.data());
            _exit(127);
        }
        const pid_t sent = grandchild;
        ssize_t n;
        do n = write(pipeFds[1], &sent, sizeof sent);
        while (n < 0 && errno == EINTR);
        _exit(grandchild > 0 ? 0 : 1);
    }
    close(pipeFds[1]);
    close(devNull);
    close(log);
    if (child < 0) {
        *why = std::string("could not fork: ") + std::strerror(errno);
        close(pipeFds[0]);
        return -1;
    }
    pid_t host = -1;
    ssize_t n;
    do n = read(pipeFds[0], &host, sizeof host);
    while (n < 0 && errno == EINTR);
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (n != (ssize_t)sizeof host || host <= 0) {
        *why = "could not start `mutible host`";
        return -1;
    }
    return host;
}

}  // namespace

// ---------------------------------------------------------------- a link

int Link::dispatchOnMain(void* p) {
    const std::shared_ptr<Wake>& w = *static_cast<std::shared_ptr<Wake>*>(p);
    w->queued = false;
    if (std::shared_ptr<Link> l = w->link.lock()) l->dispatch();
    return G_SOURCE_REMOVE;
}

void Link::makeClient() {
    client_ = std::make_unique<Shared::Client>(Model::Unit::CodePoint, "gtk");
    // The windows back up their unsaved files themselves (Persistence.h), so
    // a headless host keeps no backup of a document while one has it open.
    client_->setKeepsBackups(true);
    // Its tabs save through a host that saves for apps (stage 7: Hello flag
    // 16, so a page may save a file a window shows), and hear the host's
    // count of each document as unsaved and its writes for others, which go
    // to the document by its id (listen). On the main loop, inside dispatch.
    client_->setSavesThroughHost(true);
    client_->hearDirty([this](Shared::DocId doc, bool dirty) {
        auto it = notices_.find(doc);
        if (it != notices_.end()) it->second->hostDirty(dirty);
    });
    client_->hearWrites([this](Shared::DocId doc, std::uint64_t v, const Json& json) {
        auto it = notices_.find(doc);
        if (it != notices_.end()) it->second->hostWrote(v, json);
    });
    // The host's messages arrive on its thread (or the socket's reader);
    // the main loop dispatches them, once per batch.
    std::shared_ptr<Wake> w = wake_;
    client_->setWake([w] {
        if (!w->queued.exchange(true))
            g_idle_add_full(G_PRIORITY_DEFAULT, dispatchOnMain, new std::shared_ptr<Wake>(w),
                            [](gpointer p) { delete static_cast<std::shared_ptr<Wake>*>(p); });
    });
}

void Link::startInProcess(bool loopback, unsigned delayUs) {
    kind_ = loopback ? Kind::Loopback : Kind::InProcess;
    socketPath_.clear();
    startedHost_ = 0;
    makeClient();
    Shared::Client* c = client_.get();
    auto wire = [loopback](Shared::Message&& m) { return loopback ? Shared::roundTrip(m) : std::move(m); };
    viewers_ = std::make_shared<Viewers>();
    std::shared_ptr<Viewers> v = viewers_;
    host_ = std::make_unique<Shared::SessionHost>(
        [c, wire, v](Shared::ClientId to, Shared::Message&& m) {
            {
                std::lock_guard<std::mutex> lk(v->mu);
                auto it = v->clients.find(to);
                if (it != v->clients.end()) {
                    it->second->deliver(wire(std::move(m)));
                    return;
                }
            }
            c->deliver(wire(std::move(m)));
        },
        delayUs);
    Shared::SessionHost* h = host_.get();
    // The disk, for the browser's page of this window (its tree and Source
    // Control follow it), before any renderer. Only roots a page names in a
    // watch are walked, so a window with no page served starts none; the
    // window keeps its own monitors and follows its tabs itself (this
    // client asks for no Hello flag 32).
    h->watchDisk(true);
    hostId_ = h->connect();
    const Shared::ClientId id = hostId_;
    client_->connect([h, id, wire](Shared::Message&& m) { h->post(id, wire(std::move(m))); });
    // The Welcome, with the base of this renderer's ids (Client::newId).
    if (!client_->settle()) g_printerr("mutible: the session's host did not answer\n");
}

std::function<void()> Link::connectViewer(Shared::Client& client) {
    if (!host_ || !viewers_) return nullptr;
    Shared::SessionHost* h = host_.get();
    const Shared::ClientId id = h->connect();
    std::shared_ptr<Viewers> v = viewers_;
    {
        std::lock_guard<std::mutex> lk(v->mu);
        v->clients[id] = &client;
    }
    const bool loopback = kind_ == Kind::Loopback;
    client.connect([h, id, loopback](Shared::Message&& m) {
        h->post(id, loopback ? Shared::roundTrip(m) : std::move(m));
    });
    return [h, id, v] {
        {
            std::lock_guard<std::mutex> lk(v->mu);
            v->clients.erase(id);
        }
        h->disconnect(id);
    };
}

// The folder's headless host, found or started, attached to. False, with
// why, when there is none to be had; nothing of it is kept then.
bool Link::startSocket(std::string* why, bool onlyExisting) {
    if (!g_file_test(folder_.c_str(), G_FILE_TEST_IS_DIR)) {
        *why = folder_.empty() ? "the window has no folder" : folder_ + " is not a folder";
        return false;
    }
    // The lasting folder too: a host a window over SSH started on this
    // machine (`mutible host --stdio`) is found as `--stdio` finds it.
    const std::vector<std::string> dirs = Shared::withLastingFolder(ControlChannel::socketFolders(), lastingSocketFolder());
    std::string path = Shared::findHostSocket(folder_, dirs), error;
    bool refused = false;
    std::unique_ptr<Shared::HostLink> link;
    if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
    pid_t started = 0;
    if (!link) {
        // A host that answered and was refused (another user's) is not
        // replaced; a socket nothing listens on (a host that died) or none
        // at all gets a host of our own, which removes it.
        if (!path.empty() && !refused) {
            *why = error;
            return false;
        }
        if (onlyExisting) {
            *why = "no host is running for " + folder_;
            return false;
        }
        started = startHostProcess(folder_, why);
        if (started <= 0) return false;
        // Its socket, within a few seconds: a host starts in milliseconds.
        const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        while (!link && g_get_monotonic_time() < deadline) {
            path = Shared::findHostSocket(folder_, dirs);
            refused = false;
            if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
            if (!link && !path.empty() && !refused) {
                *why = error;
                return false;
            }
            if (!link) {
                if (kill(started, 0) != 0 && errno == ESRCH) break;   // it ended, reaped already
                g_usleep(20 * 1000);
            }
        }
        if (!link) {
            *why = "the `mutible host` it started (pid " + std::to_string(started) +
                   ") did not answer within 5 seconds; see " + hostLogPath(folder_);
            return false;
        }
    }
    kind_ = Kind::Socket;
    makeClient();
    Shared::Client* c = client_.get();
    Shared::HostLink* raw = link.get();
    // Delivered on the link's reader thread; its end (the host gone, or our
    // own detach) leaves the client waiting for nothing.
    raw->start([c](Shared::Message&& m) { c->deliver(std::move(m)); }, [c] { c->hostLost(); });
    client_->connect([raw](Shared::Message&& m) { raw->send(m); });
    socket_ = std::move(link);
    // The Welcome, with the base of this renderer's ids.
    if (!client_->settle(5000) || !client_->idBase()) {
        *why = "the host at " + path + " did not welcome it";
        socket_->close();
        socket_.reset();
        client_.reset();
        return false;
    }
    socketPath_ = path;
    startedHost_ = started;
    return true;
}

// `mutible host --stdio <path>` on the target, through the user's ssh (or
// MUTIBLE_SSH, the checks' stand-in), its pipes the client's transport. The
// Welcome is waited for by the caller (connectRemote), not here.
bool Link::startRemote(std::string* why) {
    std::string ssh = envString("MUTIBLE_SSH");
    if (ssh.empty()) ssh = "/usr/bin/ssh";
    std::vector<std::string> env;
    {
        EnvLock::EnvRead r;
        for (char** e = environ; e && *e; e++) env.push_back(*e);
    }
    std::string error;
    std::unique_ptr<Shared::HostLink> link = Shared::HostLink::spawn(Shared::sshCommand(target_, ssh), env, &error);
    if (!link) {
        *why = error;
        return false;
    }
    kind_ = Kind::Remote;
    makeClient();
    // The lasting host drops this window's stale connection at once (Host::setDrop).
    if (!resume_.empty()) client_->setResume(resume_);
    // The host watches its disk (Hello flag 32): the notices go to the
    // listeners, a turn of the loop later (never inside a dispatch, which a
    // wait can run in the middle of anything).
    client_->hearDisk(
        [this](std::uint64_t window, const Json& changed) {
            diskQueue_.emplace_back(window, changed);
            if (diskQueued_) return;
            diskQueued_ = true;
            std::weak_ptr<Link> weak = wake_->link;
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
                auto* w = static_cast<std::weak_ptr<Link>*>(p);
                if (std::shared_ptr<Link> l = w->lock()) l->flushDisk();
                return G_SOURCE_REMOVE;
            }, new std::weak_ptr<Link>(weak), [](gpointer p) { delete static_cast<std::weak_ptr<Link>*>(p); });
        },
        [this](Shared::DocId doc, int disk) {
            // A file the host's disk lost or got back (flag 4096; 1, changed
            // under unsaved changes, is not sent to a window).
            auto it = notices_.find(doc);
            if (it != notices_.end()) it->second->hostDisk(disk);
        });
    // And its language servers' files changed there (LspFiles).
    client_->hearFiles([this](std::uint64_t window, const Json& files) {
        const auto listeners = filesListeners_;
        for (const auto& l : listeners)
            if (filesListeners_.count(l.first)) l.second(window, files);
    });
    link->connect(*client_);   // its sends written there, the host's messages delivered, its Hello
    socket_ = std::move(link);
    return true;
}

Link::RemoteText Link::readText(const std::string& path) {
    RemoteText out;
    const bool under = path.size() > folder_.size() + 1 && path.compare(0, folder_.size(), folder_) == 0 &&
                       path[folder_.size()] == '/';
    if (lost() || folder_.empty() || !under) return out;
    const std::string rel = path.substr(folder_.size() + 1);
    // 64 MB at most, as the host follows a file (SessionFiles.h's kMaxFollowed);
    // one round trip under 4 MB (Client::readFile).
    Shared::Client::FileRead read = client_->readFile(0, rel, std::size_t(64) << 20);
    out.tooLarge = read.tooLarge;
    if (!read.ok) return out;
    std::string bytes = std::move(read.bytes);
    if (!FolderSearch::decodeFileText(&bytes, &out.encoding, &out.bom)) return out;
    if (!g_utf8_validate(bytes.data(), (gssize)bytes.size(), nullptr)) return out;
    out.text = std::move(bytes);
    out.ok = true;
    return out;
}

bool Link::locate(const std::string& asked, Located* out, int timeoutMs) {
    *out = Located();
    if (kind_ != Kind::Remote || lost() || folder_.empty() || asked.empty()) return false;
    Json ask = Json::object();
    ask.set("op", Json("locate"));
    ask.set("path", Json(asked));
    // Its own answer waited for, no Ping behind it: one round trip.
    const Json answer = client_->read(0, std::move(ask), timeoutMs);
    if (answer.isNull()) return false;
    // A host from before the read answers its error: nothing known.
    if (!answer["error"].isNull() || answer["op"].asString() != "locate") return false;
    out->inside = answer["inside"].asBool();
    if (out->inside) {
        const std::string rel = answer["rel"].asString();
        out->path = rel.empty() ? folder_ : (folder_ == "/" ? "" : folder_) + "/" + rel;
        out->dir = answer["dir"].asBool();
        out->isNew = answer["new"].asBool();
    }
    return true;
}

Link::~Link() {
    // The transport first: its thread delivers to the client.
    if (socket_) socket_->close();   // detaches: the host keeps everything else
    socket_.reset();
    host_.reset();                   // the in-process host's thread joined
    client_.reset();
}

void Link::unlisten(Shared::DocId doc, DocNotices* n) {
    auto it = notices_.find(doc);
    if (it != notices_.end() && it->second == n) notices_.erase(it);
}

void Link::detach() {
    if (!socket_ || detached_) return;
    detached_ = true;
    socket_->close();
}

bool Link::readAsync(Json request, std::function<void(bool, const Json&)> done) {
    if (lost()) return false;
    const std::uint64_t req = client_->requestRead(0, std::move(request));
    if (!req) return false;
    reads_[req] = std::move(done);
    return true;
}

bool Link::fileOpAsync(Json request, std::function<void(bool, const Json&)> done) {
    if (lost() || !client_->hostDoesFileOps()) return false;
    const std::uint64_t req = client_->requestFileOp(0, std::move(request));
    if (!req) return false;
    reads_[req] = std::move(done);
    return true;
}

bool Link::runsGit() const { return !lost() && client_->hostRunsGit(); }
bool Link::replacesOne() const { return !lost() && client_->hostReplacesOne(); }

bool Link::gitAsync(Json request, std::string input, BytesFn done) {
    if (!runsGit()) return false;
    const std::uint64_t req = client_->requestGit(0, std::move(request), std::move(input));
    if (!req) return false;
    byteReads_[req] = std::move(done);
    return true;
}

bool Link::runsFormatters() const { return !lost() && client_->hostFormats(); }

bool Link::formatAsync(Json request, std::string text, BytesFn done) {
    if (!runsFormatters()) return false;
    const std::uint64_t req = client_->requestFormat(0, std::move(request), std::move(text));
    if (!req) return false;
    byteReads_[req] = std::move(done);
    return true;
}

bool Link::readBytesAsync(Json request, BytesFn done) {
    if (lost()) return false;
    const std::uint64_t req = client_->requestRead(0, std::move(request));
    if (!req) return false;
    byteReads_[req] = std::move(done);
    return true;
}

namespace {
// The pieces of one file, asked one after another.
struct PieceRead {
    std::weak_ptr<Link> link;
    std::string rel;
    std::string bytes;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::function<void(Link::RemoteBytes&&)> done;
};
void askPiece(std::shared_ptr<PieceRead> r) {
    std::shared_ptr<Link> link = r->link.lock();
    auto fail = [r](bool tooLarge) {
        Link::RemoteBytes out;
        out.tooLarge = tooLarge;
        r->done(std::move(out));
    };
    if (!link || link->lost()) return fail(false);
    Json ask = Json::object();
    ask.set("op", Json("read"));
    ask.set("path", Json(r->rel));
    ask.set("offset", Json((double)r->bytes.size()));
    ask.set("length", Json((double)Shared::kMaxReadBytes));
    ask.set("hidden", Json(true));
    const bool sent = link->readBytesAsync(std::move(ask), [r, fail](bool ok, const Json& answer, std::string&& piece) {
        if (r->cancel && r->cancel->load()) return;
        if (!ok || !answer["error"].isNull()) return fail(false);
        const double size = answer["size"].asNumber(0);
        if (size > (double)(std::size_t(64) << 20)) return fail(true);   // as the host follows a file
        r->bytes += piece;
        if (piece.empty() || (double)r->bytes.size() >= size) {
            Link::RemoteBytes out;
            out.ok = true;
            out.bytes = std::move(r->bytes);
            r->done(std::move(out));
            return;
        }
        askPiece(r);
    });
    if (!sent) fail(false);
}
}  // namespace

bool Link::readTextAsync(const std::string& path, std::shared_ptr<std::atomic<bool>> cancel,
                         std::function<void(RemoteBytes&&)> done) {
    const bool under = path.size() > folder_.size() + 1 && path.compare(0, folder_.size(), folder_) == 0 &&
                       path[folder_.size()] == '/';
    if (lost() || folder_.empty() || !under) return false;
    auto r = std::make_shared<PieceRead>();
    r->link = weak_from_this();
    r->rel = path.substr(folder_.size() + 1);
    r->cancel = std::move(cancel);
    r->done = std::move(done);
    askPiece(r);   // a first piece refused calls done with failure at once
    return true;
}

std::uint64_t Link::stream(Json request, StreamFn answers) {
    if (lost()) return 0;
    const std::uint64_t req = client_->requestStream(0, std::move(request));
    if (req) streams_[req] = std::move(answers);
    return req;
}

std::uint64_t Link::replace(Json request, StreamFn answers) {
    if (lost()) return 0;
    const std::uint64_t req = client_->requestReplace(0, std::move(request));
    if (req) streams_[req] = std::move(answers);
    return req;
}

void Link::cancelStream(std::uint64_t req) {
    auto it = streams_.find(req);
    if (it == streams_.end()) return;
    streams_.erase(it);
    if (!lost()) client_->cancelStream(req, 0);
}

int Link::addDiskListener(DiskFn f) {
    const int token = ++nextListener_;
    diskListeners_[token] = std::move(f);
    return token;
}

void Link::removeDiskListener(int token) { diskListeners_.erase(token); }

int Link::addFilesListener(FilesFn f) {
    const int token = ++nextListener_;
    filesListeners_[token] = std::move(f);
    return token;
}

void Link::removeFilesListener(int token) { filesListeners_.erase(token); }

// Answers that came: each waiting read and stream is given what is in. A
// callback may ask for more, or cancel (the maps are looked up again).
void Link::pumpStreams() {
    if (pumping_) return;
    pumping_ = true;
    std::vector<std::uint64_t> ids;
    for (const auto& r : reads_) ids.push_back(r.first);
    std::vector<std::uint64_t> byteIds;
    for (const auto& r : byteReads_) byteIds.push_back(r.first);
    for (std::uint64_t id : byteIds) {
        Json answer;
        std::string bytes;
        auto it = byteReads_.find(id);
        if (it == byteReads_.end() || !client_->readAnswer(id, &answer, &bytes)) continue;
        auto cb = std::move(it->second);
        byteReads_.erase(it);
        cb(true, answer, std::move(bytes));
    }
    for (std::uint64_t id : ids) {
        Json answer;
        auto it = reads_.find(id);
        if (it == reads_.end() || !client_->readAnswer(id, &answer)) continue;
        auto cb = std::move(it->second);
        reads_.erase(it);
        cb(true, answer);
    }
    ids.clear();
    for (const auto& s : streams_) ids.push_back(s.first);
    for (std::uint64_t id : ids) {
        if (!streams_.count(id)) continue;
        std::vector<Json> batch;
        const bool done = client_->takeStream(id, &batch);
        if (batch.empty() && !done) continue;
        auto it = streams_.find(id);
        if (it == streams_.end()) continue;
        StreamFn cb = done ? std::move(it->second) : it->second;
        if (done) streams_.erase(it);
        cb(std::move(batch), done);
    }
    pumping_ = false;
}

// The connection is gone: nothing waiting will be answered.
void Link::failPending() {
    auto reads = std::move(reads_);
    reads_.clear();
    for (auto& r : reads) r.second(false, Json());
    auto byteReads = std::move(byteReads_);
    byteReads_.clear();
    for (auto& r : byteReads) r.second(false, Json(), std::string());
    auto streams = std::move(streams_);
    streams_.clear();
    for (auto& s : streams) s.second({}, true);
}

void Link::flushDisk() {
    diskQueued_ = false;
    auto queue = std::move(diskQueue_);
    diskQueue_.clear();
    for (const auto& n : queue) {
        const auto listeners = diskListeners_;
        for (const auto& l : listeners)
            if (diskListeners_.count(l.first)) l.second(n.first, n.second);
    }
}

void Link::dispatch() {
    client_->dispatch();
    for (const std::string& e : client_->takeErrors())
        g_printerr("mutible: the session's host for %s: %s\n", folder_.c_str(), e.c_str());
    pumpStreams();
    if (client_->lost() && (!reads_.empty() || !byteReads_.empty() || !streams_.empty())) failPending();
    if (!client_->lost() || detached_ || lostTold_) return;
    lostTold_ = true;
    if (kind_ == Kind::Remote) {
        // Over SSH: the connection dropped. ssh's end, said in the window's
        // words (the link reaped it before it told the client); the text
        // stays in the window, and Reconnect opens it there again.
        failure_ = Shared::explainSshFailure(target_, socket_ ? socket_->exitStatus() : -1,
                                             socket_ ? socket_->stderrText() : std::string());
        const std::string message = "Disconnected from " + label_ + (failure_.empty() ? "." : ": " + failure_) +
                                    " Your text is kept here; Reconnect opens it there again";
        if (client_->idBase())   // one that never came is told by connectRemote's caller
            g_printerr("mutible: the connection to %s went away%s%s\n", address_.c_str(),
                       failure_.empty() ? "" : ": ", failure_.c_str());
        const auto listeners = lostListeners_;
        for (const auto& [token, f] : listeners)
            if (lostListeners_.count(token)) f(message);
        return;
    }
    const std::string message = "The session's host for " + folder_ +
                                " went away: undo is off in its files and its terminals ended";
    g_printerr("mutible: the session's host for %s went away (%s); its files stay open "
               "without undo and its terminals ended\n",
               folder_.c_str(), socketPath_.c_str());
    // A listener may take itself off.
    const auto listeners = lostListeners_;
    for (const auto& [token, f] : listeners)
        if (lostListeners_.count(token)) f(message);
}

int Link::addLostListener(std::function<void(const std::string&)> f) {
    const int token = ++nextListener_;
    lostListeners_[token] = std::move(f);
    return token;
}

void Link::removeLostListener(int token) { lostListeners_.erase(token); }

std::uint64_t Link::hostWindow(std::uint64_t window) {
    auto it = hostWindow_.find(window);
    if (it != hostWindow_.end()) return it->second;
    return hostWindow_[window] = client_->newId();
}

Json Link::tabsJson(std::uint64_t window, const Model::Window& w) const {
    Json tabs = Shared::windowJson(w);
    Json terms = Json::array();
    auto t = windowTerms_.find(window);
    if (t != windowTerms_.end())
        for (std::uint64_t id : t->second) terms.push(Json((int64_t)id));
    tabs.set("terms", std::move(terms));
    auto l = windowLayout_.find(window);
    if (l != windowLayout_.end() && !l->second.isNull()) tabs.set("termLayout", l->second);
    auto p = windowPanes_.find(window);
    if (p != windowPanes_.end()) {
        if (!p->second.browser.isNull()) tabs.set("browser", p->second.browser);
        if (!p->second.panes.isNull()) tabs.set("panes", p->second.panes);
        if (!p->second.reviews.isNull()) tabs.set("reviews", p->second.reviews);
    }
    return tabs;
}

void Link::windowPanes(std::uint64_t window, const Model::Window& w, Json browser, Json panes, Json reviews) {
    windowPanes_[window] = {std::move(browser), std::move(panes), std::move(reviews)};
    windowTabs(window, w);
}

void Link::windowTerminals(std::uint64_t window, const Model::Window& w, std::vector<std::uint64_t> terms,
                           Json layout) {
    windowTerms_[window] = std::move(terms);
    windowLayout_[window] = std::move(layout);
    windowTabs(window, w);
}

void Link::windowDetached(std::uint64_t window, const Model::Window& w) {
    auto it = hostWindow_.find(window);
    const bool kept = w.count() > 0 || (windowTerms_.count(window) && !windowTerms_[window].empty());
    if (it != hostWindow_.end() && kept) {
        // Sent again even when unchanged: its "seen" makes it the latest
        // record a closing window left, and "detached" tells a page it is
        // closed (the Mac marks its own the same way).
        Json tabs = tabsJson(window, w);
        tabs.set("detached", Json(true));
        client_->windowTabs(it->second, std::move(tabs));
        hostWindow_.erase(it);
        sentTabs_.erase(window);
        windowTerms_.erase(window);
        windowLayout_.erase(window);
        windowPanes_.erase(window);
        return;
    }
    windowClosed(window);
}

Link::Held Link::free(bool all) {
    if (client_->lost()) return Held();
    std::set<std::uint64_t> live;
    for (const auto& [w, h] : hostWindow_) live.insert(h);
    Shared::Client& c = *client_;
    return Shared::reattachFrom(
        c.sessionState(), c.clientId(), live, [&c](Shared::DocId d) { return c.isOpen(d); },
        [&c](Shared::TermId t) { return c.showsTerminal(t); }, all);
}

Link::Held Link::takeForWindow(std::uint64_t window) {
    Held h = free(false);
    if (h.window) {
        // What the window reported before it took the record (its tabs as
        // it was made) goes, or a later window could take that empty record
        // for the latest one.
        auto had = hostWindow_.find(window);
        if (had != hostWindow_.end() && had->second != h.window) client_->windowClosed(had->second);
        hostWindow_[window] = h.window;
        sentTabs_.erase(window);
    }
    return h;
}

Link::Held Link::allFree() { return free(true); }

void Link::windowTabs(std::uint64_t window, const Model::Window& w) {
    Json tabs = tabsJson(window, w);
    std::string dump = tabs.dump();
    auto it = sentTabs_.find(window);
    if (it != sentTabs_.end() && it->second == dump) return;
    sentTabs_[window] = std::move(dump);
    client_->windowTabs(hostWindow(window), std::move(tabs));
}

void Link::windowClosed(std::uint64_t window) {
    sentTabs_.erase(window);
    windowTerms_.erase(window);
    windowLayout_.erase(window);
    windowPanes_.erase(window);
    auto it = hostWindow_.find(window);
    if (it == hostWindow_.end()) return;   // never sent
    client_->windowClosed(it->second);
    hostWindow_.erase(it);
}

std::string Link::checkDocuments(const std::vector<std::pair<std::uint64_t, std::string>>& docs) {
    Shared::Client& c = *client_;
    if (c.lost()) return "";   // nothing to compare with
    std::string out;
    if (!c.settle()) return "the session's host did not answer";
    for (const auto& [id, text] : docs) {
        if (!c.isOpen(id)) {
            out += "document " + std::to_string(id) + " is not open on the host; ";
            continue;
        }
        const Shared::Client::HostDoc h = c.hostDocument(id);
        if (!h.open) out += "the host has no document " + std::to_string(id) + "; ";
        else if (Model::utf8FromUtf16(h.text) != text)
            out += "the host's text of document " + std::to_string(id) + " differs from its buffer (" +
                   std::to_string(h.text.size()) + " units against " + std::to_string(text.size()) +
                   " bytes); ";
    }
    for (const std::string& e : c.takeErrors()) out += "the host said: " + e + "; ";
    return out;
}

std::string Link::checkWindow(std::uint64_t window, const Model::Window& w) {
    Shared::Client& c = *client_;
    if (c.lost()) return "";
    std::string out;
    // The tabs as the host has them, once the window's are sent (the strip
    // sends them as it is drawn; a change that drew nothing is sent here).
    windowTabs(window, w);
    const Json want = tabsJson(window, w);
    const Json state = c.sessionState();
    if (!state.isObject()) return "the session's host did not answer";
    const std::uint64_t onHost = hostWindow(window);
    bool found = false;
    for (const Json& win : state["windows"].elements())
        if ((std::uint64_t)win["window"].asInt() == onHost) {
            found = true;
            if (win["tabs"] != want)
                out += "the host's tabs of window " + std::to_string(window) + " are " + win["tabs"].dump() +
                       ", the window's " + want.dump() + "; ";
        }
    if (!found && w.count() > 0) out += "the host has no tabs for window " + std::to_string(window) + "; ";
    for (const std::string& e : c.takeErrors()) out += "the host said: " + e + "; ";
    return out;
}

// ---------------------------------------------------------------- links

namespace {

std::string folderKey(const std::string& root) {
    std::string folder = Shared::resolveFolder(root);
    return folder.empty() ? root : folder;
}

// In-process links kept while they hold what closed windows left
// (keepWhileHolding), by folder.
std::map<std::string, std::shared_ptr<Link>>& kept() {
    static auto* k = new std::map<std::string, std::shared_ptr<Link>>();
    return *k;
}

}  // namespace

bool folderLost(const std::string& root) {
    std::shared_ptr<Link> have = registry()[folderKey(root)].lock();
    return have && have->lost();
}

void keepWhileHolding(const std::shared_ptr<Link>& link) {
    if (!link) return;
    auto& k = kept();
    // A window over SSH keeps nothing for a next window: its host ends as
    // its input closes.
    if (link->kind() == Link::Kind::Socket || link->kind() == Link::Kind::Remote || link->lost()) {
        if (k.count(link->folder()) && k[link->folder()] == link) k.erase(link->folder());
        return;
    }
    const Json st = link->client().sessionState();
    const bool holding = st.isObject() && (st["documents"].size() > 0 || st["terminals"].size() > 0 ||
                                           st["windows"].size() > 0);
    if (holding) k[link->folder()] = link;
    else if (k.count(link->folder()) && k[link->folder()] == link) k.erase(link->folder());
}

void endDetached() {
    for (const std::shared_ptr<Link>& l : all()) {
        if (l->lost()) continue;
        Shared::Client& c = l->client();
        const Link::Held h = l->allFree();
        for (const Link::HeldDoc& d : h.docs) {
            c.open(d.id, "", "", nullptr);   // a seat, to close it with
            c.close(d.id);
        }
        for (const Link::HeldTerm& t : h.terms) {
            c.attachTerminal(t.id, nullptr);
            c.closeTerminal(t.id);
        }
        for (std::uint64_t w : h.records) c.windowClosed(w);
        c.settle();
    }
    // Kept links holding nothing now go (their hosts' threads end).
    std::vector<std::shared_ptr<Link>> keptNow;
    for (const auto& [folder, l] : kept()) keptNow.push_back(l);
    for (const std::shared_ptr<Link>& l : keptNow) keepWhileHolding(l);
}

std::shared_ptr<Link> forFolder(const std::string& root, bool headless) {
    const std::string folder = folderKey(root);
    auto& reg = registry();
    // A link whose host went is passed over: its holders keep it until they
    // move to the new one.
    if (std::shared_ptr<Link> have = reg[folder].lock(); have && !have->lost()) return have;
    if (kept().count(folder) && kept()[folder]->lost()) kept().erase(folder);
    std::shared_ptr<Link> link(new Link());
    link->folder_ = folder;
    link->wake_ = std::make_shared<Link::Wake>();
    link->wake_->link = link;
    const std::string mode = envString("MUTIBLE_HOST");
    const unsigned delayUs = (unsigned)std::strtoul(envString("MUTIBLE_HOST_DELAY_US").c_str(), nullptr, 10);
    const bool socket = mode == "socket" || (mode.empty() && headless);
    std::string why;
    // The default (a host in this process) shares a lasting host that is
    // running for the folder already (one a window over SSH started here,
    // `mutible host --stdio`) instead of making a second host for it; a
    // mode chosen by MUTIBLE_HOST stays as chosen.
    const bool shareLasting = !socket && mode.empty();
    if ((socket && link->startSocket(&why)) || (shareLasting && link->startSocket(&why, true))) {
        if (link->startedHost())
            g_printerr("mutible: started the session's host for %s (pid %d, %s)\n", folder.c_str(),
                       (int)link->startedHost(), link->socketPath().c_str());
    } else {
        if (socket)
            g_printerr("mutible: the session's host for %s stays in this process: %s\n", folder.c_str(),
                       why.c_str());
        const bool loopback = mode == "loopback";
        static bool told = false;
        if (loopback && !told)
            g_printerr("mutible: the session's host: every message through the protocol's encoding\n");
        if (delayUs && !told) g_printerr("mutible: the session's host: each message %u us late\n", delayUs);
        told = told || loopback || delayUs;
        link->startInProcess(loopback, delayUs);
    }
    reg[folder] = link;
    return link;
}

// Every remote link, by its machine and path as the address named them,
// while something holds it.
namespace {
std::map<std::string, std::weak_ptr<Link>>& remoteRegistry() {
    static auto* r = new std::map<std::string, std::weak_ptr<Link>>();
    return *r;
}

using RemoteDone = std::function<void(std::shared_ptr<Link>, std::string)>;

// `f` from the main loop, never inside the call that asked.
void answerLater(RemoteDone f, std::shared_ptr<Link> l, std::string why) {
    struct Reply {
        RemoteDone f;
        std::shared_ptr<Link> l;
        std::string why;
    };
    g_idle_add([](gpointer p) -> gboolean {
        std::unique_ptr<Reply> r(static_cast<Reply*>(p));
        r->f(std::move(r->l), std::move(r->why));
        return G_SOURCE_REMOVE;
    }, new Reply{std::move(f), std::move(l), std::move(why)});
}
}  // namespace

namespace {
// A random token for one window's connections (/dev/urandom, else GLib's).
std::string newResumeToken() {
    unsigned char raw[16];
    bool ok = false;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        ok = std::fread(raw, 1, sizeof raw, f) == sizeof raw;
        std::fclose(f);
    }
    if (!ok)
        for (unsigned char& b : raw) b = (unsigned char)g_random_int_range(0, 256);
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (unsigned char b : raw) {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}
}  // namespace

// `lost`: the link this connects again for (reconnectRemote), whose token it
// keeps and whose registration stays until this one has its Welcome.
std::shared_ptr<Link> connectRemoteFor(const std::string& address, const std::shared_ptr<Link>& lost,
                                       std::function<void(std::shared_ptr<Link>, std::string)> done) {
    Shared::SshTarget t;
    std::string error;
    if (!Shared::parseSshUrl(address, &t, &error)) {
        answerLater(std::move(done), nullptr, error);
        return nullptr;
    }
    const std::string key = Shared::sshMachineKey(t) + "\n" + t.path;
    auto& reg = remoteRegistry();
    std::shared_ptr<Link> before = reg[key].lock();
    if (before && before != lost && !before->lost() && before->client().idBase()) {
        answerLater(std::move(done), before, "");   // another window's try got there first
        return before;
    }
    std::shared_ptr<Link> link(new Link());
    link->resume_ = lost ? lost->resume_ : before && before->lost() ? before->resume_ : newResumeToken();
    link->target_ = t;
    link->address_ = address;
    link->machine_ = Shared::sshMachineKey(t);
    link->wake_ = std::make_shared<Link::Wake>();
    link->wake_->link = link;
    if (!link->startRemote(&error)) {
        g_printerr("mutible: no window over ssh for %s: %s\n", address.c_str(), error.c_str());
        answerLater(std::move(done), nullptr, error);
        return nullptr;
    }
    link->connecting_ = true;
    if (!lost) reg[key] = link;   // a try again registers when it has its Welcome
    struct Wait {
        std::shared_ptr<Link> link;
        std::string key;
        RemoteDone done;
        gint64 deadline = 0;
    };
    Wait* w = new Wait{link, key, std::move(done), g_get_monotonic_time() + 30 * G_USEC_PER_SEC};
    // The Welcome is waited for by polling: the main loop dispatches the
    // host's messages (the client's wake) and runs on meanwhile.
    g_timeout_add(20, [](gpointer p) -> gboolean {
        Wait* w = static_cast<Wait*>(p);
        Link& l = *w->link;
        std::string why;
        if (l.cancelled_) {   // cancelConnecting: ssh killed, nothing to tell
            if (remoteRegistry()[w->key].lock() == w->link) remoteRegistry().erase(w->key);
            delete w;
            return G_SOURCE_REMOVE;
        }
        if (l.socket_) {
            const Shared::ConnectProgress::Stage before = l.progress_.stage();
            l.progress_.heard(l.socket_->stderrText());
            if (l.progress_.stage() != before && l.progressListener_) l.progressListener_();
        }
        if (l.client_->idBase() && !l.client_->lost()) {
            if (l.client_->hostSavesForApps() && l.client_->hostRunsOwnShell() && !l.client_->hostFolder().empty()) {
                l.folder_ = l.client_->hostFolder();
                l.askedFile_ = l.client_->welcomeJson()["file"].asString();
                // The title's "host:path": the folder as the address names it, so a
                // file's address shows its folder.
                Shared::SshTarget folder = l.target_;
                if (!l.askedFile_.empty()) {
                    const std::size_t slash = folder.path.rfind('/');
                    folder.path = slash == std::string::npos ? "~" : slash == 0 ? "/" : folder.path.substr(0, slash);
                }
                l.label_ = Shared::sshLabel(folder);
                l.connecting_ = false;
                l.progress_.welcomed(l.folder_);
                if (l.progressListener_) l.progressListener_();
                remoteRegistry()[w->key] = w->link;
                RemoteDone f = std::move(w->done);
                std::shared_ptr<Link> link = std::move(w->link);
                delete w;
                f(std::move(link), "");
                return G_SOURCE_REMOVE;
            }
            why = "mutible on " + l.target_.host + " is too old for a window over SSH. Upgrade it there, then "
                  "open the address again.";
        } else if (!l.client_->lost() && g_get_monotonic_time() <= w->deadline) {
            return G_SOURCE_CONTINUE;
        }
        // Closed, not let go: ssh's status and last words stay for saying why.
        if (l.socket_) l.socket_->close();
        if (why.empty())
            why = Shared::explainSshFailure(l.target_, l.socket_ ? l.socket_->exitStatus() : -1,
                                            l.socket_ ? l.socket_->stderrText() : std::string());
        l.detached_ = true;   // nothing to tell: the caller says why
        l.connecting_ = false;
        l.progress_.failed();
        if (l.progressListener_) l.progressListener_();
        if (remoteRegistry()[w->key].lock() == w->link) remoteRegistry().erase(w->key);
        RemoteDone f = std::move(w->done);
        delete w;
        f(nullptr, why);
        return G_SOURCE_REMOVE;
    }, w);
    return link;
}

void Link::cancelConnecting() {
    if (!connecting_ || cancelled_) return;
    cancelled_ = true;
    connecting_ = false;
    progress_.cancel();
    detached_ = true;
    if (socket_) {
        socket_->terminate();   // ssh's group, by its pid, now
        socket_->close();
    }
    if (progressListener_) progressListener_();
    // The poll sees cancelled_ on its next turn and lets go of the try.
}

std::shared_ptr<Link> connectRemote(const std::string& address,
                                    std::function<void(std::shared_ptr<Link>, std::string)> done) {
    return connectRemoteFor(address, nullptr, std::move(done));
}

std::shared_ptr<Link> reconnectRemote(const std::shared_ptr<Link>& lost,
                                      std::function<void(std::shared_ptr<Link>, std::string)> done) {
    if (!lost || lost->kind() != Link::Kind::Remote) {
        answerLater(std::move(done), nullptr, "not a window over SSH");
        return nullptr;
    }
    return connectRemoteFor(lost->address(), lost, std::move(done));
}

bool Link::resumeWindowFrom(const Link& old, std::uint64_t window) {
    auto it = old.hostWindow_.find(window);
    if (it == old.hostWindow_.end() || client_->lost()) return false;
    const Json st = client_->sessionState();
    for (const Json& w : st["windows"].elements())
        if ((std::uint64_t)w["window"].asInt() == it->second) {
            hostWindow_[window] = it->second;
            return true;
        }
    return false;
}

std::string lastingSocketFolder() {
    return ControlChannel::socketFolder(Rename::getenvOrOld("MUTIBLE_CONTROL_DIR"), nullptr, getenv("TMPDIR"),
                                        (unsigned)getuid());
}

std::vector<std::shared_ptr<Link>> all() {
    std::vector<std::shared_ptr<Link>> out;
    for (auto* reg : {&registry(), &remoteRegistry()})
        for (auto it = reg->begin(); it != reg->end();) {
            if (std::shared_ptr<Link> l = it->second.lock()) {
                out.push_back(l);
                ++it;
            } else {
                it = reg->erase(it);
            }
        }
    return out;
}

void detachAll() {
    for (const std::shared_ptr<Link>& l : all()) l->detach();
    kept().clear();   // in-process hosts end, their shells hung up
}

}  // namespace SessionLink
