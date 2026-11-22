// SessionLsp.h — language servers for a window on another machine (stage
// 9's second slice, docs/notes/session-host.md, "Language servers over
// SSH"). A window over SSH (`mutible ssh://host/path`) edits files that
// are on the other machine, so their language server must run there: the
// host starts it for the window, with that machine's own programs, and
// relays its JSON-RPC both ways. The window's LspClient (LspClient.h) is
// the same as for a server on this Mac; only the transport differs (the
// Mac's MCLspServer writes LspInput instead of a pipe, and reads LspOutput).
//
//   renderer → host  LspStart {term: the server's id (made as a terminal's),
//                    window, json: ServerRequest}: the commands to try in
//                    order (the window's settings, lsp.<language>, else the
//                    defaults), each looked for on the host (planServer);
//                    LspInput {term, bytes}: bytes for its standard input;
//                    LspStop {term}: its input closed, then SIGTERM after
//                    kServerGraceMs and SIGKILL after kServerKillMs.
//   host → renderer  LspStarted {term, v: its pid, json: startedJson (the
//                    command line run, which of the commands it was, and
//                    what the project-code check said)}; LspOutput {term,
//                    bytes}: what it wrote to its standard output, as it
//                    came (the renderer frames it, Lsp::Reader); LspExited
//                    {term, flag: the wait status (-1 unknown), name: why
//                    it never started ("missing", "waits", "failed",
//                    "busy"), json: {"stderr": the end of what it wrote
//                    there, "tried", "file"}}; LspFiles {window, json:
//                    {"changes": [[path, type], ...]}}: files the host saw
//                    change on its disk (DiskWatch's folders, compared
//                    with what the folder held: FolderStamps), for the
//                    servers' workspace/didChangeWatchedFiles, to each
//                    renderer with a server running.
//
// A host offers it in its Welcome (flag 512, kHostRunsServers), so a
// renderer never sends LspStart to a host from before it (which would take
// the line for one that is not a message and drop the connection); a
// window over SSH says its servers need a newer mutible there instead
// (serversTooOldText). Only from an app's window: a reader and a guest are
// refused. A server is its renderer's: it ends when the renderer asks
// (LspStop), when the window the request named closes (WindowClose), and
// when the renderer's connection goes (a drop: the window starts it again
// once it is back, which reopens its documents there).
//
// Paths need no mapping: the window's documents carry the remote paths, so
// the URIs it sends are the remote's, and the server's answers name files
// the window opens through its host.
//
// Consent (server_consent): the folder's answer about running its own code
// is the window's, kept per remote host and folder (the request's
// `allowed`); the check of what the folder holds (Lsp::checkProjectCode:
// a cargo configuration, the folder's own TypeScript) is made by the host
// on its own disk, and its verdict comes back in LspStarted (or LspExited
// "waits" with the file that decided).
// Pure C++17 and POSIX (posix_spawn, pipes, waitpid, opendir, stat).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Json.h"
#include "LspClient.h"

