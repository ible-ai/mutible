// session_lsp_tests.inc — stage 9's second slice: language servers for a
// window over SSH (SessionLsp.h: LspStart, LspInput, LspStop, LspStarted,
// LspOutput, LspExited, LspFiles, Welcome flag 512). Included after
// session_git_tests.inc, whose helpers (realScratch, fsWrite, Net, nowMs)
// it uses.

#include "SessionLsp.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

namespace {

// A fake disk for finding programs: folders and executables by path.
Shared::ServerFs fakeServerFs(std::set<std::string> dirs, std::set<std::string> exes,
                              std::map<std::string, std::vector<std::string>> lists = {}) {
    Shared::ServerFs fs;
    fs.isDir = [dirs](const std::string& p) { return dirs.count(p) > 0; };
    fs.isExecutable = [exes](const std::string& p) { return exes.count(p) > 0; };
    fs.list = [lists](const std::string& d) {
        auto it = lists.find(d);
        return it == lists.end() ? std::vector<std::string>() : it->second;
    };
    return fs;
}

Lsp::ProjectFs fakeProjectFs(std::set<std::string> files) {
    Lsp::ProjectFs fs;
    fs.exists = [files](const std::string& p) { return files.count(p) > 0; };
    fs.subfolders = [](const std::string&) { return std::vector<std::string>(); };
    return fs;
}

// What a server's listener heard.
struct HeardServer : Shared::Client::ServerListener {
    bool started = false, exited = false;
    int pid = 0, status = -2;
    std::string out, why;
    Json startedJson, info;
    void serverStarted(std::uint64_t, int p, const Json& j) override {
        started = true;
        pid = p;
        startedJson = j;
    }
    void serverOutput(std::uint64_t, const std::string& bytes) override { out += bytes; }
    void serverExited(std::uint64_t, int st, const std::string& w, const Json& j) override {
        exited = true;
        status = st;
        why = w;
        info = j;
    }
};

// The host's loop, for a Net (whose host has no thread): its servers'
// pipes polled and read, its tick, and every client's messages taken, until
// `done` or `ms` pass.
bool pumpServers(Net& net, double ms, const std::function<bool()>& done) {
    const double end = nowMs() + ms;
    for (;;) {
        for (auto& l : net.links) l->client->dispatch();
        if (done()) return true;
        if (nowMs() > end) return false;
        std::vector<Shared::Host::PollFd> fds;
        net.host.pollSet(&fds);
        std::vector<pollfd> polls;
        for (const auto& f : fds)
            polls.push_back(pollfd{f.fd, (short)((f.read ? POLLIN : 0) | (f.write ? POLLOUT : 0)), 0});
        if (!polls.empty()) poll(polls.data(), (nfds_t)polls.size(), 20);
        else usleep(10000);
        for (size_t i = 0; i < fds.size(); i++) {
            const short r = polls[i].revents;
            if (!r || !fds[i].server) continue;
            net.host.serverReady(fds[i], (r & (POLLIN | POLLHUP | POLLERR)) != 0, (r & (POLLOUT | POLLERR)) != 0);
        }
        net.host.tick();
    }
}

Shared::ServerRequest catRequest(const std::string& key = "custom") {
    Shared::ServerRequest r;
    r.key = key;
    r.commands = {{"no-such-language-server-here"}, {"cat"}};
    return r;
}

}  // namespace

