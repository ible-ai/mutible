// SessionGit.cpp — see SessionGit.h.
#include "SessionGit.h"

#include <climits>
#include <cstdlib>
#include <set>

#include "GitModel.h"
#include "GitStatus.h"
#include "SessionWebGit.h"

namespace Shared {

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool under(const std::string& path, const std::string& dir) {
    if (path == dir) return true;
    if (dir == "/") return !path.empty() && path[0] == '/';
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

std::string resolved(const std::string& path) {
    char buf[PATH_MAX];
    return !path.empty() && ::realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// A path an operand names after "--" for diff --no-index: /dev/null, or
// relative and without "..", so the comparison stays in the folder.
bool insideOperand(const std::string& a) {
    if (a == "/dev/null") return true;
    if (a.empty() || a[0] == '/') return false;
    std::size_t at = 0;
    while (at <= a.size()) {
        std::size_t slash = a.find('/', at);
        if (slash == std::string::npos) slash = a.size();
        if (a.compare(at, slash - at, "..") == 0 && slash - at == 2) return false;
        at = slash + 1;
    }
    return true;
}

}  // namespace

std::string gitArgsRefusal(const std::vector<std::string>& args) {
    std::size_t i = 0;
    // The global options the apps give: --literal-pathspecs, and -c with
    // the two settings they set. Any other (-C, --git-dir, --work-tree,
    // --exec-path, -c core.sshCommand=...) would point git elsewhere or run
    // another program.
    for (; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--literal-pathspecs" || a == "--no-pager" || a == "--no-optional-locks") continue;
        if (a == "-c") {
            if (i + 1 >= args.size()) return "git's -c needs a setting.";
            const std::string& kv = args[++i];
            const std::string key = lower(kv.substr(0, kv.find('=')));
            if (key != "core.quotepath" && key != "log.showsignature")
                return "The other machine's git takes no setting " + kv.substr(0, kv.find('=')) +
                       " from a window.";
            continue;
        }
        if (!a.empty() && a[0] == '-') return "The other machine's git takes no option " + a + " before its command.";
        break;
    }
    if (i >= args.size()) return "No git command was given.";
    const std::string cmd = args[i];
    if (cmd == "fetch" || cmd == "pull" || cmd == "push" || cmd == "ls-remote" || cmd == "clone" ||
        cmd == "submodule")
        return "Fetch, pull and push do not work in a window over SSH yet: run them in its terminal, "
               "which runs on that machine.";
    static const std::set<std::string> allowed = {
        // reads
        "status", "diff", "diff-tree", "log", "show", "rev-parse", "rev-list", "for-each-ref", "ls-files",
        "ls-tree", "cat-file", "merge-base", "symbolic-ref", "check-ignore", "remote", "worktree", "blame",
        // the index and the work tree, as the panel and the change bars write them
        "add", "restore", "rm", "commit", "reset", "update-ref", "update-index", "hash-object", "stash",
        "apply", "switch"};
    if (!allowed.count(cmd)) return "The other machine does not run git " + cmd + " for a window.";

    // Options that write a file outside the repository, run a program, or
    // let a patch reach past the work tree.
    bool afterDashes = false, noIndex = false;
    for (std::size_t j = i + 1; j < args.size(); j++) {
        const std::string& a = args[j];
        if (afterDashes) {
            if (noIndex && !insideOperand(a)) return "git diff --no-index compares files in the folder only.";
            continue;
        }
        if (a == "--") {
            afterDashes = true;
            continue;
        }
        if (startsWith(a, "--output") || startsWith(a, "--exec") || startsWith(a, "--upload-pack") ||
            startsWith(a, "--receive-pack") || a == "--unsafe-paths" || a == "--ext-diff" ||
            startsWith(a, "--open-files-in-pager") || startsWith(a, "--run-hook"))
            return "The other machine's git takes no " + a.substr(0, a.find('=')) + " from a window.";
        if (a == "--no-index") noIndex = true;
    }
    if (noIndex && !afterDashes) return "git diff --no-index compares files in the folder only.";
    auto has = [&](std::initializer_list<const char*> opts) {
        for (std::size_t j = i + 1; j < args.size() && args[j] != "--"; j++)
            for (const char* o : opts)
                if (args[j] == o) return true;
        return false;
    };
    const std::string sub = i + 1 < args.size() ? args[i + 1] : std::string();
    if (cmd == "reset" && has({"--hard", "--merge", "--keep"}))
        return "The other machine's git does not reset the work tree for a window.";
    if (cmd == "rm" && !has({"--cached"})) return "The other machine's git removes files from the index only (rm --cached).";
    if (cmd == "switch" && has({"--discard-changes", "-f", "--force"}))
        return "The other machine's git does not switch over changes for a window.";
    if (cmd == "stash" && !sub.empty() && sub != "push" && sub != "pop" && sub != "list" && sub != "show")
        return "The other machine's git does not run git stash " + sub + " for a window.";
    if (cmd == "worktree" && sub != "list") return "The other machine's git only lists worktrees for a window.";
    if (cmd == "remote" && !sub.empty() && sub != "-v" && sub != "show" && sub != "get-url")
        return "The other machine's git only names remotes for a window.";
    return "";
}

Json gitRequest(const std::string& dir, const std::vector<std::string>& args, double timeout) {
    Json j = Json::object();
    j.set("op", Json("git"));
    j.set("dir", Json(dir));
    Json a = Json::array();
    for (const std::string& s : args) a.push(Json(s));
    j.set("args", std::move(a));
    if (timeout > 0) j.set("timeout", Json(timeout));
    return j;
}

bool gitDirAllowed(const std::string& folder, const std::string& dir, const std::string& top) {
    if (folder.empty() || dir.empty()) return false;
    return under(dir, folder) || (!top.empty() && dir == top);
}

GitAnswer gitRefused(const std::string& why, const std::string& reason) {
    GitAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("git"));
    a.json.set("error", Json(why));
    a.json.set("reason", Json(reason));
    return a;
}

GitAnswer runGitRequest(const std::string& folder, const Json& request, const std::string& input,
                        const std::atomic<bool>* cancel) {
    const Json& list = request["args"];
    if (!list.isArray() || list.size() == 0 || list.size() > 4096)
        return gitRefused("A git request names its arguments.", "args");
    std::vector<std::string> args;
    for (const Json& a : list.elements()) {
        if (!a.isString()) return gitRefused("A git request's arguments are words.", "args");
        const std::string s = a.asString();
        if (s.find('\0') != std::string::npos) return gitRefused("A git request's arguments are words.", "args");
        args.push_back(s);
    }
    const std::string refusal = gitArgsRefusal(args);
    if (!refusal.empty()) {
        const std::string cmd = Git::subcommand(args);
        return gitRefused(refusal, Git::talksToRemote(args) || cmd == "ls-remote" || cmd == "clone" ? "remote" : "args");
    }
    if (input.size() > kMaxGitInput) return gitRefused("That is more input than git takes from a window.", "args");

    const std::string root = resolved(folder);
    if (root.empty()) return gitRefused("The session's folder is not there.", "missing");
    std::string asked = request["dir"].asString();
    if (asked.empty()) asked = root;
    else if (asked[0] != '/') asked = root + "/" + asked;
    const std::string dir = resolved(asked);
    if (dir.empty()) return gitRefused("There is no folder " + asked + " on this machine.", "missing");
    if (!gitDirAllowed(root, dir, "")) {
        // The repository's top level, when the folder is below it: what
        // the apps run status and the rest in.
        const ChangeReview::GitOut t = Web::runGit(root, {"rev-parse", "--show-toplevel"}, "", 30000, cancel);
        const std::string top = t.ok ? resolved(trimmed(t.out)) : std::string();
        if (!gitDirAllowed(root, dir, top))
            return gitRefused("git runs in the session's folder or its repository's top level, not in " + asked + ".",
                              "outside");
    }
    double seconds = request["timeout"].asNumber(0);
    if (!(seconds > 0)) seconds = Git::timeoutFor(args);
    if (seconds > kMaxGitSeconds) seconds = kMaxGitSeconds;
    if (seconds < 1) seconds = 1;
    ChangeReview::GitOut o = Web::runGit(dir, args, input, (int)(seconds * 1000), cancel);
    GitAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("git"));
    a.json.set("ok", Json(o.ok));
    a.json.set("status", Json((std::int64_t)o.status));
    std::string err = o.err.size() > kMaxGitErr ? o.err.substr(o.err.size() - kMaxGitErr) : o.err;
    a.json.set("err", Json(GitUi::validUtf8(err)));
    if (o.timedOut) a.json.set("timedOut", Json(true));
    a.out = std::move(o.out);
    return a;
}

GitReply gitReplyFrom(const Json& answer, std::string&& out) {
    GitReply r;
    if (!answer["error"].isNull() || answer["op"].asString() != "git") {
        r.err = answer["error"].isNull() ? std::string("The other machine did not run git.") : answer["error"].asString();
        return r;
    }
    r.ok = answer["ok"].asBool();
    r.status = (int)answer["status"].asNumber(-1);
    r.timedOut = answer["timedOut"].asBool();
    r.err = answer["err"].asString();
    r.out = std::move(out);
    return r;
}

std::string gitTooOldText(const std::string& host) {
    return "Source Control needs a newer mutible on " + host +
           ": the one there cannot run git for this window. Upgrade it there, then Reconnect.";
}

std::string gitLostText(const std::string& host) {
    return "The connection to " + host + " is lost: Reconnect, then try again.";
}

std::string gitRemoteOpText(const std::string& host) {
    return "Fetch, pull and push do not work in a window over SSH yet: run them in the terminal, which runs on " +
           host + ".";
}

}  // namespace Shared
