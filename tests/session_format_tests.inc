// session_format_tests.inc — stage 9's third slice: formatters for a window
// over SSH (SessionFormat.h: FsRead's "format", Welcome flag 1024,
// Client::requestFormat). Included after session_lsp_tests.inc; uses
// realScratch, fsWrite, fsRead, Net and nowMs from the files before it.

#include "SessionFormat.h"

namespace {

// A formatter request answered through a Net's host (its workers pumped
// here, as gitAsk does).
Shared::FormatReply formatAsk(Net& net, Net::Link& l, const Shared::FormatRequest& r, const std::string& text,
                              Json* raw = nullptr) {
    const std::uint64_t req = l.client->requestFormat(0, Shared::formatRequestJson(r), text);
    Shared::FormatReply none;
    none.message = "not sent";
    if (!req) return none;
    const double end = nowMs() + 15000;
    Json a;
    std::string bytes;
    while (nowMs() < end) {
        net.host.waitReads(50);
        l.client->dispatch();
        if (l.client->readAnswer(req, &a, &bytes)) {
            if (raw) *raw = a;
            return Shared::formatReplyFrom(a, std::move(bytes));
        }
    }
    none.message = "no answer";
    return none;
}

void writeScript(const std::string& path, const std::string& body) {
    fsWrite(path, "#!/bin/sh\n" + body);
    ::chmod(path.c_str(), 0755);
}

}  // namespace

