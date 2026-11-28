// session_typeset_tests.inc — the LaTeX preview over SSH's first slice: the
// host typesets a window's document with its own tectonic (SessionTypeset.h:
// FsRead's "typeset" and "typesetStop", Welcome flag 8192,
// Client::requestTypeset). Included after session_format_tests.inc; uses
// realScratch, fsWrite, fsRead, writeScript, Net and nowMs from the files
// before it. The tectonic is a stand-in script (MUTIBLE_TECTONIC), never the
// real one.

#include "SessionTypeset.h"
#include "LatexScratch.h"
#include "Backups.h"

#include <sys/wait.h>

namespace {

// The environment of this process, put back at the end (the host's workers
// read it when a request comes).
struct TypesetEnv {
    std::vector<std::pair<std::string, std::string>> saved;
    std::vector<std::string> unset;
    void set(const std::string& name, const std::string& value) {
        if (const char* old = std::getenv(name.c_str())) saved.push_back({name, old});
        else unset.push_back(name);
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    ~TypesetEnv() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) ::setenv(it->first.c_str(), it->second.c_str(), 1);
        for (const std::string& n : unset) ::unsetenv(n.c_str());
    }
};

// The names in a folder (not . and ..), sorted.
std::vector<std::string> typesetNames(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool typesetHasSibling(const std::string& dir) {
    for (const std::string& n : typesetNames(dir))
        if (n.size() > 12 && n[0] == '.' && n.compare(n.size() - 12, 12, ".mutible.tex") == 0) return true;
    return false;
}

bool typesetWaitFor(Net& net, Net::Link& l, const std::function<bool()>& done, int ms = 15000) {
    const double end = nowMs() + ms;
    while (nowMs() < end) {
        net.host.waitReads(20);
        l.client->dispatch();
        if (done()) return true;
    }
    return done();
}

Shared::TypesetReply typesetAsk(Net& net, Net::Link& l, Shared::DocId doc, const std::string& view,
                                std::uint64_t gen, double timeout = 0, bool* sent = nullptr) {
    Shared::TypesetReply r;
    r.error = "not sent";
    const std::uint64_t req = l.client->requestTypeset(doc, 0, view, gen, timeout);
    if (sent) *sent = req != 0;
    if (!req) return r;
    const bool got = typesetWaitFor(net, l, [&] { return l.client->typesetAnswer(req, &r); });
    if (!got) r.error = "no answer";
    return r;
}

}  // namespace

