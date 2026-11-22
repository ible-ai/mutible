// LspProcess.h — one language server process (lspgtk::Server): its pipes,
// its Lsp::Client, its log, and stopping it; and finding a server's program
// on the disk. The Mac's counterpart is src/LspServer.{h,mm}. Only the
// language server session's files (Lsp.cpp and LspSession*.cpp) include
// this; the rest of the app sees Lsp.h.
//
// Everything runs on the main thread: output is read from a non-blocking
// pipe when the main loop says it is readable, and writes that do not fit in
// the pipe wait for it to drain, so a server that is slow to read never
// blocks typing. LspTerminateAllServers, LspSearchDirs, LspFindProgram,
// LspRunningServerCount and LspServerPids (Lsp.h) are this file's.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Json.h"
#include "LspClient.h"
#include "SessionLsp.h"

namespace lspgtk {

class RemoteHost;

// The places language servers get installed, after PATH: a launcher started
// from the dock may have a short PATH, and cargo, go and pip put servers in
// the home folder.
std::vector<std::string> searchDirs();
// A program by name on those folders (or a path, ~ allowed), "" when none.
std::string findProgram(const std::string& name);
// A command line from the settings (or the defaults) -> the program's full
// path plus its arguments, or nothing when the program is not installed.
// A bare name is looked for in `first` before the usual folders (a Python
// server beside the interpreter it is given).
std::vector<std::string> resolveCommand(const std::string& command,
                                        const std::vector<std::string>& first = {});

class Server : public std::enable_shared_from_this<Server> {
public:
    // Starts `argv` in `root`; null when it could not be started. `key` is
    // the settings key it runs for (cpp, python, ...).
    static std::shared_ptr<Server> start(const std::string& key,
                                         const std::vector<std::string>& argv,
                                         const std::string& root);
    // A server the window's host runs on the other machine (a window over
    // SSH, LspRemote.h): `argv` the first command's words, only to name it
    // until the host says what it started. Null when the host cannot be
    // asked. The client is initialized by the session once onStarted comes.
    static std::shared_ptr<Server> startRemote(const std::string& key, const std::vector<std::string>& argv,
                                               const std::shared_ptr<RemoteHost>& host,
                                               const Shared::ServerRequest& request);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    Lsp::Client& client() { return *client_; }
    const std::string& key() const { return key_; }
    const std::string& name() const { return name_; }
    bool running() const { return running_; }
    bool stopping() const { return stopping_; }
    int pid() const { return pid_; }
    std::function<void(Server*)> onExit;

    // ---- a remote server (startRemote); a pid there is the other machine's:
    // shown, never signalled, and it is not among LspTerminateAllServers'.
    bool remote() const { return isRemote_; }
    // The host started it (what it found: its command line, the project-code
    // verdict); the session initializes the client from this.
    std::function<void(Server*, const Shared::ServerStarted&)> onStarted;
    // Why the host never started it or lost it ("missing", "waits",
    // "failed", "busy", "lost"), "" when it ran and ended by itself, and the
    // host's json (the programs looked for, the file, its stderr's end).
    const std::string& refusal() const { return refusal_; }
    const Json& refusalInfo() const { return refusalInfo_; }
    // The end of what it wrote to standard error (the host's report).
    const std::string& errTail() const { return errTail_; }
    // From the host, through RemoteHost's relay.
    void remoteStarted(int pid, const Json& info);
    void remoteWrote(const std::string& bytes);
    void remoteExited(int status, const std::string& why, const Json& info);

    void stop();       // shutdown, exit, then signals if it lingers
    void stopNow();    // for quitting: exit at once, written before returning
    void signal(int sig) { if (running_ && proc_) g_subprocess_send_signal(proc_, sig); }

private:
    Server() = default;
    void send(const std::string& bytes);
    void flushWrites();
    void closeInput();   // EOF on the server's stdin, after what is queued
    void doClose();
    bool readAvailable();   // true at EOF
    void ended();
    void log(const char* prefix, const char* data, size_t n);
    static gboolean onReadable(int fd, GIOCondition cond, gpointer self);
    static gboolean onWritable(int fd, GIOCondition cond, gpointer self);
    static void onWaited(GObject* src, GAsyncResult* res, gpointer data);

    std::string key_, name_;
    GSubprocess* proc_ = nullptr;
    int pid_ = 0;
    int inFd_ = -1, outFd_ = -1;   // our ends of the server's stdin and stdout
    guint readId_ = 0, writeId_ = 0;
    std::string out_;              // bytes waiting for room in the pipe
    bool closeWanted_ = false, inputClosed_ = false;
    bool running_ = false, stopping_ = false;
    int logFd_ = -1;
    std::unique_ptr<Lsp::Client> client_;
    // A remote server: its host, its id there, whether the host said it
    // started, why it did not, what its stderr ended with.
    std::shared_ptr<RemoteHost> remote_;
    bool isRemote_ = false;
    std::uint64_t remoteId_ = 0;
    bool remoteStarted_ = false;
    std::string refusal_, errTail_;
    Json refusalInfo_;
};

}  // namespace lspgtk