static void testSessionFormat() {
    GROUP("hostformat:request");
    {
        Shared::FormatRequest r;
        r.path = "/r/p/a.py";
        r.setting = "ruff format -";
        r.consent = "allow";
        r.firstLine = 3;
        r.lastLine = 7;
        r.projectOnly = true;
        r.timeout = 12;
        const Shared::FormatRequest b = Shared::formatRequestFrom(Shared::formatRequestJson(r));
        CHECK(b.path == r.path && b.setting == r.setting && b.consent == "allow" && b.firstLine == 3 &&
              b.lastLine == 7 && b.projectOnly && b.timeout == 12);
        CHECK(Shared::formatRequestJson(r)["op"].asString() == "format");
        // What a request may not say: another answer, lines backwards, no end.
        Json j = Json::object();
        j.set("path", Json("/x.py"));
        j.set("consent", Json("yes"));
        j.set("first", Json((std::int64_t)5));
        j.set("last", Json((std::int64_t)2));
        j.set("timeout", Json(100000.0));
        const Shared::FormatRequest c = Shared::formatRequestFrom(j);
        CHECK(c.consent.empty() && c.firstLine == 0 && c.lastLine == 0 && c.timeout == Shared::kMaxFormatSeconds);
    }

    GROUP("hostformat:setting");
    {
        using Shared::formatSettingRefusal;
        const std::vector<std::string>& known = Formatter::knownPrograms();
        CHECK(std::is_sorted(known.begin(), known.end()));
        for (const char* p : {"black", "biome", "clang-format", "gofmt", "latexindent", "prettier", "pyink", "ruff",
                              "rustfmt", "shfmt", "yapf"})
            CHECK(Formatter::isKnownProgram(p));
        CHECK(!Formatter::isKnownProgram("sed") && !Formatter::isKnownProgram("npx"));
        for (const char* ok : {"", "off", "ruff format --stdin-filename {file} -", "/opt/tools/black -q -",
                               "npx prettier --stdin-filepath {file}", "pnpm exec biome format",
                               "~/bin/shfmt -i 4", "prettier"})
            CHECK(formatSettingRefusal("python", ok).empty());
        for (const char* no : {"sed -e s/a/b/", "sh -c 'rm -rf x'", "npm run fmt", "/tmp/evil {file}"}) {
            const std::string why = formatSettingRefusal("python", no);
            CHECK(why.find("format.python runs ") == 0 && why.find("does not run for a window") != std::string::npos);
        }
    }

    GROUP("hostformat:run");
    {
        const std::string top = realScratch();
        const std::string folder = top + "/proj", bin = top + "/bin", home = top + "/home", mark = top + "/mark.txt";
        std::filesystem::create_directories(folder + "/sub");
        std::filesystem::create_directories(bin);
        std::filesystem::create_directories(home);
        // The table's shfmt, as a stand-in on the host's PATH: it uppercases,
        // and says where it ran and with what.
        writeScript(bin + "/shfmt", "{ pwd -P; printf '%s\\n' \"$@\"; } > \"$MARK\"\ntr a-z A-Z\n");
        const std::vector<std::string> env = {"PATH=" + bin + ":/usr/bin:/bin", "HOME=" + home, "MARK=" + mark};
        const std::string file = folder + "/sub/x.sh";
        fsWrite(file, "echo hi\n");
        Shared::FormatRequest r;
        r.path = file;
        auto run = [&](const Shared::FormatRequest& q, const std::string& text, const std::atomic<bool>* cancel = nullptr) {
            return Shared::runFormatRequest(folder, Shared::formatRequestJson(q), text, env, cancel);
        };
        Shared::FormatAnswer a = run(r, "echo hi\n");
        CHECK(a.json["kind"].asString() == "run" && a.json["ok"].asBool());
        CHECK(a.out == "ECHO HI\n");
        CHECK(a.json["tool"].asString() == "shfmt" && a.json["style"].asString() == "Google style");
        // In the file's folder, Google's shell style, the file named.
        CHECK(fsRead(mark) == folder + "/sub\n-i\n2\n-ci\n-bn\n--filename\n" + file + "\n");
        Shared::FormatReply reply = Shared::formatReplyFrom(a.json, std::move(a.out));
        CHECK(reply.kind == "run" && reply.ok && reply.text == "ECHO HI\n" && reply.message.empty());

        // A setting naming one of the table's programs runs as written.
        r.setting = bin + "/shfmt -i 4";
        a = run(r, "x\n");
        CHECK(a.json["ok"].asBool() && a.out == "X\n" && a.json["style"].asString() == "format.shell in the settings");
        CHECK(fsRead(mark).find("\n-i\n4\n") != std::string::npos);
        // One that is not one of them is refused, nothing run.
        std::filesystem::remove(mark);
        r.setting = "tr a-z A-Z";
        a = run(r, "x\n");
        CHECK(a.json["reason"].asString() == "setting" && a.out.empty());
        CHECK(!std::filesystem::exists(mark));
        // One not installed there: missing.
        r.setting = "/nonexistent/shfmt -i 2";
        a = run(r, "x\n");
        CHECK(a.json["kind"].asString() == "failed" && a.json["missing"].asBool());
        reply = Shared::formatReplyFrom(a.json, std::string());
        CHECK(reply.missing && reply.message.find("was not found") != std::string::npos);
        CHECK(Shared::formatMissingText(reply.message, "pi").find("run on pi: install one there") != std::string::npos);
        r.setting.clear();

        // A formatter's complaint, and a run stopped.
        writeScript(bin + "/gofmt", "cat >/dev/null\necho 'x.go:1:1: expected package' >&2\nexit 2\n");
        Shared::FormatRequest g;
        g.path = folder + "/x.go";
        a = run(g, "nope\n");
        CHECK(a.json["kind"].asString() == "run" && !a.json["ok"].asBool());
        CHECK(a.json["message"].asString().find("gofmt could not format x.go:\n") == 0);
        CHECK(a.json["message"].asString().find("expected package") != std::string::npos);
        writeScript(bin + "/rustfmt", "sleep 20\n");
        Shared::FormatRequest rs;
        rs.path = folder + "/x.rs";
        std::atomic<bool> stop{true};
        const double t0 = nowMs();
        a = run(rs, "fn main() {}\n", &stop);
        CHECK(!a.json["ok"].asBool() && a.json["message"].asString() == "rustfmt was stopped.");
        CHECK(nowMs() - t0 < 5000);
        rs.timeout = 1;
        a = run(rs, "fn main() {}\n");
        CHECK(!a.json["ok"].asBool() && a.json["message"].asString().find("took longer than 1 seconds") != std::string::npos);

        // Nothing planned for a text file: a language server may.
        Shared::FormatRequest t;
        t.path = folder + "/notes.txt";
        a = run(t, "x");
        CHECK(a.json["kind"].asString() == "unavailable" && a.json["serverMayFormat"].asBool());

        // The folder's own program waits for the window's answer (kept per
        // host and folder on the Mac); allowed, it runs.
        std::filesystem::create_directories(folder + "/.venv/bin");
        writeScript(folder + "/.venv/bin/black", "tr a-z A-Z\n");
        fsWrite(folder + "/pyproject.toml", "[tool.black]\nline-length = 100\n");
        Shared::FormatRequest py;
        py.path = folder + "/app.py";
        a = run(py, "x = 1\n");
        CHECK(a.json["kind"].asString() == "consent" && a.json["tool"].asString() == "black" &&
              a.json["consentFolder"].asString() == ".venv");
        py.consent = "allow";
        a = run(py, "x = 1\n");
        CHECK(a.json["ok"].asBool() && a.out == "X = 1\n" && a.json["style"].asString() == "the project's pyproject.toml");

        // Only in the session's folder: not outside, not through a link out.
        const std::string elsewhere = top + "/elsewhere";
        std::filesystem::create_directories(elsewhere);
        Shared::FormatRequest o;
        o.path = elsewhere + "/x.sh";
        CHECK(run(o, "x").json["reason"].asString() == "outside");
        CHECK(::symlink(elsewhere.c_str(), (folder + "/out").c_str()) == 0);
        o.path = folder + "/out/x.sh";
        CHECK(run(o, "x").json["reason"].asString() == "outside");
        o.path = "sub/x.sh";
        CHECK(run(o, "x").json["reason"].asString() == "args");
        // A file not there yet formats in its folder.
        o.path = folder + "/sub/new.sh";
        a = run(o, "y\n");
        CHECK(a.json["ok"].asBool() && a.out == "Y\n");
    }

    GROUP("hostformat:wire");
    {
        const std::string top = realScratch();
        const std::string folder = top + "/proj", bin = top + "/bin";
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(bin);
        // Through the encoding, on the Net's host, whose environment is
        // this process's: the formatter named by its path in the setting.
        writeScript(bin + "/shfmt", "tr a-z A-Z\n");
        Net net(true, true);
        net.host.setFolder(folder);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        app.client->dispatch();
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(app.client->hostFormats());
        Shared::FormatRequest r;
        r.path = folder + "/a.sh";
        r.setting = bin + "/shfmt";
        Shared::FormatReply reply = formatAsk(net, app, r, "echo é\n");
        CHECK(reply.ok && reply.text == "ECHO é\n" && reply.tool == "shfmt");
        // Never from a page.
        Json raw;
        reply = formatAsk(net, guest, r, "x", &raw);
        CHECK(!reply.ok && reply.kind == "error" && raw["reason"].asString() == "readonly");
        reply = formatAsk(net, reader, r, "x", &raw);
        CHECK(!reply.ok && raw["reason"].asString() == "readonly");
        // An older host: no flag, nothing sent.
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.flag = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512;
        app.client->deliver(std::move(w));
        app.client->dispatch();
        CHECK(!app.client->hostFormats() && app.client->requestFormat(0, Shared::formatRequestJson(r), "x") == 0);
        CHECK(Shared::formatTooOldText("pi:~/p").find("newer mutible on pi:~/p") != std::string::npos);
        CHECK(Shared::formatLostText("pi").find("connection to pi is lost") != std::string::npos);
    }
}
