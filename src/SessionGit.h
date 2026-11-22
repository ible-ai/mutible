// SessionGit.h — git for a window on another machine (stage 9's first
// slice, docs/notes/session-host.md, "Git over SSH"). A window over SSH
// (`mutible ssh://host/path`) shows Source Control, the editor's change
// bars, the tree's git colors and the status bar's branch as a window on
// this Mac does, but its folder is on the other machine: the host there
// runs git for it, with that machine's own git, and the renderer parses
// what git printed with the same core code it uses for its own git
// (GitStatus.h, GitModel.h, QuickDiff.h).
//
// The seam is one request, where the apps already run a git process: an
// argument vector, a folder to run it in, and bytes for its standard input.
//
//   FsRead {req, window, json: {"op": "git", "dir", "args": [...],
//           "timeout"}, bytes: stdin}
//     -> FsData {req, json: {"op": "git", "ok", "status", "err",
//                "timedOut"}, bytes: stdout}
//      or FsData {req, json: {"op": "git", "error", "reason"}} (not run)
//
// A host offers it in its Welcome (flag 256, kHostRunsGit), so a renderer
// never sends it to a host from before it (which would answer "no read":
// the renderer says the host is too old instead of showing wrong data).
// The host runs it on a worker of its own (SessionSearch.h's ReadWorkers),
// never on its thread, queued behind the renderer's edits and saves (a
// commit after a save sees the saved file), with runGit (SessionWebGit.h):
// no shell, a process group of its own, no prompt for a password or an
// editor, GIT_OPTIONAL_LOCKS=0, stopped at its time limit (the request's,
// at most kMaxGitSeconds) or when the renderer goes.
//
// Held to what the apps run (gitArgsRefusal): the subcommands the Mac's and
// GTK's Source Control, change bars, tree colors and review use, each with
// the options that would write outside the repository, run another program
// or lose work refused; no global option but --literal-pathspecs and
// `-c` with the two keys the apps set; ls-remote, clone and submodule not
// at all. Fetch, pull and push (since stage 9's third slice) as the apps'
// syncPlan runs them: a configured remote's name (never a URL or a path),
// never forced, nothing deleted there; run with that machine's own
// credentials (its ssh agent and keys, its credential helper) and never a
// prompt (remoteGitSettings: a session of its own, no askpass, ssh in batch
// mode unless the user's GIT_SSH_COMMAND, GIT_SSH or core.sshCommand says
// otherwise), so a missing credential fails at once and is said plainly
// (gitCredentialsText). Run only in the session's folder, a folder inside it, or the top
// level of the repository the folder is in (gitDirAllowed: links resolved,
// so a link out of the folder leads nowhere). Refused from a renderer that
// only reads and from a guest (a browser's page has its own reads,
// SessionWebGit.h). Not a wall against the renderer, which may already run
// a shell there: a window over SSH is its user's own; it keeps an app's
// mistake from reaching past the folder.
// Pure C++17 and POSIX (realpath, and git through runGit).
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "Json.h"

namespace Shared {

constexpr int kHostRunsGit = 256;            // the Welcome's flag
constexpr double kMaxGitSeconds = 600;       // Git::timeoutFor's longest (commit)
constexpr std::size_t kMaxGitInput = std::size_t(64) << 20;   // a patch, a blob
constexpr std::size_t kMaxGitErr = std::size_t(64) << 10;     // stderr kept
constexpr std::size_t kMaxGitJobs = 16;      // a host's workers, git and reads together

// Why the host's git will not run `args` for a window, "" when it will.
std::string gitArgsRefusal(const std::vector<std::string>& args);

// The request a renderer sends: `dir` absolute on the host's machine (as
// git and the host name it), `timeout` in seconds (Git::timeoutFor's for
// the command when 0 or less).
Json gitRequest(const std::string& dir, const std::vector<std::string>& args, double timeout = 0);

// Whether git may run in `dir` for a session on `folder`: both resolved
// (no links), `top` the top level of the repository `folder` is in ("" when
// none). `dir` the folder, inside it, or `top` itself.
bool gitDirAllowed(const std::string& folder, const std::string& dir, const std::string& top);

// What the host answers: the json, and git's standard output (FsData's
// bytes). Runs git (runGit) in `dir` resolved on this machine, after
// gitArgsRefusal and gitDirAllowed, the repository's top level asked of git
// in the folder only when `dir` is outside it. `cancel` stops it (the
// renderer went). Fetch, pull and push start from `hostEnv` (the host's
// own environment; null: this process's) with remoteGitSettings put in.
struct GitAnswer {
    Json json;
    std::string out;
};
GitAnswer runGitRequest(const std::string& folder, const Json& request, const std::string& input,
                        const std::atomic<bool>* cancel = nullptr,
                        const std::vector<std::string>* hostEnv = nullptr);

// What fetch, pull and push run with on the host, given its environment
// `env` and the repository's core.sshCommand ("" when unset): no terminal
// prompt, no askpass (git's, ssh's or Git Credential Manager's windows),
// and GIT_SSH_COMMAND="ssh -o BatchMode=yes" unless `env` has
// GIT_SSH_COMMAND or GIT_SSH or the repository sets core.sshCommand (the
// user's own way to run ssh is left as it is). "NAME=value" each.
std::vector<std::string> remoteGitSettings(const std::vector<std::string>& env, const std::string& sshCommand);
// The refusal's answer: {"op": "git", "error", "reason"}.
GitAnswer gitRefused(const std::string& why, const std::string& reason);

// The answer read back on the renderer's side: what the Mac's MCGitResult
// and GTK's git result hold. A refusal is status -1, not ok, its sentence
// as `err`.
struct GitReply {
    bool ok = false;
    int status = -1;
    bool timedOut = false;
    std::string out, err;
};
GitReply gitReplyFrom(const Json& answer, std::string&& out);

// What a window over SSH says when its host cannot run git for it: the
// host is too old (no flag 256) or the connection is lost. `host` is the
// window's "host:path" or machine name.
std::string gitTooOldText(const std::string& host);
std::string gitLostText(const std::string& host);
// A fetch, pull or push over SSH that wanted a credential the other
// machine has not set up for `remote` (GitUi::needsCredentials): what to
// set up there, and git's last line (stderr's, else stdout's).
std::string gitCredentialsText(const std::string& host, const std::string& remote, const std::string& out,
                               const std::string& err);

}  // namespace Shared
