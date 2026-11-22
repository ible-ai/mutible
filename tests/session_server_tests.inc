// session_server_tests.inc — the `hostserver:*` groups of run_tests.cpp: a
// headless session host (SessionServer.h, SESSION-DESIGN.md stage 6)
// started in this process on a socket in a scratch folder, and renderers
// that attach to it over the socket, each a Client with a HostLink: one
// host per folder, two renderers editing at once and converging, one
// detaching and attaching again to the same state, terminals running on
// across a detach and replayed on attach, `attach --dump`'s text, and the
// lifetime rules with a short timer. Real sockets and shells, under TSan
// in CI. Included in run_tests.cpp after session_terminal_tests.inc, whose
// helpers (TestMirror, scratchFolder, nowMs, gone) it uses.

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <iterator>

#include "SessionFiles.h"   // kMaxReadBytes
#include "HostIntegration.h"
#include "SessionServer.h"
#include "ShellIntegration.h"

namespace {

// A host serving a scratch folder, run() on a thread of its own.
struct ServerRig {
    std::string dir = scratchFolder();   // the socket folder: short, for sun_path
    std::string proj;
    std::unique_ptr<Shared::HostServer> server;
    std::thread thread;
    std::atomic<int> result{-1};   // run()'s answer: 1 the lifetime ended, 0 stopped
    double started = 0, ended = 0;
    bool watch = false;   // HostOptions::watchDisk

    ServerRig() {
        proj = Shared::resolveFolder(scratchFolder());
        dir += "/sockets";
    }
    ~ServerRig() { stop(); }
    Shared::HostServer::Start start(long long idleMs, std::string* error = nullptr) {
        server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        o.socketDir = dir;
        o.idleMs = idleMs;
        o.watchDisk = watch;
        std::string why;
        const Shared::HostServer::Start s = server->start(o, &why);
        if (error) *error = why;
        if (s != Shared::HostServer::Start::Ok) return s;
        started = nowMs();
        thread = std::thread([this] {
            const bool lifetime = server->run();
            ended = nowMs();
            result = lifetime ? 1 : 0;   // after `ended`, which a check reads once it sees this
        });
        return s;
    }
    void stop() {
        if (thread.joinable()) {
            server->stop();
            thread.join();
        }
        server.reset();
    }
    std::string path() const { return dir + "/" + Shared::hostSocketName(proj); }
    // The host's terminal screen's text, "" when it has no such terminal.
    std::string screenText(Shared::TermId t) {
        std::string s;
        server->host().inspect([&](const Shared::Host& h) {
            if (const TerminalScreen* sc = h.screen(t)) s = sc->text();
        });
        return s;
    }
    std::string busy() {
        std::string s;
        server->host().inspect([&](const Shared::Host& h) { s = h.busyReason(); });
        return s;
    }
};

// A renderer over the host's socket: a client, one document's mirror and
// terminals fed into a screen of its own (its emulator).
struct SocketRenderer : Shared::Client::TermListener {
    std::unique_ptr<Shared::HostLink> link;
    std::unique_ptr<Shared::Client> client;
    TestMirror mirror;
    TerminalScreen screen{80, 24};
    std::string out;   // every byte fed, as it came
    int replays = 0;
    int pid = 0;

