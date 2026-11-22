// session_git_tests.inc — stage 9's first slice: git for a window over SSH
// (SessionGit.h, the host's FsRead "git", Welcome flag 256,
// Client::requestGit), and the Discard words over SSH (GitModel.h's
// discardPlanOverSsh). Included after session_fileops_tests.inc, whose
// helpers (realScratch, fsWrite, Net) it uses.

#include "GitModel.h"
#include "SessionGit.h"
#include "SessionWebGit.h"

extern char** environ;

namespace {

std::string fsRead(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

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
                 {"submodule", "update"},
                 {"ls-remote", "origin"},
                 {"clone", "x"},
                 // fetch, pull and push: a configured remote, never forced,
                 // nothing deleted, no program of the request's own
                 {"push", "--force"},
                 {"push", "-f", "origin", "main"},
                 {"push", "--force-with-lease=main"},
                 {"push", "--delete", "origin", "main"},
                 {"push", "--mirror"},
                 {"push", "origin", "+main"},
                 {"push", "origin", ":main"},
                 {"push", "--receive-pack=touch x", "origin"},
                 {"fetch", "--upload-pack=touch x", "origin"},
                 {"fetch", "/etc/repo"},
                 {"fetch", "../other"},
                 {"fetch", "ext::sh -c touch% x"},
                 {"fetch", "file:///tmp/x"},
                 {"pull", "https://example.com/x.git"},
                 {"pull", "--exec=x"},
                 {"fetch", "--recurse-submodules=yes", "origin"}})
            CHECK(!gitArgsRefusal(no).empty());
        // Fetch, pull and push as the panel's Sync plans them run there now.
        GitUi::Snapshot s;
        s.inRepository = true;
        s.branch = "main";
        s.upstream = "origin/main";
        s.hasAheadBehind = true;
        s.remotes = {"origin"};
        GitUi::Snapshot unpublished = s;
        unpublished.upstream.clear();
        unpublished.hasAheadBehind = false;
        for (const GitUi::SyncPlan& plan :
             {GitUi::syncPlan(GitUi::SyncOp::Fetch, s), GitUi::syncPlan(GitUi::SyncOp::Pull, s),
              GitUi::syncPlan(GitUi::SyncOp::Push, s), GitUi::syncPlan(GitUi::SyncOp::Sync, s),
              GitUi::syncPlan(GitUi::SyncOp::Push, unpublished)}) {
            CHECK(!plan.steps.empty() && plan.remote == "origin");
            for (const std::vector<std::string>& step : plan.steps) {
                const std::string why = gitArgsRefusal(step);
                CHECK(why.empty());
                if (!why.empty()) std::fprintf(stderr, "  refused: %s (%s)\n", Git::commandLine(step).c_str(), why.c_str());
            }
        }
        CHECK(gitArgsRefusal({"fetch", "upstream/fork"}).empty());
        CHECK(gitArgsRefusal({"pull", "--ff-only"}).empty());
        CHECK(gitArgsRefusal({"push", "-u", "origin", "topic:topic"}).empty());
        CHECK(gitArgsRefusal({"fetch", "/etc/repo"}).find("remotes") != std::string::npos);

        // What they run with: no prompt, and ssh in batch mode unless the
        // user's own way to run it says otherwise.
        auto has = [](const std::vector<std::string>& v, const std::string& kv) {
            return std::find(v.begin(), v.end(), kv) != v.end();
        };
        std::vector<std::string> set = Shared::remoteGitSettings({"PATH=/usr/bin", "HOME=/h"}, "");
        CHECK(has(set, "GIT_TERMINAL_PROMPT=0") && has(set, "GIT_ASKPASS=") && has(set, "SSH_ASKPASS_REQUIRE=never"));
        CHECK(has(set, "GIT_SSH_COMMAND=ssh -o BatchMode=yes"));
        set = Shared::remoteGitSettings({"GIT_SSH_COMMAND=ssh -i ~/.ssh/work"}, "");
        CHECK(has(set, "GIT_TERMINAL_PROMPT=0"));
        for (const std::string& kv : set) CHECK(kv.rfind("GIT_SSH_COMMAND=", 0) != 0);
        set = Shared::remoteGitSettings({"GIT_SSH=/usr/local/bin/myssh"}, "");
        for (const std::string& kv : set) CHECK(kv.rfind("GIT_SSH_COMMAND=", 0) != 0);
        set = Shared::remoteGitSettings({}, "ssh -i key");
        for (const std::string& kv : set) CHECK(kv.rfind("GIT_SSH_COMMAND=", 0) != 0);
        set = Shared::remoteGitSettings({"GIT_SSH_COMMAND="}, "");   // empty: unset
        CHECK(has(set, "GIT_SSH_COMMAND=ssh -o BatchMode=yes"));

        // A missing credential, said plainly with git's last line.
        CHECK(GitUi::needsCredentials("", "git@host: Permission denied (publickey).\nfatal: Could not read"));
        CHECK(GitUi::needsCredentials("", "fatal: could not read Username for 'https://x': terminal prompts disabled"));
        CHECK(!GitUi::needsCredentials("", "fatal: 'nope' does not appear to be a git repository"));
        const std::string said = Shared::gitCredentialsText(
            "pi", "origin", "", "git@x: Permission denied (publickey).\r\nfatal: Could not read from remote repository.\n\n");
        CHECK(said.find("origin needs credentials set up on pi") == 0);
        CHECK(said.find("credential helper") != std::string::npos && said.find("never asks") != std::string::npos);
        CHECK(said.substr(said.find('\n')) == "\ngit: git@x: Permission denied (publickey).");
        CHECK(GitUi::credentialsLine("", "remote: x\nfatal: could not read Password for 'https://h': terminal "
                                         "prompts disabled\r\n") ==
              "fatal: could not read Password for 'https://h': terminal prompts disabled");
        // With no line naming the credential, git's last line.
        const std::string other = Shared::gitCredentialsText("pi", "origin", "", "one\nfatal: the end.\n\n");
        CHECK(other.substr(other.find('\n')) == "\ngit: fatal: the end.");
        CHECK(Shared::gitCredentialsText("pi", "", "", "").find("this repository's remote needs") == 0);
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
        a = run(repo, repo, {"ls-remote"});
        CHECK(a.json["reason"].asString() == "remote");
        a = run(repo, repo, {"push", "--force"});
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

    GROUP("hostgit:sync");
    {
        // A bare repository on the same disk as the remote's origin: push,
        // fetch and pull through the host's request, with no network.
        const std::string top = realScratch();
        const std::string bare = top + "/origin.git", repo = top + "/work", other = top + "/other";
        const std::string ident = "git config user.email a@b && git config user.name n && git config commit.gpgsign false";
        CHECK(gitSh(top, "git init -q --bare -b main origin.git"));
        CHECK(gitSh(top, "git clone -q origin.git work 2>/dev/null; cd work && " + ident +
                             " && git symbolic-ref HEAD refs/heads/main && echo one > a.txt && git add -A && "
                             "git commit -q -m first"));
        auto run = [&](const std::vector<std::string>& args, const std::vector<std::string>* env = nullptr) {
            return Shared::runGitRequest(repo, Shared::gitRequest(repo, args), "", nullptr, env);
        };
        // Publish: push --set-upstream, as the panel's Publish Branch.
        Shared::GitAnswer a = run({"push", "--set-upstream", "origin", "main"});
        CHECK(a.json["ok"].asBool());
        if (!a.json["ok"].asBool()) std::fprintf(stderr, "  push: %s\n", a.json["err"].asString().c_str());
        CHECK(Shared::Web::runGit(bare, {"log", "-1", "--format=%s", "main"}, "").out == "first\n");
        // A second commit, pushed.
        CHECK(gitSh(repo, "echo two >> a.txt && git commit -qam second"));
        a = run({"push"});
        CHECK(a.json["ok"].asBool());
        CHECK(Shared::Web::runGit(bare, {"log", "-1", "--format=%s", "main"}, "").out == "second\n");
        // Someone else pushes; fetch brings it, pull merges it.
        CHECK(gitSh(top, "git clone -q origin.git other && cd other && " + ident +
                             " && echo three > b.txt && git add -A && git commit -q -m third && git push -q"));
        a = run({"fetch", "origin"});
        CHECK(a.json["ok"].asBool());
        CHECK(Shared::Web::runGit(repo, {"log", "-1", "--format=%s", "origin/main"}, "").out == "third\n");
        CHECK(Shared::Web::runGit(repo, {"log", "-1", "--format=%s"}, "").out == "second\n");
        a = run({"pull", "--ff-only"});
        CHECK(a.json["ok"].asBool());
        CHECK(Shared::Web::runGit(repo, {"log", "-1", "--format=%s"}, "").out == "third\n");

        // An origin over ssh that needs a key the machine cannot use: ssh
        // (a stand-in on the host's PATH) runs in batch mode, with no
        // terminal to ask on, and fails at once; the words say what to set up.
        const std::string bin = top + "/bin", seen = top + "/ssh-seen.txt";
        std::filesystem::create_directories(bin);
        fsWrite(bin + "/ssh", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + seen +
                                  "'\nif (exec 3</dev/tty) 2>/dev/null; then echo tty >> '" + seen +
                                  "'; else echo no-tty >> '" + seen +
                                  "'; fi\necho 'git@locked.invalid: Permission denied (publickey).' >&2\nexit 255\n");
        ::chmod((bin + "/ssh").c_str(), 0755);
        CHECK(gitSh(repo, "git remote add locked git@locked.invalid:x.git"));
        std::vector<std::string> env;
        for (char** e = environ; e && *e; e++) {
            const std::string kv = *e;
            if (kv.rfind("PATH=", 0) == 0) env.push_back("PATH=" + bin + ":" + kv.substr(5));
            else if (kv.rfind("GIT_SSH", 0) != 0) env.push_back(kv);
        }
        const double t0 = nowMs();
        a = run({"fetch", "locked"}, &env);
        CHECK(!a.json["ok"].asBool() && !a.json["timedOut"].asBool());
        CHECK(nowMs() - t0 < 10000);
        const std::string err = a.json["err"].asString();
        CHECK(GitUi::needsCredentials("", err));
        const std::string args = fsRead(seen);
        CHECK(args.find("-o\nBatchMode=yes\n") == 0);
        CHECK(args.find("no-tty") != std::string::npos);
        if (args.find("-o\nBatchMode=yes\n") != 0) std::fprintf(stderr, "  ssh was run with: %s\n", args.c_str());
        CHECK(Shared::gitCredentialsText("pi", "locked", "", err).find(
                  "\ngit: git@locked.invalid: Permission denied (publickey).") != std::string::npos);
        // The user's own GIT_SSH_COMMAND passes through as it is.
        std::filesystem::remove(seen);
        env.push_back("GIT_SSH_COMMAND=" + bin + "/ssh -o User=mine");
        a = run({"fetch", "locked"}, &env);
        CHECK(!a.json["ok"].asBool());
        CHECK(fsRead(seen).find("-o\nUser=mine\n") == 0);
        CHECK(fsRead(seen).find("BatchMode") == std::string::npos);
        // As does the repository's core.sshCommand.
        env.pop_back();
        std::filesystem::remove(seen);
        CHECK(gitSh(repo, "git config core.sshCommand '" + bin + "/ssh -o User=config'"));
        a = run({"fetch", "locked"}, &env);
        CHECK(!a.json["ok"].asBool());
        CHECK(fsRead(seen).find("-o\nUser=config\n") == 0);
        CHECK(fsRead(seen).find("BatchMode") == std::string::npos);
    }
}
