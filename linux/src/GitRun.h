// GitRun.h — running git for the Source Control panel (GitPanel.h), the GTK
// counterpart of the Mac's MCGitRunSync in src/GitPanel.mm. Only ever called
// on the panel's git thread: each call blocks until git is done or its time
// limit (Git::timeoutFor) is over.
//
// An argument vector, never a shell. stdin is /dev/null, or `input` through
// a pipe (git apply's patch). stdout and stderr are read together, so a
// chatty stderr cannot fill its pipe and stall git. git leads a process group
// of its own, so a time limit stops the hooks, filters and credential helpers
// it started along with it; a command that talks to a remote
// (Git::talksToRemote: fetch, pull, push) gets a session of its own too, as
// the Mac's POSIX_SPAWN_SETSID gives it, so it has no controlling terminal:
// ssh cannot ask for a passphrase or a host key on the terminal mutible was
// started from, and fails at once instead, as git does for a password with
// GIT_TERMINAL_PROMPT=0 (GitUi::gitEnvironment).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "FolderSearch.h"
#include "GitModel.h"

struct GitResult {
    int status = -1;
    std::string out, err;
    bool timedOut = false;   // stopped at its time limit; err says so
    bool ok() const { return status == 0; }
};

// ---- a window over SSH (stage 9): its git runs on the other machine
// through the window's host (SessionGit.h, Client::requestGit). A git
// thread (the panel's, the change bars', the tree colors') runs inside a
// GitRunScope holding the window's runner; runGitWithin then hands the
// command to it instead of spawning git here. The runner is called on that
// thread and waits there for the answer, which crosses the main loop
// (hostGitRunner); never on the main thread.
using GitRunFn = std::function<GitResult(const std::string& dir, const std::vector<std::string>& args,
                                         double limit, const std::string* input)>;
// The runner of a window over SSH also reads a file of the host's work tree
// (the review page's changed files; ChangeReview::ImageReading::readWork):
// like the command, called on a git thread and waiting there.
struct GitRunner : GitRunFn {
    using GitRunFn::GitRunFn;
    std::function<FolderSearch::ReadStatus(const std::string& path, std::string* out)> readFile;
};
using GitRunnerPtr = std::shared_ptr<const GitRunner>;
class GitRunScope {
public:
    explicit GitRunScope(const GitRunnerPtr& runner);
    ~GitRunScope();
    GitRunScope(const GitRunScope&) = delete;
    GitRunScope& operator=(const GitRunScope&) = delete;

private:
    const GitRunner* previous_;
};
// Whether the calling thread's git runs on a host.
bool gitRunsOnHost();
// The calling thread's host's reader of a work tree file; empty when its git
// runs here.
std::function<FolderSearch::ReadStatus(const std::string& path, std::string* out)> hostWorkReader();

class RemoteWorkspace;
// The runner for a window over SSH: asks `ws` (held weakly) on the main loop
// and waits for the answer. Fetch, pull and push, a lost connection and a
// host too old to run git are answered at once in Shared::gitRemoteOpText's,
// gitLostText's and gitTooOldText's words, status -1.
GitRunnerPtr hostGitRunner(const std::shared_ptr<RemoteWorkspace>& ws);

// Where git is, found once on PATH. Empty when it is not installed.
const std::string& gitExecutable();

// git `args` in `dir`, with `input` on its stdin when given, within `limit`
// seconds; past it git's group gets SIGTERM, then SIGKILL 2 seconds later,
// and the result says "git did not answer within 30 seconds: git status ...".
GitResult runGitWithin(const std::string& dir, const std::vector<std::string>& args,
                       double limit, const std::string* input = nullptr);
// With the Mac's limit for the command.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args,
                 const std::string* input = nullptr);

// What to tell the user about a failed command (GitUi::failureText).
std::string gitFailure(const GitResult& r);

// What the panel shows about the folder at `root`: its top level, the
// status and the remotes (for Publish Branch, Fetch and autofetch). Read off
// the main thread.
GitUi::Snapshot gitSnapshotAt(const std::string& root);