    bool attach(const std::string& path, const char* name) {
        std::string error;
        link = Shared::HostLink::open(path, &error);
        if (!link) {
            std::printf("  attach: %s\n", error.c_str());
            return false;
        }
        client = std::make_unique<Shared::Client>(Model::Unit::Utf16, name);
        link->connect(*client);
        mirror = TestMirror();
        mirror.client = client.get();
        return true;
    }
    void detach() {
        if (link) link->close();   // the reader first: it delivers to the client
        client.reset();
        link.reset();
    }
    ~SocketRenderer() { detach(); }
    void openDoc(Shared::DocId d, const std::u16string& text) {
        mirror.doc = d;
        mirror.text = text;
        client->open(d, "/notes.txt", Model::utf8FromUtf16(text), &mirror);
    }
    void termOpened(Shared::TermId, int p) override { pid = p; }
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool replay) override {
        screen.feed(b);
        out += b;
        if (replay) replays++;
        else client->ackTerminal(t, b.size());
    }
    bool wait(const std::function<bool()>& done, int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            client->dispatch();
            if (done()) return true;
            if (nowMs() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

bool waitUntil(const std::function<bool()>& done, int ms = 8000) {
    const double end = nowMs() + ms;
    while (!done()) {
        if (nowMs() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

Shared::TermSpec shellSpec(const std::string& cwd, std::vector<std::string> argv) {
    Shared::TermSpec s;
    s.program = "/bin/sh";
    s.argv = std::move(argv);
    s.env = {"PATH=/usr/bin:/bin", "HOME=" + cwd, "TERM=xterm-256color", "PS1=$ ", "LANG=C", "ENV="};
    s.cwd = cwd;
    s.scrollback = 1000;
    return s;
}

// A renderer over two pipes, as a window over SSH is to `mutible host
// --stdio` (stage 8): what it sends is written to the host's input, and a
// reader thread of its own delivers what the host writes, then hears the
// host is gone when that ends.
struct PipeRenderer : Shared::Client::TermListener {
    int toHost = -1, fromHost = -1;
    std::thread reader;
    std::unique_ptr<Shared::Client> client;
    TestMirror mirror;
    TerminalScreen screen{80, 24};
    std::atomic<int> pid{0};

    // `before`: what the client says in its Hello beyond an app's (hearDisk).
    void connect(int write, int read, const std::function<void(Shared::Client&)>& before = nullptr) {
        toHost = write;
        fromHost = read;
        marks.setEventsOnly(true);
        client = std::make_unique<Shared::Client>(Model::Unit::Utf16, "mac");
        client->setKeepsBackups(true);
        client->setSavesThroughHost(true);
        if (before) before(*client);
        mirror.client = client.get();
        reader = std::thread([this] {
            std::string in;
            char buf[65536];
            for (;;) {
                const ssize_t n = ::read(fromHost, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                in.append(buf, (std::size_t)n);
                std::size_t nl;
                while ((nl = in.find('\n')) != std::string::npos) {
                    Shared::Message m;
                    if (Shared::decode(in.substr(0, nl), &m)) client->deliver(std::move(m));
                    in.erase(0, nl + 1);
                }
            }
            client->hostLost();
        });
        client->connect([this](Shared::Message&& m) {
            if (toHost < 0) return;
            const std::string line = Shared::encode(m) + "\n";
            std::size_t at = 0;
            while (at < line.size()) {
                const ssize_t n = ::write(toHost, line.data() + at, line.size() - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return;
                at += (std::size_t)n;
            }
        });
    }
    void closeInput() {
        if (toHost >= 0) ::close(toHost);
        toHost = -1;
    }
    ~PipeRenderer() {
        closeInput();
        if (reader.joinable()) reader.join();
        if (fromHost >= 0) ::close(fromHost);
    }
    void termOpened(Shared::TermId, int p) override { pid = p; }
    void termIntegrated(Shared::TermId, const std::string& shell) override { integrated = shell; }
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool) override {
        screen.feed(b);
        // The marks as the Mac's view reads them: its own stream, with the
        // nonce it gave (marks set).
        for (const TermEvent& e : marks.feed(b)) Shared::applyTermEvent(shell, e);
        client->ackTerminal(t, b.size());
    }
    std::string integrated;       // termIntegrated's (the dispatching thread's)
    TerminalStream marks;         // setMarkNonce'd by a check that reads marks
    Shared::TermShell shell;      // what those marks say
    bool wait(const std::function<bool()>& done, int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            client->dispatch();
            if (done()) return true;
            if (nowMs() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

}  // namespace

static void testSessionServer() {
    GROUP("hostserver:one-per-folder");
    {
        ServerRig rig;
        std::string error;
        CHECK(rig.start(60000, &error) == Shared::HostServer::Start::Ok);
        CHECK(rig.server->socketPath() == rig.path());
        struct stat st;
        CHECK(lstat(rig.dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        CHECK(lstat(rig.path().c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        CHECK(Shared::findHostSocket(rig.proj, {rig.dir}) == rig.path());
        // A second host for the folder finds the first and names its socket.
        Shared::HostServer second;
        Shared::HostOptions o;
        o.folder = rig.proj;
        o.socketDir = rig.dir;
        CHECK(second.start(o, &error) == Shared::HostServer::Start::Running);
        CHECK(error == rig.path());
        // Another folder gets a host of its own beside it.
        Shared::HostServer other;
        o.folder = Shared::resolveFolder(scratchFolder());
        CHECK(other.start(o, &error) == Shared::HostServer::Start::Ok);
        CHECK(other.socketPath() != rig.path());
        // The host says which folder it serves.
        SocketRenderer r;
        CHECK(r.attach(rig.path(), "one"));
        if (r.client) CHECK(r.client->sessionState()["folder"].asString() == rig.proj);
        r.detach();
        rig.stop();
        CHECK(lstat(rig.path().c_str(), &st) != 0);   // the socket goes with the host

        // A socket a dead host left (nothing listens) is replaced.
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", rig.path().c_str());
        CHECK(bind(fd, (sockaddr*)&addr, sizeof addr) == 0);
        close(fd);
        bool refused = false;
        CHECK(!Shared::HostLink::open(rig.path(), &error, &refused) && refused);
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        CHECK(r.attach(rig.path(), "again"));
        r.detach();
        rig.stop();

        // A socket folder others can enter is never used.
        chmod(rig.dir.c_str(), 0755);
        CHECK(!Shared::trustedSocketFolder(rig.dir));
        CHECK(rig.start(60000, &error) == Shared::HostServer::Start::Failed);
        CHECK(Shared::findHostSocket(rig.proj, {rig.dir}).empty());
        chmod(rig.dir.c_str(), 0700);
        CHECK(Shared::resolveFolder(rig.proj + "/missing").empty());
        CHECK(Shared::hostSocketName(rig.proj) == Shared::hostSocketName(rig.proj + "/x/..") ||
              Shared::resolveFolder(rig.proj + "/x/..").empty());
        // A host's socket shares the control channel's folder, so the
        // control command line must never take it for a mutible's.
        CHECK(ControlChannel::pidFromSocketName(Shared::hostSocketName(rig.proj)) == 0);
        CHECK(ControlChannel::pidFromSocketName(Shared::hostSocketName("/")) == 0);
    }

    GROUP("hostserver:attach-edit-detach");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a, b;
        CHECK(a.attach(rig.path(), "a"));
        CHECK(b.attach(rig.path(), "b"));
        CHECK(waitUntil([&] { return rig.server->renderers() == 2; }));
        if (a.client && b.client) {
            const std::u16string start = u"hello\nworld\n";
            a.openDoc(1, start);
            CHECK(a.client->settle());
            // The second renderer finds the document in the session and
            // opens it from the host's text.
            const Json s = b.client->sessionState();
            CHECK(s["documents"].size() == 1 && s["documents"][0]["path"].asString() == "/notes.txt");
            b.mirror.doc = 1;
            b.client->open(1, "/notes.txt", "", &b.mirror);
            CHECK(b.client->settle());
            CHECK(b.mirror.text == start);
            // Both type at once, neither waiting for the other.
            a.mirror.caret = 0;
            b.mirror.caret = b.mirror.text.size();
            for (int i = 0; i < 40; i++) {
                a.mirror.type(u"a");
                b.mirror.type(i % 7 == 0 ? u"\n" : u"b");
                if (i % 5 == 0) a.client->dispatch();
                if (i % 3 == 0) b.client->dispatch();
            }
            for (int i = 0; i < 3; i++) {
                CHECK(a.client->settle());
                CHECK(b.client->settle());
            }
            const std::u16string host = a.client->hostDocument(1).text;
            CHECK(a.mirror.text == b.mirror.text && a.mirror.text == host);
            CHECK(host.size() == start.size() + 80);
            CHECK(a.client->hostDocument(1).dirty);

            // One detaches; the other edits on; it attaches again to the
            // same state, and the host kept its undo history.
            a.detach();
            CHECK(waitUntil([&] { return rig.server->renderers() == 1; }));
            b.client->breakCoalescing(1);
            b.mirror.type(u"!");
            CHECK(b.client->settle());
            CHECK(a.attach(rig.path(), "a again"));
            a.mirror.doc = 1;
            a.client->open(1, "/notes.txt", "", &a.mirror);
            CHECK(a.client->settle());
            CHECK(a.mirror.text == b.mirror.text);
            CHECK(a.client->undoInfo(1).canUndo);
            CHECK(a.client->undo(1, false) == Shared::Outcome::Done);   // b's "!"
            CHECK(b.client->settle());
            CHECK(a.mirror.text == b.mirror.text && a.mirror.text == host);

            // The text dump, from a third renderer.
            SocketRenderer c;
            CHECK(c.attach(rig.path(), "dump"));
            const std::string dump = Shared::dumpSession(*c.client);
            CHECK(dump.find("== host " + rig.proj + "\n") == 0);
            CHECK(dump.find("== document 1 /notes.txt (version ") != std::string::npos);
            CHECK(dump.find(", unsaved)\n" + Model::utf8FromUtf16(host)) != std::string::npos);
        }
    }

    GROUP("hostserver:id-bases");
    {
        // Two renderers over the socket take their ids from the bases their
        // Welcomes gave: their documents and terminals never clash, and a
        // document the host holds keeps its id when the other edits it.
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a, b;
        CHECK(a.attach(rig.path(), "a"));
        CHECK(b.attach(rig.path(), "b"));
        const std::string home = scratchFolder();
        if (a.client && b.client) {
            CHECK(a.client->settle() && b.client->settle());
            const std::uint64_t ba = a.client->idBase(), bb = b.client->idBase();
            CHECK(ba >= Shared::kIdRange && bb >= Shared::kIdRange && ba != bb);
            CHECK(ba % Shared::kIdRange == 0 && bb % Shared::kIdRange == 0);
            const Shared::DocId da = a.client->newId(), db = b.client->newId();
            const Shared::TermId ta = a.client->newId(), tb = b.client->newId();
            CHECK(da == ba + 1 && db == bb + 1 && ta == ba + 2 && tb == bb + 2);
            a.openDoc(da, u"from a\n");
            b.openDoc(db, u"from b\n");
            a.client->openTerminal(ta, shellSpec(home, {"sh"}), &a);
            b.client->openTerminal(tb, shellSpec(home, {"sh"}), &b);
            CHECK(a.wait([&] { return a.pid > 0; }) && b.wait([&] { return b.pid > 0; }));
            CHECK(a.pid != b.pid);
            const Json s = a.client->sessionState();
            std::set<std::uint64_t> docs, terms;
            for (const Json& d : s["documents"].elements()) docs.insert((std::uint64_t)d["doc"].asInt());
            for (const Json& t : s["terminals"].elements()) terms.insert((std::uint64_t)t["term"].asInt());
            CHECK(docs == (std::set<std::uint64_t>{da, db}) && terms == (std::set<std::uint64_t>{ta, tb}));
            // Each terminal's input reaches its own shell.
            a.client->terminalInput(ta, "echo mine-$((1+1))\n");
            b.client->terminalInput(tb, "echo theirs-$((2+2))\n");
            CHECK(a.wait([&] { return a.screen.text().find("mine-2") != std::string::npos; }));
            CHECK(b.wait([&] { return b.screen.text().find("theirs-4") != std::string::npos; }));
            CHECK(a.screen.text().find("theirs-4") == std::string::npos);
            // b edits a's document under its id; a's mirror follows.
            TestMirror other;
            other.client = b.client.get();
            other.doc = da;
            b.client->open(da, "/notes.txt", "", &other);
            CHECK(b.client->settle());
            CHECK(other.text == u"from a\n");
            other.caret = 0;
            other.type(u">");
            CHECK(b.client->settle());
            CHECK(a.wait([&] { return a.mirror.text == u">from a\n"; }));
            CHECK(a.client->hostDocument(da).text == u">from a\n");
            CHECK(a.client->sessionState()["documents"].size() == 2);
            b.client->close(da);   // before `other` goes
            a.client->closeTerminal(ta);
            b.client->closeTerminal(tb);
            CHECK(waitUntil([&] { return gone(a.pid) && gone(b.pid); }, 5000));
        }
    }

    GROUP("hostserver:host-gone");
    {
        // The host ending under a renderer (killed, or its lifetime): the
        // link closes and the client hears it (HostLink::connect), so its
        // waits return at once instead of each waiting out its timeout,
        // nothing more is sent, and ids already given keep coming.
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "a"));
        if (a.client) {
            CHECK(a.client->settle());
            const Shared::DocId d = a.client->newId();
            a.openDoc(d, u"kept\n");
            a.mirror.caret = 4;
            a.mirror.type(u"!");
            CHECK(a.client->settle() && a.client->hostDocument(d).text == u"kept!\n");
            CHECK(!a.client->lost());
            rig.stop();   // the host ends: its connections close
            CHECK(waitUntil([&] { return a.client->lost(); }, 5000));
            const double t0 = nowMs();
            CHECK(!a.client->settle(10000));
            CHECK(a.client->undo(d, false) == Shared::Outcome::Nothing);
            CHECK(!a.client->undoInfo(d).canUndo);
            CHECK(a.client->sessionState().isNull());
            a.mirror.type(u"?");   // recorded, and dropped
            CHECK(!a.client->settle(10000));
            CHECK(nowMs() - t0 < 2000);
            CHECK(a.client->newId() == d + 1);
            a.client->dispatch();   // what the wake queued: nothing to do
        }
    }

    GROUP("hostserver:terminals-across-detach");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "a"));
        const std::string home = scratchFolder();
        if (a.client) {
            a.client->openTerminal(7, shellSpec(home, {"sh"}), &a);
            CHECK(a.wait([&] { return a.pid > 0 && a.out.find("$ ") != std::string::npos; }));
            a.client->terminalInput(7, "echo before-$((1+1))\n");
            CHECK(a.wait([&] { return a.screen.text().find("before-2") != std::string::npos; }));
            // Output that comes while nobody is attached goes into the
            // host's screen; the shell runs on.
            a.client->terminalInput(7, "sleep 0.3; echo while-$((2+2))\n");
            a.client->settle();
            a.detach();
            CHECK(waitUntil([&] { return rig.server->renderers() == 0; }));
            CHECK(waitUntil([&] { return rig.screenText(7).find("while-4") != std::string::npos; }));
            CHECK(!gone(a.pid));

            // A renderer attaching gets the screen replayed, then live output.
            SocketRenderer b;
            CHECK(b.attach(rig.path(), "b"));
            const Json s = b.client->sessionState();
            CHECK(s["terminals"].size() == 1 && s["terminals"][0]["term"].asNumber() == 7);
            CHECK(s["terminals"][0]["pid"].asNumber() == a.pid);
            b.client->attachTerminal(7, &b);
            CHECK(b.wait([&] { return b.replays == 1; }));
            CHECK(b.screen.text().find("before-2") != std::string::npos);
            CHECK(b.screen.text().find("while-4") != std::string::npos);
            b.client->terminalInput(7, "echo after-$((3+3))\n");
            CHECK(b.wait([&] { return b.screen.text().find("after-6") != std::string::npos; }));

            // Two renderers show it at once: both hear its output.
            SocketRenderer c;
            CHECK(c.attach(rig.path(), "c"));
            c.client->attachTerminal(7, &c);
            CHECK(c.wait([&] { return c.replays == 1; }));
            c.client->terminalInput(7, "echo both-$((4+4))\n");
            CHECK(c.wait([&] { return c.screen.text().find("both-8") != std::string::npos; }));
            CHECK(b.wait([&] { return b.screen.text().find("both-8") != std::string::npos; }));

            // The dump draws the screen from the replay.
            SocketRenderer d;
            CHECK(d.attach(rig.path(), "dump"));
            const std::string dump = Shared::dumpSession(*d.client);
            CHECK(dump.find("== terminal 7 (80x24, shell " + std::to_string(a.pid)) != std::string::npos);
            CHECK(dump.find("while-4\n") != std::string::npos && dump.find("both-8\n") != std::string::npos);
            d.detach();

            b.client->closeTerminal(7);
            CHECK(c.wait([&] { return c.client->sessionState()["terminals"].size() == 0; }));
            CHECK(waitUntil([&] { return gone(a.pid); }, 5000));
        }
    }

    GROUP("hostserver:lifetime");
    {
        // No renderer from the start: the host ends once the time is up.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
            CHECK(rig.ended - rig.started >= 140);
        }
        // Unsaved changes keep it; saving lets it go.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            if (r.client) {
                r.openDoc(3, u"text");
                r.mirror.caret = 4;
                r.mirror.type(u"+");
                CHECK(r.client->settle());
                r.detach();
                CHECK(rig.busy().find("/notes.txt has unsaved changes") == 0);
                // Time is the condition: three times the idle time without
                // a renderer, and the host is still there.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                CHECK(rig.result == -1);
                CHECK(r.attach(rig.path(), "r again"));
                r.mirror.doc = 3;
                r.client->open(3, "/notes.txt", "", &r.mirror);
                CHECK(r.client->settle());
                CHECK(r.mirror.text == u"text+");
                // A renderer attached keeps it whatever the time.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                CHECK(rig.result == -1);
                r.client->saved(3);
                CHECK(r.client->settle());
                const double left = nowMs();
                r.detach();
                CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
                CHECK(rig.ended - left >= 140);
            }
        }
        // A command running keeps it (the integration's marks say so), and
        // so does a program in the foreground; an idle shell does not.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            const std::string home = scratchFolder();
            if (r.client) {
                Shared::TermSpec spec = shellSpec(home, {"sh", "-c", "exec cat"});
                spec.nonce = "N9";
                r.client->openTerminal(2, spec, &r);
                CHECK(r.wait([&] { return r.pid > 0; }));
                r.client->terminalInput(2, "\033]633;E;sleep 9;mc=N9\007\033]133;C;mc=N9\007\n");
                CHECK(waitUntil([&] { return rig.busy() == "terminal 2 is running sleep 9"; }));
                r.detach();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));   // as above
                CHECK(rig.result == -1);
                CHECK(r.attach(rig.path(), "r again"));
                r.client->attachTerminal(2, &r);
                r.client->terminalInput(2, "\033]133;D;0;mc=N9\007\n");
                CHECK(waitUntil([&] { return rig.busy().empty(); }));
                r.detach();
                CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
                CHECK(!gone(r.pid));
                rig.stop();   // the host ends: its shells with it
                CHECK(waitUntil([&] { return gone(r.pid); }, 5000));
            }
        }
        {
            ServerRig rig;
            CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            const std::string home = scratchFolder();
            if (r.client) {
                r.client->openTerminal(4, shellSpec(home, {"sh", "-i"}), &r);
                CHECK(r.wait([&] { return r.pid > 0 && r.out.find("$ ") != std::string::npos; }));
                CHECK(rig.busy().empty());
                r.client->terminalInput(4, "sleep 30\n");
                CHECK(waitUntil([&] { return rig.busy() == "terminal 4 is running a program"; }));
                r.client->terminalInput(4, "\003");
                CHECK(waitUntil([&] { return rig.busy().empty(); }));
            }
        }
    }

    GROUP("hostserver:command-line");
    {
        using namespace ControlChannel;
        const std::string cwd = "/work", home = "/home/me";
        auto exists = [](const std::string&) { return true; };
        CliCommand c = parseCommandLine({"host", "--folder", "proj", "--idle-minutes", "0.5"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "host");
        CHECK(c.args["folder"].asString() == "/work/proj" && c.args["idle_minutes"].asNumber() == 0.5);
        CHECK(parseCommandLine({"host"}, cwd, home, exists).error == "host needs --folder or --stdio");
        // Over stdio (stage 8): a folder or a file, ~ this machine's home.
        c = parseCommandLine({"host", "--stdio", "~/proj"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "host" && c.args["stdio"].asString() == "/home/me/proj");
        CHECK(parseCommandLine({"host", "--stdio", "~"}, cwd, home, exists).args["stdio"].asString() == "/home/me");
        CHECK(parseCommandLine({"host", "--stdio", "/srv/a.c"}, cwd, home, exists).args["stdio"].asString() == "/srv/a.c");
        CHECK(!parseCommandLine({"host", "--stdio", "p", "--folder", "q"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"host", "--stdio", "p", "--web", "8080"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"host", "--stdio"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"attach", "--stdio", "p"}, cwd, home, exists).error.empty());
        CHECK(usageText("host").find("--stdio") != std::string::npos);
        CHECK(!parseCommandLine({"host", "--folder", "x", "--idle-minutes", "soon"}, cwd, home, exists)
                   .error.empty());
        CHECK(!parseCommandLine({"host", "--folder", "x", "--dump"}, cwd, home, exists).error.empty());
        c = parseCommandLine({"attach", "--folder", "~/p", "--dump"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "attach" && c.args["dump"].asBool());
        CHECK(c.args["folder"].asString() == "/home/me/p");
        CHECK(!parseCommandLine({"attach", "--folder", "p"}, cwd, home, exists).error.empty());
        CHECK(parseCommandLine({"attach", "-h"}, cwd, home, exists).help);
        CHECK(!usageText("host").empty() && !usageText("attach").empty());
        CHECK(isSubcommand("host") && isSubcommand("attach"));
    }

    GROUP("hostserver:stdio");
    {
        // `mutible host --stdio` (stage 8): one renderer on two pipes, no
        // socket. Its Welcome offers what a window on another machine needs
        // (saves for apps, its own shell); a file typed into is written by
        // the host; a terminal asking for the host's own shell gets the
        // host's environment with the renderer's laid over it, and is the
        // host's child; the renderer's input closing ends the host, its
        // shell with it.
        signal(SIGPIPE, SIG_IGN);   // a write to a pipe nobody reads is EPIPE (Linux)
        const std::string proj = Shared::resolveFolder(scratchFolder());
        {
            std::ofstream f(proj + "/notes.txt");
            f << "one\n";
        }
        int toHost[2], fromHost[2];
        CHECK(pipe(toHost) == 0 && pipe(fromHost) == 0);
        auto server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        o.shellEnv = {"PATH=/usr/bin:/bin", "HOME=" + proj, "PS1=$ ", "ENV=", "LANG=C",
                      "SHELL=/bin/sh", "MARK=host-env", "EXTRA=host-value"};
        o.askedFile = proj + "/notes.txt";
        std::string error;
        CHECK(server->startStdio(o, toHost[0], fromHost[1], &error) == Shared::HostServer::Start::Ok);
        std::atomic<int> result{-1};
        std::thread run([&] { result = server->run() ? 1 : 0; });
        PipeRenderer r;
        r.connect(toHost[1], fromHost[0]);
        CHECK(r.client->settle(5000));
        CHECK(r.client->idBase() != 0);
        CHECK(r.client->hostSavesForApps() && r.client->hostRunsOwnShell());
        CHECK(r.client->hostFolder() == proj);
        CHECK(r.client->welcomeJson()["file"].asString() == proj + "/notes.txt");
        const Json st = r.client->sessionState();
        CHECK(st["folder"].asString() == proj);

        // A file opened with the text the renderer read through the host,
        // typed into, and saved by the host.
        const Shared::DocId d = r.client->newId();
        r.mirror.doc = d;
        r.mirror.text = u"one\n";
        r.client->open(d, proj + "/notes.txt", "one\n", &r.mirror);
        r.mirror.caret = r.mirror.length();
        r.mirror.type(u"two\n");
        CHECK(r.client->settle());
        const std::uint64_t req = r.client->requestSave(d, 0, true, "utf8");
        Shared::Client::SaveAnswer a;
        CHECK(req != 0 && r.wait([&] { return r.client->saveAnswer(req, &a); }));
        CHECK(a.ok);
        {
            std::ifstream f(proj + "/notes.txt");
            std::stringstream ss;
            ss << f.rdbuf();
            CHECK(ss.str() == "one\ntwo\n");
        }

        // The host's own shell, its environment under the renderer's.
        Shared::TermSpec spec;
        spec.hostShell = true;
        spec.env = {"EXTRA=from-renderer", "TERM=xterm-256color"};
        spec.cwd = proj;
        spec.scrollback = 1000;
        r.client->openTerminal(5, spec, &r);
        CHECK(r.wait([&] { return r.pid.load() > 0; }));
        r.client->terminalInput(5, "echo \"got-$MARK-$EXTRA-$PPID\"\n");
        const std::string want = "got-host-env-from-renderer-" + std::to_string((int)getpid());
        CHECK(r.wait([&] { return r.screen.text().find(want) != std::string::npos; }));
        if (r.screen.text().find(want) == std::string::npos) std::printf("  screen: %s\n", r.screen.text().c_str());
        const int shell = r.pid.load();
        CHECK(!gone(shell));

        // The renderer's input closes (the connection went): the host ends.
        r.closeInput();
        CHECK(waitUntil([&] { return result.load() != -1; }, 5000));
        CHECK(result.load() == 1);
        if (run.joinable()) run.join();
        server.reset();   // the host's end: its shells hung up, its output closed
        CHECK(waitUntil([&] { return gone(shell); }, 5000));
        CHECK(waitUntil([&] { return r.client->lost(); }, 5000));
    }

    GROUP("hostserver:integration-shell");
    {
        // A remote renderer's terminal with the host's own integration
        // (HostIntegration.h): integrateShell lays it over ownShellSpec's
        // login shell. zsh: ZDOTDIR at the host's folder, the user's handed
        // back, the renderer's nonce, a stale one gone; still "-zsh".
        auto count = [](const std::vector<std::string>& env, const std::string& e) {
            return (int)std::count(env.begin(), env.end(), e);
        };
        auto named = [](const std::vector<std::string>& env, const std::string& n) {
            int k = 0;
            for (const std::string& e : env) k += e.compare(0, n.size() + 1, n + "=") == 0;
            return k;
        };
        const std::vector<std::string> hostEnv = {"PATH=/usr/bin:/bin", "ZDOTDIR=/u/z", "MUTIBLE_NONCE=stale"};
        const Shared::PtySpec zBefore =
            Shared::ownShellSpec("/bin/zsh", hostEnv, {"TERM=xterm-256color"}, "/", 80, 24);
        Shared::PtySpec z = zBefore;
        CHECK(Shared::integrateShell(z, "/f", true, true, {"zsh"}, "n1") == "zsh");
        CHECK(z.program == "/bin/zsh" && z.argv == std::vector<std::string>{"-zsh"});
        CHECK(count(z.env, "ZDOTDIR=/f") == 1 && named(z.env, "ZDOTDIR") == 1);
        CHECK(count(z.env, "MUTIBLE_USER_ZDOTDIR=/u/z") == 1);
        CHECK(count(z.env, "MUTIBLE_NONCE=n1") == 1 && named(z.env, "MUTIBLE_NONCE") == 1);
        CHECK(count(z.env, "PATH=/usr/bin:/bin") == 1 && count(z.env, "TERM=xterm-256color") == 1);
        // No ZDOTDIR of the user's: none handed back.
        Shared::PtySpec z2 = Shared::ownShellSpec("/usr/local/bin/zsh", {"PATH=/bin"}, {}, "/", 80, 24);
        CHECK(Shared::integrateShell(z2, "/f", true, false, {"zsh", "bash"}, "n2") == "zsh");
        CHECK(named(z2.env, "MUTIBLE_USER_ZDOTDIR") == 0 && count(z2.env, "ZDOTDIR=/f") == 1);
        // Not asked for, no script, no nonce, no folder, another shell: the
        // spec as it was.
        auto untouched = [&](const Shared::PtySpec& before, const std::string& folder, bool zsh, bool bash,
                             const std::vector<std::string>& accepted, const std::string& nonce) {
            Shared::PtySpec p = before;
            return Shared::integrateShell(p, folder, zsh, bash, accepted, nonce).empty() && p.env == before.env &&
                   p.argv == before.argv && p.program == before.program;
        };
        CHECK(untouched(zBefore, "/f", true, true, {"bash"}, "n"));
        CHECK(untouched(zBefore, "/f", false, true, {"zsh", "bash"}, "n"));
        CHECK(untouched(zBefore, "/f", true, true, {"zsh"}, ""));
        CHECK(untouched(zBefore, "", true, true, {"zsh"}, "n"));
        CHECK(untouched(zBefore, "/f", true, true, {}, "n"));
        CHECK(untouched(Shared::ownShellSpec("/bin/sh", hostEnv, {}, "/", 80, 24), "/f", true, true,
                        {"zsh", "bash", "sh"}, "n"));
        // bash, a login shell: --rcfile, which a login bash would not read,
        // so the script reads the login files (MUTIBLE_BASH_LOGIN).
        const Shared::PtySpec bBefore =
            Shared::ownShellSpec("/bin/bash", {"PATH=/bin", "MUTIBLE_BASH_LOGIN=x"}, {}, "/", 80, 24);
        Shared::PtySpec b = bBefore;
        CHECK(Shared::integrateShell(b, "/f", false, true, {"zsh", "bash"}, "n3") == "bash");
        CHECK(b.argv == (std::vector<std::string>{"bash", "--rcfile", "/f/mutible-bash-integration.bash"}));
        CHECK(count(b.env, "MUTIBLE_NONCE=n3") == 1 && count(b.env, "MUTIBLE_BASH_LOGIN=1") == 1 &&
              named(b.env, "MUTIBLE_BASH_LOGIN") == 1);
        // The Mac follows zsh's alone: a bash runs as it would have.
        CHECK(untouched(bBefore, "/f", true, true, {"zsh"}, "n4"));
        CHECK(untouched(bBefore, "/f", true, false, {"zsh", "bash"}, "n4"));
    }

    GROUP("hostserver:integration-files");
    {
        // Where an installed mutible keeps the scripts (shippedFile): beside
        // the program, in a bundle's Resources, in share/mutible, through a
        // link to the program; none, "".
        const std::string root = Shared::resolveFolder(scratchFolder());
        auto put = [](const std::string& path) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            std::ofstream(path) << "x\n";
        };
        put(root + "/build/prog");
        put(root + "/build/a.zsh");
        put(root + "/App/Contents/MacOS/cli");
        put(root + "/App/Contents/Resources/a.zsh");
        put(root + "/usr/bin/mutible");
        put(root + "/usr/share/mutible/a.zsh");
        CHECK(symlink((root + "/usr/bin/mutible").c_str(), (root + "/link").c_str()) == 0);
        CHECK(Shared::shippedFile(root + "/build/prog", "a.zsh") == root + "/build/a.zsh");
        CHECK(Shared::shippedFile(root + "/App/Contents/MacOS/cli", "a.zsh") ==
              root + "/App/Contents/MacOS/../Resources/a.zsh");
        CHECK(Shared::shippedFile(root + "/usr/bin/mutible", "a.zsh") == root + "/usr/bin/../share/mutible/a.zsh");
        CHECK(Shared::shippedFile(root + "/link", "a.zsh") == root + "/usr/bin/../share/mutible/a.zsh");
        CHECK(Shared::shippedFile(root + "/build/prog", "missing.zsh").empty());
        CHECK(Shared::shippedFile("", "a.zsh").empty());
        const std::string self = Shared::ownProgramPath();
        struct stat st;
        CHECK(!self.empty() && stat(self.c_str(), &st) == 0 && S_ISREG(st.st_mode));

        // The host's copies: made at the first terminal that wants one, in
        // a folder of its own (0700) that also clears a gone host's, made
        // again when the system cleared them, and gone with the host.
        const std::string base = root + "/base";
        mkdir(base.c_str(), 0755);
        const std::string stale = base + "/Mutible-host-999999-1", other = base + "/Mutible-host-x";
        mkdir(stale.c_str(), 0700);
        std::ofstream(stale + "/.zshenv") << "old\n";
        mkdir(other.c_str(), 0700);
        char cwd[PATH_MAX];
        const std::string zshScript = std::string(getcwd(cwd, sizeof cwd) ? cwd : ".") + "/resources/" +
                                      ShellIntegration::kZshScriptName;
        const std::string scriptText = readFile(zshScript.c_str());
        std::string folder;
        if (scriptText.empty()) {
            std::printf("  (no resources/%s here; the copies' checks skipped)\n", ShellIntegration::kZshScriptName);
        } else {
            Shared::HostIntegration hi;
            hi.setBase(base);
            hi.setScripts(zshScript, root + "/no-such.bash");
            CHECK(hi.any() && hi.folder().empty());
            Shared::PtySpec p = Shared::ownShellSpec("/bin/zsh", {"PATH=/bin"}, {}, "/", 80, 24);
            CHECK(hi.apply(p, {"zsh", "bash"}, "n5") == "zsh");
            folder = hi.folder();
            CHECK(folder.compare(0, base.size() + 14, base + "/Mutible-host-") == 0);
            CHECK(lstat(folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
            CHECK(readFile((folder + "/" + ShellIntegration::kZshScriptName).c_str()) == scriptText);
            CHECK(readFile((folder + "/.zshenv").c_str()) == ShellIntegration::zshenvStub(true));
            CHECK(std::count(p.env.begin(), p.env.end(), "ZDOTDIR=" + folder) == 1);
            CHECK(!std::filesystem::exists(stale) && std::filesystem::exists(other));
            // No bash script installed: a bash runs as it would have.
            Shared::PtySpec pb = Shared::ownShellSpec("/bin/bash", {"PATH=/bin"}, {}, "/", 80, 24);
            CHECK(hi.apply(pb, {"zsh", "bash"}, "n6").empty() && pb.argv == std::vector<std::string>{"-bash"});
            // Cleared by the system meanwhile: copied again.
            unlink((folder + "/.zshenv").c_str());
            Shared::PtySpec p2 = Shared::ownShellSpec("/bin/zsh", {"PATH=/bin"}, {}, "/", 80, 24);
            CHECK(hi.apply(p2, {"zsh"}, "n7") == "zsh" && hi.folder() == folder);
            CHECK(readFile((folder + "/.zshenv").c_str()) == ShellIntegration::zshenvStub(true));
        }
        CHECK(folder.empty() || !std::filesystem::exists(folder));
        // Nothing installed: nothing made, nothing laid over the shell.
        Shared::HostIntegration none;
        none.setBase(base);
        Shared::PtySpec p = Shared::ownShellSpec("/bin/zsh", {"PATH=/bin"}, {}, "/", 80, 24);
        CHECK(!none.any() && none.apply(p, {"zsh"}, "n").empty() && none.folder().empty());
    }

    GROUP("hostserver:integration");
    {
        // `mutible host --stdio` with its machine's zsh script (Welcome flag
        // 128): a renderer's TermSpec asking for zsh's integration with its
        // nonce gets the host's login zsh marked (TermOpened names zsh), the
        // user's ZDOTDIR and .zshrc read, the nonce out of the environment, a
        // running command busy and its status seen by the renderer's own
        // stream; asked for bash's alone, or by a renderer from before (no
        // integration asked), nothing is set up.
        char cwd[PATH_MAX];
        const std::string zshScript = std::string(getcwd(cwd, sizeof cwd) ? cwd : ".") + "/resources/" +
                                      ShellIntegration::kZshScriptName;
        if (access("/bin/zsh", X_OK) != 0 || access(zshScript.c_str(), R_OK) != 0) {
            std::printf("  (no /bin/zsh, or no resources/%s here; skipped)\n", ShellIntegration::kZshScriptName);
        } else {
            signal(SIGPIPE, SIG_IGN);
            const std::string proj = Shared::resolveFolder(scratchFolder());
            mkdir((proj + "/zdot").c_str(), 0700);
            std::ofstream(proj + "/zdot/.zshrc") << "PS1='%% '\nexport FROM_ZSHRC=yes\n";
            int toHost[2], fromHost[2];
            CHECK(pipe(toHost) == 0 && pipe(fromHost) == 0);
            auto server = std::make_unique<Shared::HostServer>();
            Shared::HostOptions o;
            o.folder = proj;
            o.shellEnv = {"PATH=/usr/bin:/bin", "HOME=" + proj, "ZDOTDIR=" + proj + "/zdot", "LANG=C",
                          "SHELL=/bin/zsh"};
            o.zshIntegration = zshScript;
            std::string error;
            CHECK(server->startStdio(o, toHost[0], fromHost[1], &error) == Shared::HostServer::Start::Ok);
            std::atomic<int> result{-1};
            std::thread run([&] { result = server->run() ? 1 : 0; });
            PipeRenderer r;
            r.connect(toHost[1], fromHost[0]);
            CHECK(r.client->settle(5000));
            CHECK(r.client->hostRunsOwnShell() && r.client->hostIntegrates());
            const std::string nonce = "0123456789abcdef0123456789abcdef";
            r.marks.setMarkNonce(nonce);
            Shared::TermSpec spec;
            spec.hostShell = true;
            spec.env = {"TERM=xterm-256color"};
            spec.cwd = proj;
            spec.nonce = nonce;
            spec.integration = {"zsh"};
            r.client->openTerminal(7, spec, &r);
            CHECK(r.wait([&] { return r.pid.load() > 0; }));
            CHECK(r.integrated == "zsh");
            CHECK(r.wait([&] { return r.shell.known && r.shell.atPrompt; }));
            r.client->terminalInput(7, "echo \"zd=$ZDOTDIR rc=$FROM_ZSHRC n=${MUTIBLE_NONCE-unset}-end\"\n");
            const std::string want = "zd=" + proj + "/zdot rc=yes n=unset-end";
            CHECK(r.wait([&] { return r.screen.text().find(want) != std::string::npos; }));
            if (r.screen.text().find(want) == std::string::npos)
                std::printf("  screen: %s\n", r.screen.text().c_str());
            const long long doneBefore = r.shell.done;
            r.client->terminalInput(7, "sleep 1; false\n");
            CHECK(r.wait([&] { return r.shell.busy && r.shell.command == "sleep 1; false"; }));
            CHECK(r.wait([&] { return !r.shell.busy && r.shell.done == doneBefore + 1 && r.shell.atPrompt; }));
            CHECK(r.shell.status == 1);

            // bash's alone asked for (the shell is zsh), and nothing asked:
            // no integration, so the renderer believes no mark.
            Shared::TermSpec bashOnly = spec;
            bashOnly.integration = {"bash"};
            r.integrated = "none yet";
            r.pid = 0;
            r.client->openTerminal(8, bashOnly, &r);
            CHECK(r.wait([&] { return r.pid.load() > 0; }));
            CHECK(r.integrated == "none yet");
            Shared::TermSpec before = spec;
            before.integration.clear();
            before.nonce.clear();
            r.pid = 0;
            r.client->openTerminal(9, before, &r);
            CHECK(r.wait([&] { return r.pid.load() > 0; }));
            CHECK(r.integrated == "none yet");

            r.closeInput();
            CHECK(waitUntil([&] { return result.load() != -1; }, 5000));
            if (run.joinable()) run.join();
            server.reset();
        }
    }

    GROUP("hostserver:read-file");
    {
        // A window over SSH opening a file (Client::readFile): one wait,
        // so one round trip, for a file under kMaxReadBytes, and only the
        // read's own answer waited for (no Ping behind it); past that the
        // pieces left asked four at a time. Errors, the limit and dotfiles
        // as before; readAll's listings in one wait.
        signal(SIGPIPE, SIG_IGN);
        const std::string proj = Shared::resolveFolder(scratchFolder());
        mkdir((proj + "/src").c_str(), 0755);
        mkdir((proj + "/src/deep").c_str(), 0755);
        {
            std::ofstream(proj + "/src/a.c") << "int a;\n";
            std::ofstream(proj + "/.env") << "X=1\n";
            std::ofstream(proj + "/empty.txt");
        }
        // Three pieces (two rounds), and six (three: four at most at once).
        auto pattern = [](std::size_t n) {
            std::string s(n, '\0');
            for (std::size_t i = 0; i < n; i++) s[i] = (char)('a' + (i * 7 + i / 4093) % 26);
            return s;
        };
        const std::string three = pattern(2 * Shared::kMaxReadBytes + 1000);
        const std::string six = pattern(5 * Shared::kMaxReadBytes + 17);
        std::ofstream(proj + "/three.txt", std::ios::binary) << three;
        std::ofstream(proj + "/six.txt", std::ios::binary) << six;
        int toHost[2], fromHost[2];
        CHECK(pipe(toHost) == 0 && pipe(fromHost) == 0);
        auto server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        std::string error;
        CHECK(server->startStdio(o, toHost[0], fromHost[1], &error) == Shared::HostServer::Start::Ok);
        std::atomic<int> result{-1};
        std::thread run([&] { result = server->run() ? 1 : 0; });
        PipeRenderer r;
        r.connect(toHost[1], fromHost[0]);
        CHECK(r.client->settle(5000));
        Shared::Client& c = *r.client;
        const std::size_t limit = std::size_t(64) << 20;
        // How many times the client waited since `from`, said when not `want`.
        auto waited = [&c](std::uint64_t from, std::uint64_t want) {
            const std::uint64_t got = c.waits() - from;
            if (got != want) std::printf("  waits: %llu, not %llu\n", (unsigned long long)got, (unsigned long long)want);
            return got == want;
        };

        std::uint64_t w = c.waits();
        Shared::Client::FileRead f = c.readFile(0, "src/a.c", limit);
        CHECK(f.ok && f.bytes == "int a;\n" && !f.tooLarge && f.error.empty());
        CHECK(waited(w, 1));
        w = c.waits();
        f = c.readFile(0, ".env", limit);   // dotfiles: the window shows what the tree lists
        CHECK(f.ok && f.bytes == "X=1\n");
        CHECK(waited(w, 1));
        f = c.readFile(0, "empty.txt", limit);
        CHECK(f.ok && f.bytes.empty());
        w = c.waits();
        f = c.readFile(0, "three.txt", limit);
        CHECK(f.ok && f.bytes.size() == three.size() && f.bytes == three);
        CHECK(waited(w, 2));
        w = c.waits();
        f = c.readFile(0, "six.txt", limit);
        CHECK(f.ok && f.bytes.size() == six.size() && f.bytes == six);
        CHECK(waited(w, 3));
        // Over the limit: nothing read, said so, after the first answer.
        w = c.waits();
        f = c.readFile(0, "three.txt", Shared::kMaxReadBytes);
        CHECK(!f.ok && f.tooLarge && f.bytes.empty());
        CHECK(waited(w, 1));
        // The host's refusals.
        f = c.readFile(0, "src/missing.c", limit);
        CHECK(!f.ok && !f.tooLarge && !f.error.empty());
        f = c.readFile(0, "src", limit);
        CHECK(!f.ok && f.error == "not a file");
        f = c.readFile(0, "../outside", limit);
        CHECK(!f.ok && !f.error.empty());
        // Nothing left behind: every answer was taken.
        CHECK(c.settle(5000));

        // Two folders listed in one wait, each answer at its place.
        auto list = [](const std::string& rel) {
            Json j = Json::object();
            j.set("op", Json("list"));
            j.set("path", Json(rel));
            j.set("hidden", Json(true));
            return j;
        };
        w = c.waits();
        const std::vector<Json> both = c.readAll(0, {list("src"), list("src/deep"), list("nowhere")});
        CHECK(waited(w, 1));
        CHECK(both.size() == 3);
        if (both.size() == 3) {
            CHECK(both[0]["path"].asString() == "src" && both[0]["entries"].size() == 2);   // deep, a.c
            CHECK(both[1]["path"].asString() == "src/deep" && both[1]["entries"].size() == 0);
            CHECK(!both[2]["error"].isNull());
        }

        // The host gone: no answer, at once.
        r.closeInput();
        CHECK(waitUntil([&] { return result.load() != -1; }, 5000));
        if (run.joinable()) run.join();
        server.reset();
        CHECK(waitUntil([&] { return c.lost(); }, 5000));
        f = c.readFile(0, "src/a.c", limit, 5000);
        CHECK(!f.ok && f.bytes.empty());
    }

    GROUP("hostserver:stdio-target");
    {
        // What `mutible host --stdio PATH` serves (stage 8): a folder, a
        // file (its folder, the file named), or a new file's name in a
        // folder that is there (named, and new); nothing else.
        namespace fs = std::filesystem;
        const std::string proj = Shared::resolveFolder(scratchFolder());
        fs::create_directories(proj + "/sub");
        {
            std::ofstream f(proj + "/notes.txt");
            f << "one\n";
        }
        Shared::StdioTarget t;
        std::string why;
        CHECK(Shared::stdioTarget(proj, &t, &why) && t.folder == proj && t.file.empty() && !t.isNew);
        CHECK(Shared::stdioTarget(proj + "/sub/", &t, &why) && t.folder == proj + "/sub" && t.file.empty());
        CHECK(Shared::stdioTarget(proj + "/notes.txt", &t, &why) && t.folder == proj &&
              t.file == proj + "/notes.txt" && !t.isNew);
        CHECK(Shared::stdioTarget(proj + "/sub/new.txt", &t, &why) && t.folder == proj + "/sub" &&
              t.file == proj + "/sub/new.txt" && t.isNew);
        // Spelled through a link: the folder as resolved, the name as given.
        fs::create_directory_symlink(proj + "/sub", proj + "/via");
        CHECK(Shared::stdioTarget(proj + "/via/later.md", &t, &why) && t.folder == proj + "/sub" &&
              t.file == proj + "/sub/later.md" && t.isNew);
        why.clear();
        CHECK(!Shared::stdioTarget(proj + "/missing/new.txt", &t, &why));
        CHECK(why == proj + "/missing/new.txt: no such file or folder");
        CHECK(!Shared::stdioTarget(proj + "/missing/..", &t, &why));
        CHECK(!Shared::stdioTarget(proj + "/notes.txt/x", &t, &why));   // a file is no folder
        CHECK(!Shared::stdioTarget("", &t, &why));
    }

    GROUP("hostserver:locate");
    {
        // FsRead "locate" (stage 8: a Mac's window over SSH asks its host
        // whether an ssh:// address is in its folder before opening another
        // window): the address's path resolved as --stdio resolves it, by
        // this machine's ~ and real places, and only inside-or-not told.
        namespace fs = std::filesystem;
        const std::string home = Shared::resolveFolder(scratchFolder());
        const std::string proj = home + "/proj";
        fs::create_directories(proj + "/src");
        fs::create_directories(home + "/elsewhere");
        {
            std::ofstream f(proj + "/src/a.txt");
            f << "a\n";
        }
        {
            std::ofstream f(home + "/elsewhere/out.txt");
            f << "out\n";
        }
        fs::create_directory_symlink(proj, home + "/alias");
        fs::create_symlink(home + "/elsewhere/out.txt", proj + "/src/leak.txt");
        auto at = [&](const std::string& asked) { return Shared::locateInFolder(proj, asked, home); };
        Json j = at("~/proj/src/a.txt");
        CHECK(j["op"].asString() == "locate" && j["path"].asString() == "~/proj/src/a.txt");
        CHECK(j["inside"].asBool() && j["rel"].asString() == "src/a.txt" && !j["dir"].asBool() &&
              !j["new"].asBool());
        // The same file by its absolute path, through a link, and with ".."
        CHECK(at(proj + "/src/a.txt")["rel"].asString() == "src/a.txt");
        CHECK(at("~/alias/src/a.txt")["rel"].asString() == "src/a.txt");
        CHECK(at("~/elsewhere/../proj/src/a.txt")["rel"].asString() == "src/a.txt");
        // The folder itself, by ~ and through the link; a folder below it.
        j = at("~/proj");
        CHECK(j["inside"].asBool() && j["rel"].asString().empty() && j["dir"].asBool());
        CHECK(at(home + "/alias/")["inside"].asBool() && at(home + "/alias/")["rel"].asString().empty());
        j = at("~/proj/src");
        CHECK(j["inside"].asBool() && j["rel"].asString() == "src" && j["dir"].asBool());
        // A file not there yet in a folder that is: inside, and new.
        j = at("~/proj/src/fresh.txt");
        CHECK(j["inside"].asBool() && j["rel"].asString() == "src/fresh.txt" && j["new"].asBool());
        // Outside: another folder, the folder's parent, a missing folder, a
        // link out of the folder, the root, nothing.
        for (const std::string& out : {std::string("~/elsewhere/out.txt"), std::string("~"), std::string("~/elsewhere"),
                                       std::string("~/proj/missing/x.txt"), std::string("~/proj/src/leak.txt"),
                                       std::string("/"), std::string(""), std::string("~/proj/src/a.txt/x")}) {
            j = at(out);
            CHECK(!j["inside"].asBool() && j["rel"].isNull());
            if (j["inside"].asBool()) std::printf("  inside: %s\n", out.c_str());
        }
        // Through readWorkspace, as the host answers FsRead (its own HOME).
        Json ask = Json::object();
        ask.set("op", Json("locate"));
        ask.set("path", Json(proj + "/src/a.txt"));
        j = Shared::readWorkspace(proj, ask);
        CHECK(j["error"].isNull() && j["inside"].asBool() && j["rel"].asString() == "src/a.txt");
    }

    GROUP("hostserver:spawn");
    {
        // A command's pipes as a link (HostLink::spawn, what carries a host
        // over ssh): a line sent is a line its output brings back, and once
        // it ends its exit status and standard error are there to say why.
        Shared::Message ping;
        ping.kind = Shared::Kind::Ping;
        ping.req = 42;
        std::string error;
        std::unique_ptr<Shared::HostLink> link = Shared::HostLink::spawn(
            {"sh", "-c", "IFS= read -r l; printf '%s\\n' \"$l\"; echo \"it said: $X\" >&2; exit 3"},
            {"PATH=/usr/bin:/bin", "X=hi"}, &error);
        CHECK(link != nullptr);
        if (link) {
            CHECK(link->pid() > 0 && link->exitStatus() == -1);
            std::mutex mu;
            std::vector<Shared::Message> got;
            std::atomic<bool> ended{false};
            link->start(
                [&](Shared::Message&& m) {
                    std::lock_guard<std::mutex> lk(mu);
                    got.push_back(std::move(m));
                },
                [&] { ended = true; });
            CHECK(link->send(ping));
            CHECK(waitUntil([&] { return ended.load(); }, 5000));
            {
                std::lock_guard<std::mutex> lk(mu);
                CHECK(got.size() == 1 && got[0].kind == Shared::Kind::Ping && got[0].req == 42);
            }
            const int st = link->exitStatus();
            CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 3);
            CHECK(link->stderrText().find("it said: hi") != std::string::npos);
            CHECK(link->closed() && !link->send(ping));
            link->close();
        }
        // A command that outlives its input closing is hung up by close().
        link = Shared::HostLink::spawn({"sleep", "30"}, {"PATH=/usr/bin:/bin"}, &error);
        CHECK(link != nullptr);
        if (link) {
            link->start([](Shared::Message&&) {});
            const int pid = link->pid();
            link->close();
            CHECK(gone(pid));
            // Hung up (or, when that did not end it in time, killed), never
            // left to end by itself 30 s on: a check of what close() did,
            // not of how long a loaded machine took to do it.
            const int st = link->exitStatus();
            CHECK(st != -1 && WIFSIGNALED(st) && (WTERMSIG(st) == SIGHUP || WTERMSIG(st) == SIGKILL));
            if (!(st != -1 && WIFSIGNALED(st))) std::printf("  sleep's status: %d\n", st);
        }
        // terminate(): the command and its group (a child it started)
        // killed at once, so close() has nothing to wait for; then nothing.
        link = Shared::HostLink::spawn({"sh", "-c", "sleep 30 & echo $! >&2; wait"}, {"PATH=/usr/bin:/bin"}, &error);
        CHECK(link != nullptr);
        if (link) {
            std::atomic<bool> ended{false};
            link->start([](Shared::Message&&) {}, [&] { ended = true; });
            const int pid = link->pid();
            CHECK(waitUntil([&] { return !link->stderrText().empty() && link->stderrText().back() == '\n'; }, 5000));
            const int child = std::atoi(link->stderrText().c_str());
            CHECK(child > 0);
            link->terminate();
            CHECK(waitUntil([&] { return ended.load(); }, 5000));
            link->close();
            const int st = link->exitStatus();
            CHECK(gone(pid) && st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
            CHECK(child > 0 && waitUntil([&] { return kill(child, 0) != 0 && errno == ESRCH; }, 5000));
            link->terminate();   // ended: nothing to do
        }
        // A program that is not there: no link, or one that ends at once
        // with 127 (a libc that reports exec's failure only so).
        link = Shared::HostLink::spawn({"/nonexistent/ssh-stand-in"}, {}, &error);
        if (link) {
            std::atomic<bool> ended{false};
            link->start([](Shared::Message&&) {}, [&] { ended = true; });
            CHECK(waitUntil([&] { return ended.load(); }, 5000));
            const int st = link->exitStatus();
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 127);
        } else {
            CHECK(error.find("could not run /nonexistent/ssh-stand-in") != std::string::npos);
        }
    }

    GROUP("hostserver:ssh-url");
    {
        Shared::SshTarget t;
        std::string why;
        CHECK(Shared::parseSshUrl("ssh://thinkpad/~/proj", &t, &why));
        CHECK(t.user.empty() && t.host == "thinkpad" && t.port == 0 && t.path == "~/proj");
        CHECK(Shared::sshLabel(t) == "thinkpad:~/proj" && Shared::sshMachineKey(t) == "ssh://thinkpad");
        CHECK(Shared::parseSshUrl("SSH://eric@pi.local:2222/srv/www/", &t, &why));
        CHECK(t.user == "eric" && t.host == "pi.local" && t.port == 2222 && t.path == "/srv/www");
        CHECK(Shared::sshLabel(t) == "eric@pi.local:/srv/www");
        CHECK(Shared::sshMachineKey(t) == "ssh://eric@pi.local:2222");
        CHECK(Shared::parseSshUrl("ssh://box", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/~", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/my%20notes/a%27b.txt", &t, &why) && t.path == "/my notes/a'b.txt");
        CHECK(Shared::parseSshUrl("ssh://[fe80::1%25en0]:22/x", &t, &why));
        CHECK(t.host == "fe80::1%25en0" && t.port == 22 && t.path == "/x");
        CHECK(Shared::sshMachineKey(t) == "ssh://[fe80::1%25en0]:22");
        // Refused: not ssh, no host, an option in disguise, a bad port, a
        // host name with what no host name holds, a line break in the path.
        for (const char* bad :
             {"http://box/x", "ssh://", "ssh:///x", "ssh://-oProxyCommand=touch%20x/x", "ssh://-x/y",
              "ssh://-l@box/x", "ssh://box:0/x", "ssh://box:65536/x", "ssh://box:22a/x", "ssh://box:/x",
              "ssh://bo x/y", "ssh://box;rm/x", "ssh://box/a%0Ab", "ssh://box/a%2", "ssh://@box/x",
              "ssh://[::1/x"}) {
            why.clear();
            const bool ok = Shared::parseSshUrl(bad, &t, &why);
            if (ok) std::printf("  taken: %s\n", bad);
            CHECK(!ok && !why.empty());
        }
    }

    GROUP("hostserver:ssh-command");
    {
        Shared::SshTarget t;
        CHECK(Shared::parseSshUrl("ssh://eric@pi:2222/~/it's here", &t, nullptr));
        const std::vector<std::string> a = Shared::sshCommand(t, "/usr/bin/ssh");
        const std::vector<std::string> want = {
            "/usr/bin/ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "-o",
            "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", "-e", "none", "-p", "2222", "-l",
            "eric", "--", "pi", "mutible host --stdio '~/it'\\''s here'"};
        CHECK(a == want);
        CHECK(Shared::shellQuote("") == "''" && Shared::shellQuote("a b") == "'a b'");
        CHECK(Shared::shellQuote("$(x)`y`") == "'$(x)`y`'");
        CHECK(Shared::parseSshUrl("ssh://box/p", &t, nullptr));
        CHECK(Shared::sshCommand(t, "")[0] == "ssh");
        CHECK(Shared::sshCommand(t, "").size() == 15);   // no -p, no -l

        // Why it never answered, in the window's words.
        auto exited = [](int code) { return code << 8; };   // as waitpid gives it
        std::string s = Shared::explainSshFailure(t, exited(255), "eric@box: Permission denied (publickey).\n");
        CHECK(s.find("password or a key's passphrase") != std::string::npos && s.find("`ssh box`") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(255), "Host key verification failed.\n");
        CHECK(s.find("host key") != std::string::npos && s.find("`ssh box` once in a terminal") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(127), "bash: line 1: mutible: command not found\n");
        CHECK(s.find("mutible is not installed on box") != std::string::npos && s.find("PPA") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(2), "mutible: unknown option --stdio for host\n");
        CHECK(s.find("too old") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(255), "ssh: connect to host box port 22: Connection refused\n\n");
        CHECK(s == "ssh to box failed: ssh: connect to host box port 22: Connection refused");
        s = Shared::explainSshFailure(t, exited(1), "mutible: /home/x/p: no such file or folder\n");
        CHECK(s == "The host on box ended: mutible: /home/x/p: no such file or folder");
        CHECK(Shared::explainSshFailure(t, -1, "") == "The connection to box closed.");
        CHECK(Shared::parseSshUrl("ssh://eric@box:2200/p", &t, nullptr));
        s = Shared::explainSshFailure(t, exited(255), "Permission denied\n");
        CHECK(s.find("`ssh -p 2200 eric@box`") != std::string::npos);
        // `--stdio`'s word that it started is never the reason quoted.
        const std::string started = std::string(Shared::kStdioStartedLine) + "\n";
        s = Shared::explainSshFailure(t, exited(1), started);
        CHECK(s == "The host on eric@box ended (exit status 1).");
        s = Shared::explainSshFailure(t, exited(1), "mutible: /x: no such file or folder\n" + started);
        CHECK(s == "The host on eric@box ended: mutible: /x: no such file or folder");
        s = Shared::explainSshFailure(t, exited(1), started + "mutible: no lasting host\n");
        CHECK(s == "The host on eric@box ended: mutible: no lasting host");
    }

    GROUP("hostserver:connect-progress");
    {
        // A window over SSH connecting: its line through each stage, the
        // stages passed, and the ends it cannot leave.
        Shared::SshTarget t;
        CHECK(Shared::parseSshUrl("ssh://eric@pi/~/proj", &t, nullptr));
        using Stage = Shared::ConnectProgress::Stage;
        const std::string started = Shared::kStdioStartedLine;
        CHECK(Shared::stdioStarted(started + "\n") && Shared::stdioStarted("Warning: added key\r\n" + started));
        CHECK(Shared::stdioStarted(started));   // its line not ended yet
        CHECK(!Shared::stdioStarted("") && !Shared::stdioStarted("x" + started + "\n"));
        CHECK(!Shared::stdioStarted(started.substr(0, started.size() - 1)));   // half of it so far

        Shared::ConnectProgress p;
        CHECK(p.stage() == Stage::Connecting && !p.ended() && p.passed(Stage::Connecting));
        CHECK(p.line(t) == "Connecting to eric@pi…");
        p.heard("");
        p.heard("Warning: Permanently added 'pi' to the list of known hosts.\n");
        CHECK(p.stage() == Stage::Connecting && !p.passed(Stage::Starting));
        p.heard("Warning: ...\n" + started + "\n");
        CHECK(p.stage() == Stage::Starting && p.line(t) == "Starting mutible on eric@pi…");
        p.welcomed("/home/eric/proj");
        CHECK(p.stage() == Stage::Opening && p.line(t) == "Opening /home/eric/proj on eric@pi…");
        p.heard(started);   // heard again: nothing goes back
        CHECK(p.stage() == Stage::Opening);
        p.opened();
        CHECK(p.stage() == Stage::Open && p.ended() && p.line(t) == "Opened /home/eric/proj on eric@pi.");
        CHECK(p.passed(Stage::Connecting) && p.passed(Stage::Starting) && p.passed(Stage::Opening));
        CHECK(!p.cancel() && p.stage() == Stage::Open);   // too late to cancel
        p.failed();
        CHECK(p.stage() == Stage::Open && !p.passed(Stage::Failed));

        // An older host says nothing as it starts: connecting, then opening.
        Shared::ConnectProgress old;
        old.welcomed("/srv");
        CHECK(old.stage() == Stage::Opening && !old.passed(Stage::Starting));
        CHECK(old.line(t) == "Opening /srv on eric@pi…");
        old.opened();
        CHECK(old.stage() == Stage::Open);

        // Opened only from opening.
        Shared::ConnectProgress early;
        early.opened();
        CHECK(early.stage() == Stage::Connecting);

        // Cancelled while ssh connects: it ends there, and nothing later moves it.
        Shared::ConnectProgress c;
        CHECK(c.cancel() && c.stage() == Stage::Cancelled && c.ended());
        CHECK(c.line(t) == "Stopped connecting to eric@pi.");
        c.heard(started);
        c.welcomed("/x");
        c.opened();
        c.failed();
        CHECK(c.stage() == Stage::Cancelled && !c.passed(Stage::Starting) && !c.passed(Stage::Failed));
        CHECK(!c.cancel());

        // Failed while mutible starts there.
        Shared::ConnectProgress f;
        f.heard(started + "\n");
        f.failed();
        CHECK(f.stage() == Stage::Failed && f.passed(Stage::Starting) && f.line(t) == "Could not connect to eric@pi.");
        CHECK(!f.cancel());
        f.welcomed("/x");
        CHECK(f.stage() == Stage::Failed && !f.passed(Stage::Opening));
    }

    GROUP("hostserver:own-shell");
    {
        // TermSpec's hostShell: the host's login shell as a login shell, in
        // the host's environment with the renderer's laid over it.
        Shared::PtySpec p = Shared::ownShellSpec(
            "", {"SHELL=/usr/bin/zsh", "PATH=/remote/bin", "TERM=dumb", "HOME=/home/e", "junk"},
            {"TERM=xterm-256color", "COLORTERM=truecolor"}, "/home/e/proj", 100, 30);
        CHECK(p.program == "/usr/bin/zsh" && p.argv == std::vector<std::string>{"-zsh"});
        CHECK(p.cwd == "/home/e/proj" && p.cols == 100 && p.rows == 30);
        const std::vector<std::string> env = {"SHELL=/usr/bin/zsh", "PATH=/remote/bin", "HOME=/home/e",
                                              "TERM=xterm-256color", "COLORTERM=truecolor"};
        CHECK(p.env == env);
        p = Shared::ownShellSpec("/bin/bash", {"SHELL=/usr/bin/zsh"}, {}, "/", 80, 24);
        CHECK(p.program == "/bin/bash" && p.argv == std::vector<std::string>{"-bash"});
        p = Shared::ownShellSpec("", {}, {}, "/", 80, 24);   // the password database's, or sh
        CHECK(!p.program.empty() && p.program[0] == '/' && p.argv.size() == 1 && p.argv[0][0] == '-');
        // Through the protocol: the flag is kept, and an older spec has none.
        Shared::TermSpec s;
        s.hostShell = true;
        CHECK(Shared::termSpecFrom(Shared::termSpecJson(s)).hostShell);
        CHECK(!Shared::termSpecFrom(Shared::termSpecJson(Shared::TermSpec())).hostShell);
        CHECK(!Shared::termSpecJson(Shared::TermSpec()).has("hostShell"));
    }

    GROUP("hostserver:lasting-rules");
    {
        // A host started for a window over SSH (stage 8): its idle time
        // from the environment, else four hours; where `--stdio` looks for
        // one; and the waits before each try at connecting again.
        CHECK(Shared::lastingIdleMs(nullptr) == 4LL * 60 * 60 * 1000);
        CHECK(Shared::lastingIdleMs("") == Shared::kLastingIdleMs);
        CHECK(Shared::lastingIdleMs("0.5") == 30000);
        CHECK(Shared::lastingIdleMs("90") == 90LL * 60000);
        CHECK(Shared::lastingIdleMs("0") == 0);
        CHECK(Shared::lastingIdleMs("soon") == Shared::kLastingIdleMs);
        CHECK(Shared::lastingIdleMs("-1") == Shared::kLastingIdleMs);
        CHECK(Shared::lastingIdleMs("5m") == Shared::kLastingIdleMs);
        CHECK((Shared::withLastingFolder({"/run/user/1/mutible"}, "/tmp/mutible-1") ==
               std::vector<std::string>{"/run/user/1/mutible", "/tmp/mutible-1"}));
        CHECK((Shared::withLastingFolder({"/tmp/mutible-1"}, "/tmp/mutible-1") ==
               std::vector<std::string>{"/tmp/mutible-1"}));
        CHECK(Shared::reconnectDelayMs(0) == 500 && Shared::reconnectDelayMs(1) == 1000);
        CHECK(Shared::reconnectDelayMs(4) == 8000 && Shared::reconnectDelayMs(5) == 16000);
        CHECK(Shared::reconnectDelayMs(6) == 30000 && Shared::reconnectDelayMs(11) == 30000);
        CHECK(Shared::reconnectDelayMs(Shared::kReconnectAttempts) == -1 && Shared::reconnectDelayMs(-1) == -1);
        long long total = 0;
        for (int i = 0; Shared::reconnectDelayMs(i) >= 0; i++) total += Shared::reconnectDelayMs(i);
        CHECK(total > 2 * 60000 && total < 5 * 60000);   // a few minutes of trying, then Reconnect
        // The command line: --lasting with --folder only.
        using namespace ControlChannel;
        auto exists = [](const std::string&) { return true; };
        CliCommand c = parseCommandLine({"host", "--folder", "p", "--lasting"}, "/w", "/h", exists);
        CHECK(c.error.empty() && c.args["lasting"].asBool() && c.args["folder"].asString() == "/w/p");
        CHECK(!parseCommandLine({"host", "--stdio", "p", "--lasting"}, "/w", "/h", exists).error.empty());
        CHECK(usageText("host").find("--lasting") != std::string::npos);
        CHECK(usageText("host").find("MUTIBLE_HOST_IDLE_MINUTES") != std::string::npos);
    }

    GROUP("hostserver:start-detached");
    {
        // What `--stdio` starts a lasting host with: not this process's
        // child, in a session of its own, SIGHUP ignored, reading
        // /dev/null, writing to its log (0600), holding no descriptor of
        // this process's.
        signal(SIGPIPE, SIG_IGN);
        const std::string dir = scratchFolder();
        const std::string log = dir + "/host.log", mark = dir + "/mark";
        int held[2];
        CHECK(pipe(held) == 0);   // not close-on-exec: the child must not keep it
        std::string error;
        const int pid = Shared::startDetached(
            {"/bin/sh", "-c",
             "echo \"pid=$$ ppid=$PPID\" > '" + mark + "'; kill -HUP $$; read x; echo \"read=$?\" >> '" + mark +
                 "'; echo to-the-log; exec sleep 30"},
            log, &error);
        CHECK(pid > 0);
        if (pid > 0) {
            auto markText = [&] {
                std::ifstream f(mark);
                return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            };
            CHECK(waitUntil([&] { return markText().find("read=") != std::string::npos; }, 5000));
            const std::string m = markText();
            CHECK(m.find("pid=" + std::to_string(pid) + " ") == 0);
            CHECK(m.find(" ppid=" + std::to_string(getpid()) + "\n") == std::string::npos);   // not ours
            CHECK(m.find("read=1") != std::string::npos);   // /dev/null: no line, EOF
            CHECK(getsid(pid) != getsid(0));   // (EPERM, where the system says so, is another session too)
            CHECK(!gone(pid));   // the hangup it sent itself was ignored
            int st = 0;
            CHECK(waitpid(pid, &st, WNOHANG) < 0 && errno == ECHILD);
            struct stat s;
            CHECK(stat(log.c_str(), &s) == 0 && (s.st_mode & 0777) == 0600);
            CHECK(waitUntil([&] {
                std::ifstream f(log);
                std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return t.find("to-the-log") != std::string::npos;
            }, 5000));
            // The pipe's write end was ours alone: closed here, it reads EOF.
            close(held[1]);
            pollfd p{held[0], POLLIN, 0};
            CHECK(poll(&p, 1, 3000) == 1);
            char b;
            CHECK(read(held[0], &b, 1) == 0);
            kill(pid, SIGKILL);   // this check's own, by the pid it started (init reaps it)
        } else {
            close(held[1]);
        }
        close(held[0]);
        CHECK(Shared::startDetached({dir + "/no-such-program"}, "", &error) == 0);
        CHECK(error.find("could not run") == 0);
        CHECK(Shared::startDetached({}, "", &error) == 0);
    }

    GROUP("hostserver:lasting");
    {
        // `mutible host --stdio` over a lasting host: found or started
        // (connectOrStartHost), relayed to (relayStdio, the Welcome naming
        // the file asked for), and outliving the connection: a renderer
        // coming back with the same resume token finds its document (text,
        // unsaved, undo) and its terminal (its screen, the same shell), and
        // the old connection, which never noticed it was gone, is let go of
        // at once (Host::setDrop), so it holds no terminal's credit.
        signal(SIGPIPE, SIG_IGN);
        ServerRig rig;
        {
            std::ofstream f(rig.proj + "/notes.txt");
            f << "one\n";
        }
        int starts = 0;
        auto start = [&](std::string* why) {
            starts++;
            const Shared::HostServer::Start s = rig.start(60000, why);
            return s == Shared::HostServer::Start::Ok ? (int)getpid() : 0;
        };
        std::string error;
        mkdir(rig.dir.c_str(), 0700);
        // A relay: two pipes to a renderer, the socket to the host.
        struct Relay {
            int toRelay[2] = {-1, -1}, fromRelay[2] = {-1, -1};
            int sock = -1;
            std::thread thread;
            std::atomic<bool> done{false};
            PipeRenderer r;
            void run(int s, const std::string& file, const std::string& token) {
                sock = s;
                CHECK(pipe(toRelay) == 0 && pipe(fromRelay) == 0);
                thread = std::thread([this, file] {
                    Shared::relayStdio(toRelay[0], fromRelay[1], sock, file, false);
                    // As `--stdio` does: the host's socket closed (the host
                    // detaches this renderer), and ssh's end.
                    ::close(sock);
                    sock = -1;
                    ::close(fromRelay[1]);
                    fromRelay[1] = -1;
                    done = true;
                });
                r.connect(toRelay[1], fromRelay[0], [&](Shared::Client& c) { c.setResume(token); });
            }
            ~Relay() {
                r.closeInput();
                if (thread.joinable()) thread.join();
                for (int fd : {toRelay[0], sock}) if (fd >= 0) ::close(fd);
            }
        };
        const int s1 = Shared::connectOrStartHost(rig.proj, {rig.dir}, start, 5000, &error);
        CHECK(s1 >= 0 && starts == 1);
        if (s1 >= 0) {
            auto a = std::make_unique<Relay>();
            a->run(s1, rig.proj + "/notes.txt", "window-1");
            CHECK(a->r.client->settle());
            const Json w = a->r.client->welcomeJson();
            CHECK(w["file"].asString() == rig.proj + "/notes.txt" && !w.has("new"));
            CHECK(a->r.client->hostFolder() == rig.proj && a->r.client->hostRunsOwnShell());
            const Shared::DocId d = a->r.client->newId();
            const Shared::TermId t = a->r.client->newId();
            a->r.mirror.doc = d;
            a->r.mirror.text = u"one\n";
            a->r.client->open(d, rig.proj + "/notes.txt", "one\n", &a->r.mirror);
            a->r.mirror.caret = 4;
            a->r.mirror.type(u"two\n");
            CHECK(a->r.client->settle());
            CHECK(a->r.client->hostDocument(d).dirty);
            a->r.client->openTerminal(t, shellSpec(rig.proj, {"sh"}), &a->r);
            CHECK(a->r.wait([&] { return a->r.pid > 0; }));
            const int shell = a->r.pid;
            a->r.client->terminalInput(t, "echo before-$((6*7))\n");
            CHECK(a->r.wait([&] { return a->r.screen.text().find("before-42") != std::string::npos; }));

            // The second connection finds the host running: nothing started.
            const int s2 = Shared::connectOrStartHost(rig.proj, {rig.dir}, start, 5000, &error);
            CHECK(s2 >= 0 && starts == 1);
            if (s2 >= 0) {
                // The first connection is left as a dead one is: open, its
                // renderer silent. The second, resuming, replaces it.
                auto b = std::make_unique<Relay>();
                b->run(s2, "", "window-1");
                CHECK(b->r.client->settle());
                CHECK(b->r.client->welcomeJson().isNull());   // no file asked: nothing added
                CHECK(waitUntil([&] { return a->done.load(); }, 5000));   // dropped by the host
                CHECK(waitUntil([&] { return rig.server->renderers() == 1; }, 5000));
                const Json st = b->r.client->sessionState();
                CHECK(st["documents"].size() == 1 && (std::uint64_t)st["documents"][0]["doc"].asInt() == d);
                CHECK(st["documents"][0]["dirty"].asBool());
                CHECK(st["terminals"].size() == 1 && (std::uint64_t)st["terminals"][0]["term"].asInt() == t);
                CHECK(st["terminals"][0]["pid"].asInt() == shell && !gone(shell));
                // The document under its id: the host's text, then undo.
                b->r.mirror.doc = d;
                b->r.client->open(d, rig.proj + "/notes.txt", "", &b->r.mirror);
                CHECK(b->r.client->settle());
                CHECK(b->r.mirror.text == u"one\ntwo\n");
                CHECK(b->r.client->undoInfo(d).canUndo);
                CHECK(b->r.client->undo(d, false) == Shared::Outcome::Done);
                CHECK(b->r.client->settle());
                CHECK(b->r.mirror.text == u"one\n");
                // The terminal: its screen replayed, and the same shell types on.
                b->r.client->attachTerminal(t, &b->r);
                CHECK(b->r.wait([&] { return b->r.screen.text().find("before-42") != std::string::npos; }));
                b->r.client->terminalInput(t, "echo after-$((7*8)) $$\n");
                CHECK(b->r.wait([&] {
                    return b->r.screen.text().find("after-56 " + std::to_string(shell)) != std::string::npos;
                }));
                // A burst of output past the credit a silent viewer would hold.
                b->r.client->terminalInput(t, "i=0; while [ $i -lt 3000 ]; do echo line-$i-of-output; i=$((i+1)); done; echo burst-done\n");
                CHECK(b->r.wait([&] { return b->r.screen.text().find("burst-done") != std::string::npos; }, 15000));
                b->r.client->closeTerminal(t);
                CHECK(waitUntil([&] { return gone(shell); }, 5000));
                // The renderer going ends the relay; the host stays.
                b->r.closeInput();
                CHECK(waitUntil([&] { return b->done.load(); }, 5000));
                CHECK(waitUntil([&] { return rig.server->renderers() == 0; }, 5000));
                CHECK(rig.result == -1);
            }
            a.reset();
        }
        rig.stop();
        // No socket folder of the user's: nothing found, and a host that
        // cannot be started says why.
        CHECK(Shared::connectOrStartHost(rig.proj, {rig.dir},
                                         [](std::string* why) {
                                             *why = "no way";
                                             return 0;
                                         },
                                         1000, &error) < 0);
        CHECK(error == "no way");
    }
}
