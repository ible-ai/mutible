// SessionFormat.h — formatters for a window on another machine (stage 9's
// third slice, docs/notes/session-host.md, "Formatters over SSH"). A window
// over SSH (`mutible ssh://host/path`) formats files that are on the other
// machine, so Format Document and format on save run there: the host plans
// the formatter on its own disk (FormatRun::decide, the same decisions the
// Mac's and GTK's Format Document make, with that machine's PATH, the usual
// install folders, its .venv and node_modules), runs it with the
// document's text on its standard input in the file's folder (so the
// project's .prettierrc, pyproject.toml or .clang-format there are what it
// reads), and answers with its standard output, or with why it did not.
// A formatter a language server offers needs nothing new: the window's
// servers are its host's already (SessionLsp.h).
//
//   FsRead {req, window, json: {"op": "format", "path", "setting",
//           "consent", "first", "last", "projectOnly", "timeout"},
//           bytes: the text}
//     -> FsData {req, json: {"op": "format", "kind": "run" | "unavailable"
//                | "consent" | "failed", "ok", "message", "tool", "style",
//                "ranged", "serverMayFormat", "missing", "consentFolder",
//                "consentConfig"}, bytes: the formatted text when ok}
//      or FsData {req, json: {"op": "format", "error", "reason"}} (not run:
//                "args", "outside", "setting", "readonly", "busy")
//
// A host offers it in its Welcome (flag 1024, kHostFormats), so a renderer
// never sends it to a host from before it; the window says Format Document
// needs a newer mutible there instead (formatTooOldText). Run on a worker
// of the host's (as git is, SessionGit.h), never on its thread, queued
// behind the renderer's edits and saves. No shell: the program found and
// its arguments (Formatter::arguments), posix_spawn in a session of its
// own, stopped at the request's time limit (at most kMaxFormatSeconds) or
// when the renderer goes.
//
// Held to the formatter table (Formatter::knownPrograms): the window's
// format.<lang> command runs only when its program, or the program a
// package runner launches (npx prettier), is one of the table's
// (formatSettingRefusal); the file must be inside the session's folder
// (links resolved), and only an app's window may ask (a reader's and a
// guest's are refused). The folder's own programs and code-running
// configurations wait for the window's answer, kept per host and folder
// (`consent`), as on the Mac.
// Pure C++17 and POSIX (posix_spawn through SessionLsp.h's ServerProcess,
// poll, stat, realpath).
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "FormatRun.h"
#include "Json.h"

namespace Shared {

constexpr int kHostFormats = 1024;                              // the Welcome's flag
constexpr double kMaxFormatSeconds = 120;                       // a request's longest
constexpr std::size_t kMaxFormatInput = std::size_t(64) << 20;  // the text sent
constexpr std::size_t kMaxFormatOutput = std::size_t(256) << 20;
constexpr std::size_t kMaxFormatErr = std::size_t(64) << 10;    // stderr kept

// The request: `path` absolute on the host (as the window's tab names it),
// `setting` format.<lang> for its language ("" unset), `consent` the
// window's answer about the folder's own programs on that host ("allow",
// "deny", ""), firstLine..lastLine (1-based, 0 and 0 for the whole file),
// `projectOnly` format.on-save = project, `timeout` in seconds.
struct FormatRequest {
    std::string path, setting, consent;
    int firstLine = 0, lastLine = 0;
    bool projectOnly = false;
    double timeout = 10;
};
Json formatRequestJson(const FormatRequest& r);
FormatRequest formatRequestFrom(const Json& j);

// Why the host will not run format.<lang> = `setting` for a window: its
// program (or the one a package runner launches) is not one of the
// formatter table's. "" when it will, and for "" and "off".
std::string formatSettingRefusal(const std::string& lang, const std::string& setting);

// What the decisions look at on this machine: its disk, the program search
// over `env`'s PATH and the usual folders (SessionLsp.h's serverSearchDirs:
// an ssh command's PATH lacks ~/.profile's), its $VIRTUAL_ENV and home.
FormatRun::Env hostFormatEnv(const std::vector<std::string>& env);

// A program run to its end: `input` on its standard input, `cwd` its
// working directory, `env` its environment; stopped (SIGTERM, then SIGKILL
// a second later) past `timeoutMs` or when `cancel` goes up.
struct ProgramRun {
    int status = -1;          // its exit status, 128 + a signal's number
    bool timedOut = false;
    bool cancelled = false;
    std::string out, err;     // err: its first kMaxFormatErr bytes
    std::string launchError;  // "could not be started (...)", "left its output open after exiting"
};
ProgramRun runProgram(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                      const std::string& cwd, const std::string& input, int timeoutMs,
                      const std::atomic<bool>* cancel = nullptr);

// What the host answers: the json, and the formatted text (FsData's
// bytes). `env` is the host's environment (its login shell's, else the
// process's).
struct FormatAnswer {
    Json json;
    std::string out;
};
FormatAnswer runFormatRequest(const std::string& folder, const Json& request, const std::string& input,
                              const std::vector<std::string>& env, const std::atomic<bool>* cancel = nullptr);
FormatAnswer formatRefused(const std::string& why, const std::string& reason);

// The answer read back on the renderer's side.
struct FormatReply {
    // "run" (ran: ok, or `message` says why the text stays), "unavailable"
    // (no formatter for the file, or off; `serverMayFormat`), "consent"
    // (ask, then format again), "failed" (`message`; `missing` when none
    // is installed there), "error" (not run: refused; `reason`).
    std::string kind = "error";
    bool ok = false;
    std::string text, message, tool, style, reason;
    bool ranged = false, serverMayFormat = false, missing = false;
    std::string consentFolder, consentConfig;
};
FormatReply formatReplyFrom(const Json& answer, std::string&& out);

// What a window over SSH says: its host is too old to format (no flag
// 1024), the connection is lost, or no formatter is installed there
// (`message` the plan's, naming what was looked for). `host` is the
// window's machine or "host:path".
std::string formatTooOldText(const std::string& host);
std::string formatLostText(const std::string& host);
std::string formatMissingText(const std::string& message, const std::string& host);

}  // namespace Shared
