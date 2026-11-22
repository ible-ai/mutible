// session_git_tests.inc — stage 9's first slice: git for a window over SSH
// (SessionGit.h, the host's FsRead "git", Welcome flag 256,
// Client::requestGit), and the Discard words over SSH (GitModel.h's
// discardPlanOverSsh). Included after session_fileops_tests.inc, whose
// helpers (realScratch, fsWrite, Net) it uses.

#include "GitModel.h"
#include "SessionGit.h"
#include "SessionWebGit.h"

namespace {

// Runs a git request on the host and waits for its one answer (a Net's
// host has no thread of its own, so its workers are pumped here).
Shared::GitReply gitAsk(Net& net, Net::Link& l, const std::string& dir, const std::vector<std::string>& args,
                        const std::string& input = std::string(), Json* raw = nullptr) {
    const std::uint64_t req = l.client->requestGit(0, Shared::gitRequest(dir, args), input);
    Shared::GitReply none;
    none.err = "not sent";
    if (!req) return none;
    const double end = nowMs() + 15000;
    Json a;
    std::string bytes;
    while (nowMs() < end) {
        net.host.waitReads(50);
        l.client->dispatch();
        if (l.client->readAnswer(req, &a, &bytes)) {
            if (raw) *raw = a;
            return Shared::gitReplyFrom(a, std::move(bytes));
        }
    }
    none.err = "no answer";
    return none;
}

bool gitSh(const std::string& dir, const std::string& c) {
    std::string q = "'";
    for (char ch : dir) q += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    q += "'";
    return std::system(("cd " + q + " && " + c + " >/dev/null 2>&1").c_str()) == 0;
}

}  // namespace