static void testSessionTypeset() {
    GROUP("hosttypeset:run");
    {
        const std::string top = realScratch();
        const std::string folder = top + "/proj", bin = top + "/bin", home = top + "/home", mark = top + "/mark",
                          tmp = top + "/tmp";
        for (const std::string& d : {folder + "/sub", bin, home, mark, tmp}) std::filesystem::create_directories(d);
        // tectonic: records what it was given and where it ran, and writes
        // what tectonic writes; the text decides what else it does.
        writeScript(bin + "/tectonic",
                    "MARK='" + mark + "'\n"
                    "last=; prev=; out=\n"
                    "for a in \"$@\"; do if [ \"$prev\" = --outdir ]; then out=$a; fi; prev=$a; last=$a; done\n"
                    "printf '%s\\n' \"$@\" > $MARK/args.txt\n"
                    "cat \"$last\" > $MARK/text.txt\n"
                    "pwd -P > $MARK/cwd.txt\n"
                    "ls -a \"$(dirname \"$last\")\" > $MARK/ls.txt\n"
                    "ls \"$out\" > $MARK/outls.txt\n"
                    "if grep -q LOOP \"$last\"; then echo $$ > $MARK/loop.pid; while :; do sleep 1; done; fi\n"
                    "stem=${last%.tex}; stem=${stem##*/}\n"
                    "if grep -q BIGPDF \"$last\"; then head -c 70000000 /dev/zero > \"$out/$stem.pdf\"; exit 0; fi\n"
                    "if grep -q FAIL \"$last\"; then echo 'Error: it failed' >&2; exit 1; fi\n"
                    "printf 'FAKEPDF %s' \"$(cat \"$last\")\" > \"$out/$stem.pdf\"\n"
                    "printf 'FAKESYNC' > \"$out/$stem.synctex.gz\"\n"
                    "echo 'TRANSCRIPT' > \"$out/$stem.log\"\n"
                    "echo 'typeset ok'\n"
                    "echo 'a warning' >&2\n");
        TypesetEnv env;
        env.set("MUTIBLE_TECTONIC", bin + "/tectonic");
        env.set("TMPDIR", tmp);
        const std::string work = tmp + "/mutible-host-latex-" + std::to_string((unsigned long)::getuid());

        Net net(true, true);
        net.host.setFolder(folder);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        app.client->dispatch();
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(app.client->hostTypesets() && Shared::kHostTypesets == 8192);

        // The held text, not the file's, is typeset beside the file.
        const std::string file = folder + "/sub/paper.tex";
        fsWrite(file, "ON DISK\n");
        TestMirror m1;
        m1.client = app.client.get();
        m1.doc = 1;
        app.client->open(1, file, "\\documentclass{article} HELD\n", &m1);
        app.client->dispatch();
        Shared::TypesetReply r = typesetAsk(net, app, 1, "v1", 7);
        CHECK(r.ok && r.error.empty());
        CHECK(r.pdf == "FAKEPDF \\documentclass{article} HELD" && r.synctex == "FAKESYNC" && r.transcript == "TRANSCRIPT\n");
        CHECK(r.gen == 7 && r.view == "v1" && r.status == 0);
        CHECK(r.log.find("typeset ok") != std::string::npos && r.log.find("a warning") != std::string::npos);
        CHECK(r.scratch.compare(0, folder.size() + 16, folder + "/sub/.paper.tex.") == 0 &&
              r.scratch.size() > 12 && r.scratch.compare(r.scratch.size() - 12, 12, ".mutible.tex") == 0);
        CHECK(!r.stem.empty() && r.scratch == folder + "/sub/" + r.stem + ".tex");
        CHECK(fsRead(mark + "/text.txt") == "\\documentclass{article} HELD\n");
        CHECK(fsRead(file) == "ON DISK\n");
        // The apps' arguments, in the file's folder, the sibling beside it.
        {
            std::istringstream in(fsRead(mark + "/args.txt"));
            std::vector<std::string> a;
            for (std::string line; std::getline(in, line);) a.push_back(line);
            const std::vector<std::string> want = {"--untrusted", "--synctex", "--keep-logs", "--chatter", "minimal",
                                                   "--color", "never", "--outdir"};
            CHECK(a.size() == 10 && std::equal(want.begin(), want.end(), a.begin()));
            CHECK(a.size() == 10 && a[8].compare(0, work.size() + 1, work + "/") == 0 && a[9] == r.scratch);
        }
        CHECK(fsRead(mark + "/cwd.txt") == std::filesystem::canonical(folder + "/sub").string() + "\n");
        CHECK(fsRead(mark + "/ls.txt").find(r.stem + ".tex") != std::string::npos);   // there while it ran
        // And gone after: no sibling in the repository, no folder in TMPDIR.
        CHECK(!typesetHasSibling(folder + "/sub") && typesetNames(folder + "/sub") == std::vector<std::string>{"paper.tex"});
        CHECK(typesetNames(work).empty());
        // What the window writes where its viewer reads them.
        {
            Shared::TypesetFiles files;
            std::string err;
            CHECK(Shared::writeTypesetFiles(r, top + "/local/work", true, &files, &err) && err.empty());
            CHECK(files.pdf == top + "/local/work/" + r.stem + ".pdf" && fsRead(files.pdf) == r.pdf);
            CHECK(files.synctex == top + "/local/work/" + r.stem + ".synctex.gz" && fsRead(files.synctex) == "FAKESYNC");
            CHECK(fsRead(files.transcript) == "TRANSCRIPT\n");
        }

        // A run that fails says so, with the log.
        {
            TestMirror m;
            m.client = app.client.get();
            m.doc = 2;
            fsWrite(folder + "/bad.tex", "x");
            app.client->open(2, folder + "/bad.tex", "FAIL\n", &m);
            app.client->dispatch();
            Shared::TypesetReply f = typesetAsk(net, app, 2, "vbad", 1);
            CHECK(!f.ok && f.status == 1 && f.log.find("Error: it failed") != std::string::npos && !f.error.empty());
            CHECK(!typesetHasSibling(folder) && typesetNames(work).empty());
            // A PDF past the limit is refused, with why.
            TestMirror b;
            b.client = app.client.get();
            b.doc = 3;
            fsWrite(folder + "/big.tex", "x");
            app.client->open(3, folder + "/big.tex", "BIGPDF\n", &b);
            app.client->dispatch();
            Shared::TypesetReply g = typesetAsk(net, app, 3, "vbig", 1);
            CHECK(!g.ok && g.pdf.empty() && g.error.find("64 MB") != std::string::npos);
            CHECK(!typesetHasSibling(folder) && typesetNames(work).empty());
        }

        // Only an app's window; a document the host holds.
        {
            const std::uint64_t q = guest.client->requestTypeset(1, 0, "g", 1);
            CHECK(q != 0);   // a page's client may send it; the host refuses
            Shared::TypesetReply gr;
            typesetWaitFor(net, guest, [&] { return guest.client->typesetAnswer(q, &gr); });
            CHECK(!gr.ok && gr.error.find("app's window") != std::string::npos);
            Shared::TypesetReply none = typesetAsk(net, app, 99, "vx", 1);
            CHECK(!none.ok && none.error.find("does not hold") != std::string::npos);
            CHECK(!typesetHasSibling(folder + "/sub"));
        }

        // A newer typeset of the preview replaces the one running, which is
        // stopped (its group killed) and answers so; its sibling goes first.
        {
            const std::string looped = folder + "/loop.tex";
            fsWrite(looped, "x");
            TestMirror a1, a2;
            a1.client = a2.client = app.client.get();
            a1.doc = 4;
            a2.doc = 5;
            app.client->open(4, looped, "LOOP\n", &a1);
            fsWrite(folder + "/next.tex", "x");
            app.client->open(5, folder + "/next.tex", "NEXT\n", &a2);
            app.client->dispatch();
            ::unlink((mark + "/loop.pid").c_str());
            const std::uint64_t first = app.client->requestTypeset(4, 0, "vs", 1);
            CHECK(first != 0);
            CHECK(typesetWaitFor(net, app, [&] { return std::filesystem::exists(mark + "/loop.pid"); }));
            CHECK(typesetHasSibling(folder));
            const int loopPid = std::atoi(fsRead(mark + "/loop.pid").c_str());
            Shared::TypesetReply second = typesetAsk(net, app, 5, "vs", 2);
            CHECK(second.ok && second.gen == 2 && second.pdf == "FAKEPDF NEXT");
            Shared::TypesetReply older;
            CHECK(typesetWaitFor(net, app, [&] { return app.client->typesetAnswer(first, &older); }));
            CHECK(!older.ok && older.superseded && older.gen == 1);
            CHECK(loopPid > 0 && typesetWaitFor(net, app, [&] { return ::kill(loopPid, 0) != 0; }, 5000));
            CHECK(!typesetHasSibling(folder) && typesetNames(work).empty());
        }

        // The time limit: a run that loops is stopped, and the answer says so.
        {
            ::unlink((mark + "/loop.pid").c_str());
            const double t0 = nowMs();
            Shared::TypesetReply t = typesetAsk(net, app, 4, "vt", 1, 1.0);
            CHECK(!t.ok && t.stopped && t.error.find("1 second") != std::string::npos);
            CHECK(nowMs() - t0 < 8000);
            const int loopPid = std::atoi(fsRead(mark + "/loop.pid").c_str());
            CHECK(loopPid > 0 && typesetWaitFor(net, app, [&] { return ::kill(loopPid, 0) != 0; }, 5000));
            CHECK(!typesetHasSibling(folder) && typesetNames(work).empty());
        }

        // The preview closed (TypesetStop), the document closed, and the window going each end
        // a run and take its sibling and folder.
        for (int how = 0; how < 3; how++) {
            Net::Link& w = how == 2 ? net.add(Unit::Utf16, "gone", 4 | 8 | 16) : app;
            w.client->dispatch();
            Shared::DocId d = (Shared::DocId)(10 + how);
            const std::string f = folder + "/stop" + std::to_string(how) + ".tex";
            fsWrite(f, "x");
            TestMirror sm;
            sm.client = w.client.get();
            sm.doc = d;
            w.client->open(d, f, "LOOP\n", &sm);
            w.client->dispatch();
            ::unlink((mark + "/loop.pid").c_str());
            const std::uint64_t req = w.client->requestTypeset(d, 0, "vc", 1);
            CHECK(req != 0);
            CHECK(typesetWaitFor(net, w, [&] { return std::filesystem::exists(mark + "/loop.pid"); }));
            CHECK(typesetHasSibling(folder) && !typesetNames(work).empty());
            const int loopPid = std::atoi(fsRead(mark + "/loop.pid").c_str());
            if (how == 0) w.client->stopTypeset("vc");
            if (how == 1) w.client->close(d);
            if (how == 2) net.host.disconnect(w.id);
            CHECK(typesetWaitFor(net, app, [&] { return !typesetHasSibling(folder) && typesetNames(work).empty(); }));
            CHECK(typesetWaitFor(net, app, [&] { return ::kill(loopPid, 0) != 0; }, 5000));
            if (how < 2) {
                Shared::TypesetReply sr;
                CHECK(typesetWaitFor(net, w, [&] { return w.client->typesetAnswer(req, &sr); }));
                CHECK(!sr.ok && sr.stopped);
            }
        }
    }

    GROUP("hosttypeset:missing");
    {
        const std::string top = realScratch();
        const std::string folder = top + "/proj", bin = top + "/bin", home = top + "/home";
        for (const std::string& d : {folder, bin, home, top + "/tmp"}) std::filesystem::create_directories(d);
        TypesetEnv env;
        env.set("MUTIBLE_TECTONIC", top + "/nowhere/tectonic");
        env.set("PATH", bin);
        env.set("HOME", home);
        env.set("XDG_DATA_HOME", home + "/data");
        env.set("TMPDIR", top + "/tmp");
        std::string tried;
        if (!Shared::findTectonic({}, &tried).empty()) {
            // A real tectonic in one of the usual folders of this machine: nothing to check.
        } else {
            CHECK(tried.find("MUTIBLE_TECTONIC (" + top + "/nowhere/tectonic)") != std::string::npos &&
                  tried.find("PATH and the usual folders") != std::string::npos);
            Net net(true, true);
            net.host.setFolder(folder);
            Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
            app.client->dispatch();
            fsWrite(folder + "/a.tex", "x");
            TestMirror m;
            m.client = app.client.get();
            m.doc = 1;
            app.client->open(1, folder + "/a.tex", "x", &m);
            app.client->dispatch();
            Shared::TypesetReply r = typesetAsk(net, app, 1, "v", 3);
            CHECK(!r.ok && r.missing && r.gen == 3);
            CHECK(r.error.find("tectonic is not installed on ") == 0 && r.error.find("MUTIBLE_TECTONIC") != std::string::npos);
            CHECK(r.error.find("PATH") != std::string::npos);
            CHECK(!typesetHasSibling(folder));
        }
        // Found there when it is in the managed folder, and on the PATH.
        std::filesystem::create_directories(home + "/data/mutible/bin");
        writeScript(home + "/data/mutible/bin/tectonic", "exit 0\n");
#ifndef __APPLE__
        CHECK(Shared::findTectonic({}, &tried) == home + "/data/mutible/bin/tectonic");
#endif
        ::unsetenv("MUTIBLE_TECTONIC");
        writeScript(bin + "/tectonic", "exit 0\n");
        env.set("XDG_DATA_HOME", home + "/nodata");
        CHECK(Shared::findTectonic({}, &tried) == bin + "/tectonic");
        CHECK(Shared::findTectonic({"MUTIBLE_TECTONIC=" + bin + "/tectonic"}, &tried) == bin + "/tectonic");
    }

    GROUP("hosttypeset:sweep");
    {
        const std::string top = realScratch();
        const std::string folder = top + "/proj", tmp = top + "/tmp";
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(tmp);
        TypesetEnv env;
        env.set("TMPDIR", tmp);
        const std::string base = Shared::typesetBaseFolder();
        CHECK(!base.empty() && base == tmp + "/mutible-host-latex-" + std::to_string((unsigned long)::getuid()));
        // A process that has gone.
        const pid_t child = ::fork();
        if (child == 0) ::_exit(0);
        int status = 0;
        ::waitpid(child, &status, 0);
        auto plant = [&](const LatexScratch::Owner& owner, const std::string& name, const std::string& sibling,
                         const std::string& bytes, const std::string& recorded) {
            LatexScratch::Record rec;
            rec.sibling = sibling;
            rec.pid = owner.pid;
            rec.start = (double)owner.start;
            rec.size = recorded.size();
            rec.sha256 = Shared::typesetSha256Hex(recorded);
            const std::string dir = base + "/" + LatexScratch::folderName(Shared::typesetSha256Hex(name).substr(0, 16), owner);
            std::filesystem::create_directories(dir);
            fsWrite(dir + "/sibling.record", LatexScratch::format(rec));
            fsWrite(dir + "/x.pdf", "pdf");
            if (!sibling.empty()) fsWrite(sibling, bytes);
            return dir;
        };
        LatexScratch::Owner dead;
        dead.pid = child;
        dead.start = 1;
        dead.view = 1;
        const std::string ours = folder + "/.a.tex.1-1.mutible.tex";
        const std::string changed = folder + "/.b.tex.1-2.mutible.tex";
        const std::string users = folder + "/c.tex";
        const std::string d1 = plant(dead, "a", ours, "the copy", "the copy");
        dead.view = 2;
        const std::string d2 = plant(dead, "b", changed, "edited since", "the copy");
        dead.view = 3;
        const std::string d3 = plant(dead, "c", users, "the user's own", "the user's own");   // not a preview copy's name
        LatexScratch::Owner live;
        live.pid = (long long)::getpid();
        live.start = (long long)Backups::processStart((int64_t)::getpid());
        live.view = 9;
        const std::string d4 = plant(live, "d", folder + "/.d.tex.1-9.mutible.tex", "mine", "mine");
        CHECK(std::filesystem::exists(d1) && std::filesystem::exists(ours));
        {
            Net net(true, true);   // a host starting
            CHECK(!std::filesystem::exists(d1) && !std::filesystem::exists(d2) && !std::filesystem::exists(d3));
            CHECK(!std::filesystem::exists(ours));                 // removed: still exactly its bytes
            CHECK(std::filesystem::exists(changed));               // left: not the bytes written
            CHECK(std::filesystem::exists(users));                 // left: not named as a preview's copy
            CHECK(std::filesystem::exists(d4) && std::filesystem::exists(folder + "/.d.tex.1-9.mutible.tex"));   // its writer is alive
        }
    }

    GROUP("hosttypeset:old");
    {
        const std::string top = realScratch();
        TypesetEnv env;
        env.set("TMPDIR", top);
        Net net(true, true);
        net.host.setFolder(top);
        Net::Link& app = net.add(Unit::Utf16, "app", 4);
        app.client->dispatch();
        CHECK(app.client->hostTypesets());
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.flag = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 4096;
        app.client->deliver(std::move(w));
        app.client->dispatch();
        CHECK(!app.client->hostTypesets() && app.client->requestTypeset(1, 0, "v", 1) == 0);
        CHECK(Shared::typesetTooOldText("pi:~/p").find("newer mutible on pi:~/p") != std::string::npos);
        CHECK(Shared::typesetLostText("pi").find("connection to pi is lost") != std::string::npos);
        // The answer's shape: sizes that do not add up are not a PDF.
        Json j = Json::object();
        j.set("ok", Json(true));
        j.set("pdf", Json((std::int64_t)5));
        j.set("stem", Json("s"));
        Shared::TypesetReply r = Shared::typesetReplyFrom(j, "abc");
        CHECK(!r.ok && !r.error.empty());
    }
}