static void testSessionLsp() {
    using Shared::FolderStamps;
    using Shared::StampEntry;

    GROUP("hostlsp:request");
    {
        Shared::ServerRequest r;
        r.key = "python";
        r.commands = {{"basedpyright-langserver", "--stdio"}, {"pylsp"}};
        r.allowed = true;
        r.fileDir = "/home/me/p/src";
        const Shared::ServerRequest back = Shared::serverRequestFrom(Shared::serverRequestJson(r));
        CHECK(back.key == "python" && back.commands == r.commands && back.allowed && back.fileDir == r.fileDir);
        // Empty commands are dropped; at most eight are tried.
        Json j = Json::object();
        Json cs = Json::array();
        cs.push(Json::array());
        Json one = Json::array();
        one.push(Json(""));
        cs.push(one);
        for (int i = 0; i < 12; i++) {
            Json w = Json::array();
            w.push(Json("s" + std::to_string(i)));
            cs.push(w);
        }
        j.set("commands", cs);
        const Shared::ServerRequest bad = Shared::serverRequestFrom(j);
        CHECK(bad.commands.size() == 8 && bad.commands[0][0] == "s0" && !bad.allowed);
    }

    GROUP("hostlsp:find");
    {
        const Shared::ServerFs fs = fakeServerFs(
            {"/usr/bin", "/bin", "/opt/x/bin", "/home/me/.local/bin", "/home/me/.cargo/bin",
             "/home/me/.nvm/versions/node/v18.20.4/bin", "/home/me/.nvm/versions/node/v22.17.0/bin",
             "/home/me/.nvm/versions/node/v9.0.0/bin", "/usr/local/bin"},
            {"/home/me/.nvm/versions/node/v22.17.0/bin/pyright-langserver",
             "/home/me/.nvm/versions/node/v18.20.4/bin/pyright-langserver", "/usr/bin/clangd",
             "/opt/x/bin/clangd", "/home/me/tools/srv"},
            {{"/home/me/.nvm/versions/node", {"v18.20.4", "v9.0.0", "v22.17.0", ".cache"}}});
        const std::vector<std::string> dirs = Shared::serverSearchDirs("/opt/x/bin:/missing:/usr/bin", "/home/me", fs);
        // PATH first, as it is; the missing folder left out; each once.
        CHECK(dirs.size() >= 3 && dirs[0] == "/opt/x/bin" && dirs[1] == "/usr/bin");
        CHECK(std::count(dirs.begin(), dirs.end(), "/usr/bin") == 1);
        CHECK(std::find(dirs.begin(), dirs.end(), "/missing") == dirs.end());
        auto at = [&](const std::string& d) { return std::find(dirs.begin(), dirs.end(), d) - dirs.begin(); };
        // ~/.local/bin and ~/.cargo/bin, which an ssh command's PATH lacks;
        // nvm's newest Node first.
        CHECK(at("/home/me/.local/bin") < (long)dirs.size() && at("/home/me/.cargo/bin") < (long)dirs.size());
        CHECK(at("/home/me/.nvm/versions/node/v22.17.0/bin") < at("/home/me/.nvm/versions/node/v18.20.4/bin"));
        CHECK(at("/home/me/.nvm/versions/node/v18.20.4/bin") < at("/home/me/.nvm/versions/node/v9.0.0/bin"));
        CHECK(Shared::findServerProgram("clangd", dirs, "/home/me", fs) == "/opt/x/bin/clangd");
        CHECK(Shared::findServerProgram("pyright-langserver", dirs, "/home/me", fs) ==
              "/home/me/.nvm/versions/node/v22.17.0/bin/pyright-langserver");
        CHECK(Shared::findServerProgram("gopls", dirs, "/home/me", fs).empty());
        CHECK(Shared::findServerProgram("~/tools/srv", dirs, "/home/me", fs) == "/home/me/tools/srv");
        CHECK(Shared::findServerProgram("/home/me/tools/srv", dirs, "/home/me", fs) == "/home/me/tools/srv");
        CHECK(Shared::findServerProgram("tools/srv", dirs, "/home/me", fs).empty());
    }

    GROUP("hostlsp:plan");
    {
        const Shared::ServerFs fs = fakeServerFs({"/usr/bin", "/home/me/.cargo/bin"},
                                                 {"/usr/bin/pylsp", "/home/me/.cargo/bin/rust-analyzer"});
        const std::vector<std::string> env = {"HOME=/home/me", "PATH=/usr/bin", "LANG=C.UTF-8"};
        Shared::ServerRequest r;
        r.key = "python";
        r.commands = {{"basedpyright-langserver", "--stdio"}, {"pyright-langserver", "--stdio"}, {"pylsp", "-v"}};
        Shared::ServerPlan p = Shared::planServer(r, "/home/me/p", "/home/me", env, fs, fakeProjectFs({}));
        CHECK(p.error.empty() && p.command == 2);
        CHECK((p.argv == std::vector<std::string>{"/usr/bin/pylsp", "-v"}));
        CHECK((p.tried == std::vector<std::string>{"basedpyright-langserver", "pyright-langserver"}));
        // Its environment: the host's, PATH the program's folder first.
        CHECK(std::count(p.env.begin(), p.env.end(), "LANG=C.UTF-8") == 1);
        std::string path;
        for (const std::string& e : p.env)
            if (e.rfind("PATH=", 0) == 0) path = e;
        CHECK(path.rfind("PATH=/usr/bin", 0) == 0 && path.find("/home/me/.cargo/bin") != std::string::npos);
        // None there.
        r.commands = {{"gopls"}};
        p = Shared::planServer(r, "/home/me/p", "/home/me", env, fs, fakeProjectFs({}));
        CHECK(p.error == "missing" && p.argv.empty() && (p.tried == std::vector<std::string>{"gopls"}));
        r.commands.clear();
        CHECK(Shared::planServer(r, "/home/me/p", "/home/me", env, fs, fakeProjectFs({})).error == "empty");
        // rust-analyzer: restricted without the folder's answer, waiting
        // with a cargo configuration there, free once allowed.
        r.key = "rust";
        r.commands = {{"rust-analyzer"}};
        p = Shared::planServer(r, "/home/me/p", "/home/me", env, fs, fakeProjectFs({}));
        CHECK(p.error.empty() && p.code.verdict == Lsp::ProjectCode::Restricted);
        const Shared::ServerStarted s = Shared::serverStartedFrom(Shared::startedJson(p));
        CHECK(s.command == 0 && s.argv == p.argv && s.code.verdict == Lsp::ProjectCode::Restricted);
        CHECK(s.code.settings["rust-analyzer"]["checkOnSave"].isBool() &&
              !s.code.settings["rust-analyzer"]["checkOnSave"].asBool());
        CHECK(!s.code.initializationOptions.isNull());
        const Lsp::ProjectFs cargo = fakeProjectFs({"/home/me/p/.cargo/config.toml"});
        p = Shared::planServer(r, "/home/me/p", "/home/me", env, fs, cargo);
        CHECK(p.error == "waits" && p.code.file == "/home/me/p/.cargo/config.toml");
        r.allowed = true;
        p = Shared::planServer(r, "/home/me/p", "/home/me", env, fs, cargo);
        CHECK(p.error.empty() && p.code.verdict == Lsp::ProjectCode::Free);
        CHECK(Shared::serverStartedFrom(Shared::startedJson(p)).code.verdict == Lsp::ProjectCode::Free);
        // A second server is never held back by the main one's rule.
        r.key = "rust.also";
        r.allowed = false;
        CHECK(Shared::planServer(r, "/home/me/p", "/home/me", env, fs, cargo).error.empty());
    }

    GROUP("hostlsp:stamps");
    {
        std::map<std::string, std::vector<StampEntry>> disk;
        auto lister = [&disk](const std::string& dir, std::vector<StampEntry>* out) {
            auto it = disk.find(dir);
            if (it == disk.end()) return false;
            *out = it->second;
            return true;
        };
        auto names = [](const std::vector<Lsp::FileChange>& cs) {
            std::vector<std::string> out;
            for (const auto& c : cs)
                out.push_back(c.path + ":" + std::to_string((int)c.type));
            std::sort(out.begin(), out.end());
            return out;
        };
        FolderStamps st;
        st.setSince(1000);
        disk["/p"] = {{"a.py", false, 500, 1}, {"b.py", false, 1500, 2}, {"src", true, 10, 0}, {".git", true, 2000, 0}};
        // Never looked at: what changed since the first server started, as
        // Changed (nothing says what was there before); .git left out.
        std::vector<Lsp::FileChange> out;
        st.look("/p", lister, &out);
        CHECK((names(out) == std::vector<std::string>{"/p/b.py:2"}));
        // Then compared: made, changed, gone, a file turned folder.
        disk["/p"] = {{"a.py", false, 600, 1}, {"c.py", false, 50, 3}, {"src", true, 99, 0}, {"b.py", true, 5, 0}};
        out.clear();
        st.look("/p", lister, &out);
        CHECK((names(out) == std::vector<std::string>{"/p/a.py:2", "/p/b.py:1", "/p/b.py:3", "/p/c.py:1"}));
        // Nothing changed: nothing reported (a folder's own time is not a change).
        disk["/p"][2].mtimeNs = 12345;
        out.clear();
        st.look("/p", lister, &out);
        CHECK(out.empty());
        // remember() reports nothing; a size change is a change.
        disk["/q"] = {{"x.rs", false, 10, 1}};
        st.remember("/q", lister);
        disk["/q"][0].size = 2;
        out.clear();
        st.look("/q", lister, &out);
        CHECK((names(out) == std::vector<std::string>{"/q/x.rs:2"}));
        // The folder gone: everything it held.
        disk.erase("/q");
        out.clear();
        st.look("/q", lister, &out);
        CHECK((names(out) == std::vector<std::string>{"/q/x.rs:3"}));
        CHECK(st.folders() == 1);
        // A real folder through listStamps.
        const std::string dir = realScratch();
        fsWrite(dir + "/one.txt", "1");
        FolderStamps real;
        real.setSince(0);
        real.remember(dir, Shared::listStamps);
        fsWrite(dir + "/two.txt", "2");
        std::filesystem::remove(dir + "/one.txt");
        out.clear();
        real.look(dir, Shared::listStamps, &out);
        CHECK((names(out) == std::vector<std::string>{dir + "/one.txt:3", dir + "/two.txt:1"}));
        // LspFiles's json, a bad entry dropped.
        Json changes = Shared::filesJson({{"/p/a.py", Lsp::FileChangeType::Created},
                                          {"/p/b", Lsp::FileChangeType::Deleted}})["changes"];
        changes.push(Json("bad"));
        Json rel = Json::array();
        rel.push(Json("relative"));
        rel.push(Json(1));
        changes.push(rel);
        Json j = Json::object();
        j.set("changes", changes);
        const std::vector<Lsp::FileChange> back = Shared::filesFrom(j);
        CHECK(back.size() == 2 && back[0].path == "/p/a.py" && back[0].type == Lsp::FileChangeType::Created &&
              back[1].type == Lsp::FileChangeType::Deleted);
    }

    GROUP("hostlsp:words");
    {
        CHECK(Shared::serversTooOldText("pi").find("newer mutible on pi") != std::string::npos);
        CHECK(Shared::serverMissingText("basedpyright", "thinkpad", {"basedpyright-langserver", "pylsp"}) ==
              "basedpyright is not installed on thinkpad (looked for basedpyright-langserver and pylsp).");
        CHECK(Shared::serverMissingText("x", "h", {"a", "b", "c"}) == "x is not installed on h (looked for a, b, and c).");
        CHECK(Shared::serverMissingText("", "h", {}) == "The language server is not installed on h.");
        CHECK(Shared::serversLostText("pi").find("connection to pi was lost") != std::string::npos);
        CHECK(Shared::serverExitText(0) == "exited with status 0");
        CHECK(Shared::serverExitText(1 << 8) == "exited with status 1");
        CHECK(Shared::serverExitText(SIGKILL) == "killed by signal 9");
        CHECK(Shared::serverExitText(-1) == "ended");
        // The status bar names the host for a server not installed there,
        // and a server the drop ended waits for the connection.
        Lsp::ServerStatus missing;
        missing.name = "basedpyright";
        missing.main = true;
        missing.state = Lsp::ServerState::NotInstalled;
        CHECK(Lsp::statusLine("Python", {missing}, 0, 0).text.find("Install a Python language server") == 0);
        missing.host = "thinkpad";
        CHECK(Lsp::statusLine("Python", {missing}, 0, 0).text == "Install a Python language server on thinkpad");
        Lsp::ServerStatus away;
        away.name = "clangd";
        away.main = true;
        away.state = Lsp::ServerState::Restarting;
        CHECK(Lsp::statusLine("C++", {away}, 0, 0).text.find("crashed") != std::string::npos);
        away.waitsForHost = true;
        CHECK(Lsp::statusLine("C++", {away}, 0, 0).text.find("clangd waits for the connection") != std::string::npos);
        // A server on another machine is given no parent process to watch.
        std::string sent;
        Lsp::Client client([&sent](const std::string& bytes) { sent += bytes; });
        client.initialize("/home/me/p", 0);
        CHECK(sent.find("\"processId\":null") != std::string::npos);
    }

    GROUP("hostlsp:process");
    {
        // cat: what goes in comes out; its input closed ends it.
        Shared::ServerProcess p;
        std::string err;
        CHECK(p.start({"/bin/cat"}, {"PATH=/bin:/usr/bin"}, "/", &err));
        CHECK(p.pid() > 0 && p.outFd() >= 0 && p.inFd() >= 0 && p.errFd() >= 0);
        p.write("Content-Length: 2\r\n\r\n{}");
        std::string got;
        const double end = nowMs() + 5000;
        while (got.size() < 23 && nowMs() < end) {
            pollfd f{p.outFd(), POLLIN, 0};
            poll(&f, 1, 50);
            p.readOut(got);
        }
        CHECK(got == "Content-Length: 2\r\n\r\n{}");
        p.closeInput();
        CHECK(p.inputClosed() && p.inFd() < 0);
        bool more = true;
        while (more && nowMs() < end + 5000) {
            pollfd f{p.outFd(), POLLIN, 0};
            poll(&f, 1, 50);
            std::string rest;
            more = p.readOut(rest);
        }
        CHECK(!more && p.outEnded());
        int status = -1;
        while (!p.reap(&status) && nowMs() < end + 5000) usleep(10000);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        // Its standard error kept, and its exit status.
        Shared::ServerProcess q;
        CHECK(q.start({"/bin/sh", "-c", "echo oops >&2; exit 3"}, {}, "/", &err));
        status = -1;
        while (!q.reap(&status) && nowMs() < end + 10000) usleep(10000);
        for (int i = 0; i < 20 && q.errTail().empty(); i++) {
            q.readErr();
            usleep(5000);
        }
        CHECK(q.errTail() == "oops\n" && WEXITSTATUS(status) == 3);
        // A signal to its group; a program not there.
        Shared::ServerProcess r;
        CHECK(r.start({"/bin/sleep", "30"}, {}, "", &err));
        r.signal(SIGKILL);
        status = -1;
        while (!r.reap(&status) && nowMs() < end + 15000) usleep(10000);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        Shared::ServerProcess none;
        CHECK(!none.start({"/no/such/server"}, {}, "", &err) && !err.empty());
    }

    GROUP("hostlsp:wire");
    {
        const std::string root = realScratch();
        fsWrite(root + "/a.py", "x = 1\n");
        Net net(true, true);   // through the encoding
        net.host.setFolder(root);
        net.host.setWatching(true);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
        // Before the Welcome nothing says the host runs servers: nothing sent.
        HeardServer early;
        CHECK(!app.client->startServer(1, 0, Shared::serverRequestJson(catRequest()), &early));
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        app.client->dispatch();
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(app.client->hostRunsServers());
        std::vector<Json> files;
        app.client->hearFiles([&files](std::uint64_t, const Json& j) { files.push_back(j); });

        // Started, the first command found on the host ("cat"); bytes both
        // ways (cat answers what it is sent).
        HeardServer s;
        const std::uint64_t id = app.client->newId();
        CHECK(app.client->startServer(id, 0, Shared::serverRequestJson(catRequest()), &s));
        CHECK(pumpServers(net, 5000, [&] { return s.started || s.exited; }));
        CHECK(s.started && s.pid > 0 && net.host.serverPid(id) == s.pid);
        const Shared::ServerStarted st = Shared::serverStartedFrom(s.startedJson);
        CHECK(st.command == 1 && !st.argv.empty() && st.argv[0].size() > 4 &&
              st.argv[0].compare(st.argv[0].size() - 4, 4, "/cat") == 0);
        app.client->serverInput(id, std::string("Content-Length: 2\r\n\r\n{}\x01\xff", 25));
        CHECK(pumpServers(net, 5000, [&] { return s.out.size() >= 25; }));
        CHECK(s.out == std::string("Content-Length: 2\r\n\r\n{}\x01\xff", 25));

        // The disk: a file made there comes as LspFiles (compared with what
        // the folder held when the server started).
        fsWrite(root + "/b.py", "y = 2\n");
        net.host.diskChanged({DiskWatch::Change{root, false}});
        CHECK(pumpServers(net, 2000, [&] { return !files.empty(); }));
        const std::vector<Lsp::FileChange> heard = files.empty() ? std::vector<Lsp::FileChange>()
                                                                 : Shared::filesFrom(files.back());
        CHECK(heard.size() == 1 && heard[0].path == root + "/b.py" && heard[0].type == Lsp::FileChangeType::Created);

        // Stopped: its input closed, cat ends, its status heard.
        app.client->stopServer(id);
        CHECK(pumpServers(net, 5000, [&] { return s.exited; }));
        CHECK(s.exited && s.why.empty() && WIFEXITED(s.status) && WEXITSTATUS(s.status) == 0);
        CHECK(net.host.serverCount() == 0 && !app.client->hasServer(id));

        // None of the commands there: "missing", with what was looked for.
        HeardServer missing;
        Shared::ServerRequest none;
        none.key = "go";
        none.commands = {{"no-such-gopls-here"}};
        CHECK(app.client->startServer(app.client->newId(), 0, Shared::serverRequestJson(none), &missing));
        CHECK(pumpServers(net, 3000, [&] { return missing.exited; }));
        CHECK(!missing.started && missing.why == "missing" && missing.info["tried"].size() == 1 &&
              missing.info["tried"][0].asString() == "no-such-gopls-here");

        // A guest and a reader are refused (the host's error), nothing run.
        HeardServer g, rd;
        CHECK(guest.client->startServer(guest.client->newId(), 0, Shared::serverRequestJson(catRequest()), &g));
        CHECK(reader.client->startServer(reader.client->newId(), 0, Shared::serverRequestJson(catRequest()), &rd));
        pumpServers(net, 200, [] { return false; });
        CHECK(!g.started && !rd.started && net.host.serverCount() == 0);
        bool refused = false;
        for (const std::string& e : guest.client->takeErrors())
            if (e.find("lsp.start") != std::string::npos) refused = true;
        CHECK(refused);

        // The renderer going ends its servers.
        HeardServer t;
        const std::uint64_t id2 = app.client->newId();
        CHECK(app.client->startServer(id2, 0, Shared::serverRequestJson(catRequest()), &t));
        CHECK(pumpServers(net, 5000, [&] { return t.started; }));
        const int pid2 = t.pid;
        net.host.disconnect(app.id);
        CHECK(pumpServers(net, 5000, [&] { return net.host.serverCount() == 0; }));
        CHECK(pid2 > 0 && ::kill(pid2, 0) != 0);

        // The host lost: a server's listener hears "lost" at the next dispatch.
        Net net2(true, true);
        net2.host.setFolder(root);
        Net::Link& app2 = net2.add(Unit::Utf16, "app", 4 | 8 | 16);
        app2.client->dispatch();
        HeardServer u;
        const std::uint64_t id3 = app2.client->newId();
        CHECK(app2.client->startServer(id3, 0, Shared::serverRequestJson(catRequest()), &u));
        CHECK(pumpServers(net2, 5000, [&] { return u.started; }));
        app2.client->hostLost();
        app2.client->dispatch();
        CHECK(u.exited && u.why == "lost" && !app2.client->hasServer(id3));
        HeardServer v;
        CHECK(!app2.client->startServer(app2.client->newId(), 0, Shared::serverRequestJson(catRequest()), &v));
        net2.host.disconnect(app2.id);
        pumpServers(net2, 5000, [&] { return net2.host.serverCount() == 0; });
        CHECK(net2.host.serverCount() == 0);
    }
}