static void testSessionGit() {
    using Shared::gitArgsRefusal;

    GROUP("hostgit:args");
    {
        // What the apps run passes.
        GitUi::Row changed;
        changed.path = "a.txt";
        changed.letter = 'M';
        GitUi::Row fresh = changed;
        fresh.path = "n.txt";
        fresh.untracked = true;
        GitUi::Row staged = changed;
        staged.staged = true;
        for (const std::vector<std::string>& ok :
             {GitUi::topLevelArgs(), GitUi::statusArgs(), GitUi::diffArgs(changed), GitUi::diffArgs(fresh),
              GitUi::diffArgs(staged), GitUi::stageArgs(changed, false), GitUi::stageArgs(staged, false),
              GitUi::stageArgs(staged, true), GitUi::commitArgs("a message"), GitUi::refArgs(),
              GitUi::logArgs(50, true, true), GitUi::remoteArgs(), GitUi::indexEntryArgs("a.txt"),
              GitUi::blobArgs("0123abcd"), GitUi::hashObjectArgs("a.txt"), GitUi::keepCopyArgs("a.txt"),
              GitUi::discardPlan(changed).args, GitUi::stashListArgs(), GitUi::headParentsArgs(),
              Git::decorationArgs(), ChangeReview::batchArgs(), ChangeReview::lsFilesArgs({"a.txt"}),
              std::vector<std::string>{"reset", "--soft", "HEAD~1"},
              std::vector<std::string>{"update-ref", "-d", "HEAD"},
              std::vector<std::string>{"stash", "push", "--include-untracked"},
              std::vector<std::string>{"stash", "pop"}, std::vector<std::string>{"apply", "--cached", "-"},
              std::vector<std::string>{"switch", "topic"},
              std::vector<std::string>{"rev-parse", "--show-toplevel", "--show-prefix"}}) {
            const std::string why = gitArgsRefusal(ok);
            CHECK(why.empty());
            if (!why.empty()) std::fprintf(stderr, "  refused: %s (%s)\n", Git::commandLine(ok).c_str(), why.c_str());
        }
        // What would point git elsewhere, run a program, write outside, or
        // lose work does not.
        for (const std::vector<std::string>& no : std::vector<std::vector<std::string>>{
                 {},
                 {"-C", "/etc", "status"},
                 {"--git-dir=/x", "status"},
                 {"--work-tree", "/x", "status"},
                 {"-c", "core.sshCommand=touch /tmp/x", "status"},
                 {"-c", "core.fsmonitor=x", "status"},
                 {"-c"},
                 {"clean", "-fdx"},
                 {"checkout", "--", "."},
                 {"reset", "--hard"},
                 {"rm", "a.txt"},
                 {"diff", "--output=/tmp/x"},
                 {"diff", "--ext-diff"},
                 {"diff", "--no-index", "--", "/etc/passwd", "a.txt"},
                 {"diff", "--no-index", "--", "../x", "a.txt"},
                 {"diff", "--no-index", "/etc/passwd", "a.txt"},
                 {"stash", "drop"},
                 {"stash", "clear"},
                 {"worktree", "add", "x"},
                 {"switch", "-f", "main"},
                 {"remote", "add", "x", "y"},
                 {"config", "user.name", "x"},
                 {"submodule", "update"}})
            CHECK(!gitArgsRefusal(no).empty());
        // Fetch, pull and push say they do not work over SSH yet.
        for (const char* op : {"fetch", "pull", "push"}) {
            const std::string why = gitArgsRefusal({op, "origin"});
            CHECK(why.find("do not work in a window over SSH yet") != std::string::npos);
        }
    }

    GROUP("hostgit:dir");
    {
        using Shared::gitDirAllowed;
        CHECK(gitDirAllowed("/r/proj", "/r/proj", ""));
        CHECK(gitDirAllowed("/r/proj", "/r/proj/src", ""));
        CHECK(!gitDirAllowed("/r/proj", "/r/project", ""));
        CHECK(!gitDirAllowed("/r/proj", "/r", ""));
        CHECK(gitDirAllowed("/r/proj", "/r", "/r"));            // the top level, the folder below it
        CHECK(!gitDirAllowed("/r/proj", "/r/other", "/r"));     // but nothing else of the repository
        CHECK(!gitDirAllowed("/r/proj", "/", "/r"));
        CHECK(!gitDirAllowed("", "/r", ""));
        CHECK(gitDirAllowed("/", "/x", ""));
    }

    GROUP("hostgit:words");
    {
        GitUi::Row changed;
        changed.path = "src/a.txt";
        changed.letter = 'M';
        GitUi::DiscardPlan p = GitUi::discardPlanOverSsh(changed);
        CHECK(p.refusal.empty() && p.keepFirst && !p.trashFirst);
        CHECK(p.args == GitUi::discardPlan(changed).args);
        CHECK(p.detail.find("no Trash") != std::string::npos && p.detail.find("git object") != std::string::npos);
        CHECK(p.detail.find("Undo Discard") == std::string::npos);
        CHECK(p.doneText == "Discarded the changes to a.txt.");
        GitUi::Row fresh = changed;
        fresh.untracked = true;
        p = GitUi::discardPlanOverSsh(fresh);
        CHECK(p.args.empty() && p.refusal.find("not tracked") != std::string::npos &&
              p.refusal.find("file tree") != std::string::npos);
        GitUi::Row deleted = changed;
        deleted.letter = 'D';
        p = GitUi::discardPlanOverSsh(deleted);
        CHECK(p.refusal.empty() && !p.keepFirst && !p.trashFirst && p.detail.find("Undo") == std::string::npos);
        GitUi::Row staged = changed;
        staged.staged = true;
        CHECK(GitUi::discardPlanOverSsh(staged).refusal == GitUi::discardPlan(staged).refusal);
        // Several: one question; refused whole for a new file among them.
        GitUi::Row other = changed;
        other.path = "b.txt";
        GitUi::DiscardBatch b = GitUi::discardBatchPlanOverSsh({changed, other});
        CHECK(b.refusal.empty() && b.plans.size() == 2 && b.plans[0].keepFirst && b.plans[1].keepFirst);
        CHECK(b.question == "Discard the changes to 2 files?" && b.detail.find("no Trash") != std::string::npos);
        b = GitUi::discardBatchPlanOverSsh({changed, fresh});
        CHECK(b.plans.empty() && b.refusal.find("Nothing was discarded: ") == 0);
        CHECK(GitUi::discardBatchPlanOverSsh({changed}).question == GitUi::discardPlanOverSsh(changed).question);
        // A hunk's Discard.
        const std::string diff = "diff --git a/src/a.txt b/src/a.txt\n--- a/src/a.txt\n+++ b/src/a.txt\n"
                                 "@@ -1,2 +1,2 @@\n one\n-two\n+TWO\n";
        GitUi::HunkPlan h = GitUi::hunkPlan(diff, GitUi::DiffSide::Changes, GitUi::HunkOp::Discard, 5, 5, false);
        CHECK(h.refusal.empty());
        GitUi::HunkPlan hs = GitUi::hunkPlanOverSsh(h);
        CHECK(hs.args == h.args && hs.patch == h.patch && hs.question == h.question);
        CHECK(hs.detail.find("no Trash") != std::string::npos && hs.doneText == "Discarded a hunk of a.txt.");
        GitUi::HunkPlan st = GitUi::hunkPlan(diff, GitUi::DiffSide::Changes, GitUi::HunkOp::Stage, 5, 5, false);
        CHECK(GitUi::hunkPlanOverSsh(st).doneText == st.doneText);
        // The kept copies, named.
        CHECK(GitUi::keptCopiesText("Discarded the changes to a.txt.", {{"src/a.txt", "0123456789abcdef0123\n"}}) ==
              "Discarded the changes to a.txt. The changed copy is kept there as git object "
              "0123456789ab: git cat-file -p shows it.");
        CHECK(GitUi::keptCopiesText("Done.", {{"a.txt", "aaaaaaaaaaaaaaaa"}, {"b/b.txt", "bbbbbbbbbbbbbbbb"}}) ==
              "Done. The changed copies are kept there as git objects aaaaaaaaaaaa (a.txt) and "
              "bbbbbbbbbbbb (b.txt): git cat-file -p shows each.");
        CHECK(GitUi::keptCopiesText("Done.", {}) == "Done.");
        CHECK(Shared::gitTooOldText("pi:~/p").find("newer mutible on pi:~/p") != std::string::npos);
    }

    if (std::system("git --version >/dev/null 2>&1") != 0) {
        std::fprintf(stderr, "  hostgit: no git here, the runs skipped\n");
        return;
    }

    GROUP("hostgit:run");
    {
        const std::string repo = realScratch(), elsewhere = realScratch();
        std::filesystem::create_directories(repo + "/sub");
        fsWrite(repo + "/a.txt", "one\ntwo\n");
        fsWrite(repo + "/sub/b.txt", "bee\n");
        CHECK(gitSh(repo, "git init -q . && git config user.email a@b && git config user.name n && "
                          "git config commit.gpgsign false && git add -A && git commit -q -m first"));
        fsWrite(repo + "/a.txt", "one\nTWO\n");
        auto run = [](const std::string& folder, const std::string& dir, const std::vector<std::string>& args,
                      const std::string& input = std::string(), const std::atomic<bool>* cancel = nullptr) {
            return Shared::runGitRequest(folder, Shared::gitRequest(dir, args), input, cancel);
        };
        Shared::GitAnswer a = run(repo, repo, {"status", "--porcelain=v1"});
        CHECK(a.json["ok"].asBool() && a.json["status"].asInt() == 0 && a.out == " M a.txt\n");
        // A relative dir, and "" for the folder.
        a = run(repo, "sub", {"rev-parse", "--show-prefix"});
        CHECK(a.json["ok"].asBool() && a.out == "sub/\n");
        a = run(repo, "", {"rev-parse", "--show-toplevel"});
        CHECK(a.out == repo + "\n");
        // Standard input, and standard output as bytes.
        a = run(repo, repo, {"hash-object", "--stdin"}, "hello\n");
        CHECK(a.json["ok"].asBool() && a.out == "ce013625030ba8dba906f756967f9e9ca394464a\n");
        // A failure's status and words.
        a = run(repo, repo, {"rev-parse", "--verify", "--quiet", "nope"});
        CHECK(!a.json["ok"].asBool() && a.json["status"].asInt() == 1 && a.json["error"].isNull());
        a = run(repo, repo, {"show", "nope"});
        CHECK(!a.json["ok"].asBool() && a.json["err"].asString().find("nope") != std::string::npos);
        // Refused: an argument, a folder outside, a link out of it.
        a = run(repo, repo, {"clean", "-fd"});
        CHECK(a.json["reason"].asString() == "args" && a.out.empty());
        a = run(repo, repo, {"push"});
        CHECK(a.json["reason"].asString() == "remote");
        a = run(repo, elsewhere, {"status"});
        CHECK(a.json["reason"].asString() == "outside");
        CHECK(::symlink(elsewhere.c_str(), (repo + "/out").c_str()) == 0);
        a = run(repo, repo + "/out", {"status"});
        CHECK(a.json["reason"].asString() == "outside");
        a = run(repo, repo + "/missing", {"status"});
        CHECK(a.json["reason"].asString() == "missing");
        Json bad = Json::object();
        bad.set("dir", Json(repo));
        bad.set("args", Json("status"));
        CHECK(Shared::runGitRequest(repo, bad, "").json["reason"].asString() == "args");
        // A session on a folder below the top level runs at the top level,
        // and nowhere else of the repository.
        a = run(repo + "/sub", repo, {"status", "--porcelain=v1"});
        CHECK(a.json["ok"].asBool() && a.out.rfind(" M a.txt\n", 0) == 0);
        std::filesystem::create_directories(repo + "/third");
        a = run(repo + "/sub", repo + "/third", {"status"});
        CHECK(a.json["reason"].asString() == "outside");
        // Stopped when the renderer goes.
        std::atomic<bool> stop{true};
        a = run(repo, repo, {"status"}, "", &stop);
        CHECK(!a.json["ok"].asBool() && a.json["timedOut"].asBool());
        // The reply as a renderer reads it.
        Shared::GitReply r = Shared::gitReplyFrom(run(repo, repo, {"status", "--porcelain=v1"}).json, std::string("x"));
        CHECK(r.ok && r.status == 0 && r.out == "x");
        r = Shared::gitReplyFrom(Shared::gitRefused("no.", "args").json, std::string());
        CHECK(!r.ok && r.status == -1 && r.err == "no.");
    }

    GROUP("hostgit:wire");
    {
        const std::string repo = realScratch();
        fsWrite(repo + "/a.txt", "one\ntwo\n");
        CHECK(gitSh(repo, "git init -q . && git config user.email a@b && git config user.name n && "
                          "git config commit.gpgsign false && git add -A && git commit -q -m first"));
        fsWrite(repo + "/a.txt", "one\nTWO\n");
        Net net(true, true);   // through the encoding: the bytes base64 and back
        net.host.setFolder(repo);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        app.client->dispatch();
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(app.client->hostRunsGit());
        Shared::GitReply r = gitAsk(net, app, repo, GitUi::statusArgs());
        CHECK(r.ok);
        const Git::Status st = Git::parseStatus(r.out);
        CHECK(st.entries.size() == 1 && st.entries.size() && st.entries[0].path == "a.txt");
        // Binary standard input and output survive the wire.
        std::string blob("\0\x01\xff\n", 4);
        r = gitAsk(net, app, repo, {"hash-object", "-w", "--stdin"}, blob);
        CHECK(r.ok && r.out.size() == 41);
        const std::string oid = r.out.substr(0, 40);
        r = gitAsk(net, app, repo, {"cat-file", "blob", oid});
        CHECK(r.ok && r.out == blob);
        // Stage, then commit, as the panel does; git there says so.
        r = gitAsk(net, app, repo, GitUi::stageArgs(GitUi::Row{}, false));
        CHECK(!r.ok);   // no path: git refuses, and says why
        GitUi::Row row;
        row.path = "a.txt";
        row.letter = 'M';
        r = gitAsk(net, app, repo, GitUi::stageArgs(row, false));
        CHECK(r.ok);
        r = gitAsk(net, app, repo, {"diff", "--cached", "--name-only"});
        CHECK(r.out == "a.txt\n");
        r = gitAsk(net, app, repo, GitUi::commitArgs("second"));
        CHECK(r.ok && GitUi::commitDone(r.out).words.find("second") != std::string::npos);
        CHECK(Shared::Web::runGit(repo, {"log", "-1", "--format=%s"}, "").out == "second\n");
        // A refusal comes as status -1 with its sentence.
        Json raw;
        r = gitAsk(net, app, repo, {"clean", "-fd"}, "", &raw);
        CHECK(!r.ok && r.status == -1 && !r.err.empty() && raw["reason"].asString() == "args");
        // Never from a page.
        r = gitAsk(net, guest, repo, GitUi::statusArgs(), "", &raw);
        CHECK(!r.ok && raw["reason"].asString() == "readonly");
        r = gitAsk(net, reader, repo, GitUi::statusArgs(), "", &raw);
        CHECK(!r.ok && raw["reason"].asString() == "readonly");
        // An older host: no flag, nothing sent.
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.flag = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128;
        app.client->deliver(std::move(w));
        app.client->dispatch();
        CHECK(!app.client->hostRunsGit() &&
              app.client->requestGit(0, Shared::gitRequest(repo, GitUi::statusArgs()), "") == 0);
    }
}