namespace Shared {

constexpr int kHostRunsServers = 512;              // the Welcome's flag
constexpr std::size_t kServerErrTail = 4096;       // stderr kept, for why it died
constexpr std::size_t kMaxServersPerRenderer = 16; // one past is refused "busy"
constexpr int kServerGraceMs = 2000;               // LspStop: SIGTERM after
constexpr int kServerKillMs = 3000;                // and SIGKILL after
constexpr std::size_t kServerReadChunk = 64 << 10; // read a turn, one LspOutput

// LspStart's json.
struct ServerRequest {
    std::string key;                                // "python", "python.also"
    std::vector<std::vector<std::string>> commands; // tried in order, each split into words
    bool allowed = false;    // the folder's answer about its own code is "allow"
    std::string fileDir;     // the file on show's folder (cargo reads its config from there up)
};
Json serverRequestJson(const ServerRequest& r);
ServerRequest serverRequestFrom(const Json& j);

// The host's disk, as finding a program reads it (checks give their own).
struct ServerFs {
    std::function<bool(const std::string& path)> isDir;
    std::function<bool(const std::string& path)> isExecutable;   // a file it may run
    std::function<std::vector<std::string>(const std::string& dir)> list;   // names
};
ServerFs realServerFs();

// Where a server's program is looked for on the host: the host's PATH,
// then the places package managers put language servers (an ssh command's
// PATH is short: no ~/.profile), the newest Node of nvm's first; only the
// folders that are there, each once.
std::vector<std::string> serverSearchDirs(const std::string& path, const std::string& home, const ServerFs& fs);
// `word` (a name, or a path with a '/': ~ is the home's) found in `dirs`:
// its path, "" when none.
std::string findServerProgram(const std::string& word, const std::vector<std::string>& dirs,
                              const std::string& home, const ServerFs& fs);

// What the host decides before it starts a server.
struct ServerPlan {
    std::string error;             // "" when it starts; "missing", "waits", "empty"
    std::vector<std::string> argv; // argv[0] the program found
    int command = -1;              // which of the request's commands
    std::vector<std::string> tried;    // the programs looked for (none found)
    Lsp::ProjectCodeCheck code;    // what the folder's code allows (rust, typescript)
    std::vector<std::string> env;  // its environment: the host's, PATH the program's folder first
};
// The first of the request's commands whose program is there, its
// project-code check (on `pfs`, the host's disk, with the request's
// answer), and its environment (`env` with PATH the program's folder, then
// the search folders, so a script's `#!/usr/bin/env node` finds the node
// it was installed with).
ServerPlan planServer(const ServerRequest& r, const std::string& root, const std::string& home,
                      const std::vector<std::string>& env, const ServerFs& fs, const Lsp::ProjectFs& pfs);
Lsp::ProjectFs realProjectFs();
// LspStarted's json: {"argv", "command", "verdict" ("free", "restricted"),
// "file", "initializationOptions", "settings"}.
Json startedJson(const ServerPlan& p);
// Read back on the renderer's side.
struct ServerStarted {
    std::vector<std::string> argv;
    int command = -1;
    Lsp::ProjectCodeCheck code;
};
ServerStarted serverStartedFrom(const Json& j);

// A language server's process on the host: its standard input, output and
// error on pipes (never blocking), in a process group of its own, with
// signals at their defaults and none of the host's other descriptors.
class ServerProcess {
public:
    ServerProcess() = default;
    ~ServerProcess();   // closes the pipes, kills the group if it still runs, reaps
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;
    bool start(const std::vector<std::string>& argv, const std::vector<std::string>& env,
               const std::string& cwd, std::string* error);
    int pid() const { return pid_; }
    int outFd() const { return out_; }
    int errFd() const { return err_; }
    int inFd() const { return in_; }
    // Bytes for its input: written now as far as the pipe takes, the rest
    // when it is writable (flush). Nothing once the input is closed.
    void write(const std::string& bytes);
    void flush();
    bool wantsWrite() const { return in_ >= 0 && !pending_.empty(); }
    std::size_t pending() const { return pending_.size(); }
    // Its input closed once what waits is written (now if nothing does).
    void closeInput();
    bool inputClosed() const { return closing_ || in_ < 0; }
    // At most kServerReadChunk of its output; false at its end (the pipe
    // closed), true while more may come.
    bool readOut(std::string& out);
    // Its standard error, kept (the last kServerErrTail bytes).
    void readErr();
    const std::string& errTail() const { return errTail_; }
    bool outEnded() const { return out_ < 0; }
    // A signal to its process group.
    void signal(int sig);
    // Whether it has ended, its wait status in *status; reaps it once.
    bool reap(int* status);
    bool reaped() const { return reaped_; }

private:
    int pid_ = 0, in_ = -1, out_ = -1, err_ = -1;
    std::string pending_, errTail_;
    bool closing_ = false, reaped_ = false;
    int status_ = -1;
};

// Files that changed in a folder, for workspace/didChangeWatchedFiles: the
// host's watch names folders, so each folder's files are compared with
// what it held when last looked at. A folder never looked at before has
// its files changed since `since` (the first server's start) reported
// Changed, and none Deleted (nothing says what was there).
struct StampEntry {
    std::string name;
    bool dir = false;
    std::int64_t mtimeNs = 0;
    std::int64_t size = 0;
};
class FolderStamps {
public:
    using Lister = std::function<bool(const std::string& dir, std::vector<StampEntry>* out)>;
    static constexpr std::size_t kMaxFolders = 4096;   // then it starts again
    static constexpr std::size_t kMaxEntries = 20000;  // a folder past it is not followed
    void setSince(std::int64_t ns) { since_ = ns; }
    // `dir` (absolute) looked at again: what changed in it since, appended
    // to *out (its files only; a folder made or gone is a Created or Deleted
    // of the folder itself, as VS Code reports one).
    void look(const std::string& dir, const Lister& list, std::vector<Lsp::FileChange>* out);
    // `dir` remembered as it is now, nothing reported.
    void remember(const std::string& dir, const Lister& list);
    void clear() { seen_.clear(); }
    std::size_t folders() const { return seen_.size(); }

private:
    std::map<std::string, std::map<std::string, StampEntry>> seen_;
    std::int64_t since_ = 0;
};
// A folder's entries with lstat's times and sizes (dotfiles too).
bool listStamps(const std::string& dir, std::vector<StampEntry>* out);

// LspFiles's json, and back.
Json filesJson(const std::vector<Lsp::FileChange>& changes);
std::vector<Lsp::FileChange> filesFrom(const Json& j);

// What a window over SSH says of its servers: the host is too old for them
// (no flag 512), a server is not installed there (none of `tried` was
// found; `name` the server's, as the status bar names it), the connection
// is lost. `host` is the window's machine (or "host:path").
std::string serversTooOldText(const std::string& host);
std::string serverMissingText(const std::string& name, const std::string& host,
                              const std::vector<std::string>& tried);
std::string serversLostText(const std::string& host);
// A wait status as a sentence ("exited with status 1", "killed by signal 9").
std::string serverExitText(int status);

}  // namespace Shared
