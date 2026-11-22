// SessionServer.h — a headless session host on a Unix socket, and a
// renderer's connection to it (SESSION-DESIGN.md, stage 6). `mutible host
// --folder <dir>` runs a HostServer: a SessionHost with no window, which
// renderers attach to and detach from over the socket, speaking the
// session's protocol (SessionProtocol.h) one JSON line a message. Several
// renderers at once; detaching leaves documents, windows and terminals as
// they are, and a terminal nobody shows keeps running.
//
// One host per folder: the socket is named for the folder's resolved path
// (host-<16 hex>.sock) in the control channel's socket folder
// (ControlPaths.h: a 0700 folder of the user's own), and a lock file beside
// it is held for the host's life, so a second host for the folder finds the
// first and stops. A socket left by a host that died is removed by the next
// one (it holds the lock, so nothing listens there). Peers of another user
// are refused, both ways.
//
// Backups (HostOptions::backupDir, stage 6's last step before the default
// flips): an unsaved document no app's window has open is backed up by the
// host into the apps' store, under the host's pid and start, so it outlives
// a crash, a kill or a reboot of the host: the folder's next window
// restores it as it restores its own (SessionHost.h, "Backups").
//
// Lifetime (the owner's answer, 2026-11-16): the host ends once it has had
// no renderer for `idleMs` (10 minutes) and nothing is unsaved or running
// (Host::busyReason); until then it lives on, however long that is.
//
// Over stdin and stdout (stage 8, the Mac over SSH): a renderer reaches a
// host through a command's pipes (HostLink::spawn), which for a window over
// SSH is the user's own ssh running `mutible host --stdio <path>` on the
// other machine (SshTarget, sshCommand). That command finds the folder's
// host there, or starts one detached that outlives the connection, and
// relays bytes between its standard input and output and the host's socket
// ("A host that outlives the connection", below). A host may also serve one
// renderer on its own standard input and output (startStdio): no socket,
// no lock, ending as soon as its input closes; `--stdio` falls back to
// that when no socket folder can be had.
//
// Pure C++17 and POSIX (Linux and macOS): the server's own thread polls the
// listening socket and the connections; the host's thread is SessionHost's.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SessionClient.h"
#include "SessionHost.h"

namespace Shared {

// The folder's resolved path (realpath), "" when it is not a folder.
std::string resolveFolder(const std::string& dir);
// What `mutible host --stdio PATH` serves (stage 8): PATH a folder; a file,
// whose folder it serves and which its Welcome names; or a file not there
// yet in a folder that is (`isNew`: the window opens it empty, and its
// first save makes it, as `mutible new.txt` does on the Mac). `file` is
// spelled under `folder` as resolved, "" for a folder. False, with why in
// the CLI's words, when PATH is none of these (no such folder, a name
// that is not one).
struct StdioTarget {
    std::string folder;
    std::string file;
    bool isNew = false;
};
bool stdioTarget(const std::string& path, StdioTarget* out, std::string* why);
// The host socket's file name for a resolved folder: host-<16 hex>.sock.
std::string hostSocketName(const std::string& folder);
// A folder the server made: a real folder (not a link) of this user's that
// nobody else can enter.
bool trustedSocketFolder(const std::string& dir);
// Makes `dir` (0700) when missing; false, with why, unless it is then
// trusted.
bool makeSocketFolder(const std::string& dir, std::string* error);
// The socket of the host serving `folder` (resolved) in the first of
// `dirs` holding one of this user's: "" when there is none.
std::string findHostSocket(const std::string& folder, const std::vector<std::string>& dirs);

struct HostOptions {
    std::string folder;                 // resolved
    std::string socketDir;              // where the socket goes
    long long idleMs = 10 * 60 * 1000;  // with no renderer and nothing to lose
    unsigned delayUs = 0;               // SessionHost's, for checks
    // The apps' backup store (Backups::defaultDir), where the host keeps
    // backups of the unsaved documents no app's window has open
    // (Host::keepBackups), owned by this process; "" keeps none.
    std::string backupDir;
    // Watch the disk (SessionHost::watchDisk, DiskWatch.h): the folder and
    // what renderers' trees have open, so trees and Source Control follow
    // it and a clean document no app's window has open follows its file.
    // `mutible host` does; the checks choose.
    bool watchDisk = false;
    // The host's own environment and login shell, for a terminal a renderer
    // on another machine asks to run there (TermSpec's hostShell; the
    // environment as "NAME=value"). Empty: the password database's shell,
    // and the renderer's environment alone.
    std::vector<std::string> shellEnv;
    std::string loginShell;
    // This machine's installed shell integration scripts (HostIntegration.h's
    // shippedFile), which the host sets up itself for such a terminal when
    // its renderer asks (TermSpec's integration); "" for one not installed.
    std::string zshIntegration, bashIntegration;
    // `mutible host --stdio` given a file: its path (spelled under `folder`),
    // which the Welcome names so the renderer shows it; `askedFileNew` when
    // there is no file there yet (the Welcome says "new").
    std::string askedFile;
    bool askedFileNew = false;
};

class HostServer {
public:
    enum class Start { Ok, Running, Failed };
    HostServer();
    ~HostServer();   // the host ends: its shells hung up, the socket removed
    HostServer(const HostServer&) = delete;
    HostServer& operator=(const HostServer&) = delete;

    // Takes the folder's lock and listens: Running (with the other host's
    // socket in `error`) when a host serves the folder already.
    Start start(const HostOptions& o, std::string* error);
    // Serves one renderer on `in` (read) and `out` (written), its standard
    // input and output (`mutible host --stdio`), instead of a socket: no
    // socket, no lock (another host of the folder, or a second over stdio,
    // may run beside it), and run() returns true as soon as `in` closes or
    // brings what is not a message. `socketDir` is not used. Both
    // descriptors are the server's from then on.
    Start startStdio(const HostOptions& o, int in, int out, std::string* error);
    // Serves renderers until the lifetime ends (true) or stop() (false).
    bool run();
    // Any thread: run() returns at its next turn.
    void stop();
    const std::string& socketPath() const { return socketPath_; }
    SessionHost& host() { return *host_; }
    // Renderers connected now (any thread).
    std::size_t renderers();

private:
    struct Conn {
        int fd = -1;
        int outFd = -1;            // over stdio: written here, read from fd (pipes)
        ClientId id = 0;
        std::string in;            // the server's thread only
        std::size_t scanned = 0;   // of `in`, no newline before
        std::string out;           // under mu_
        bool dead = false;         // under mu_
    };
    HostOptions opt_;
    std::string socketPath_, lockPath_;
    int listen_ = -1, lock_ = -1;
    int wake_[2] = {-1, -1};
    std::unique_ptr<SessionHost> host_;
    std::mutex mu_;
    std::map<ClientId, std::shared_ptr<Conn>> conns_;
    std::atomic<bool> stop_{false};
    bool stdio_ = false;   // startStdio: one renderer, ended when its input closes
    bool makeHost(const HostOptions& o);
    void wake();
    void accept();
    void readFrom(Conn& c);
    void writeTo(Conn& c);   // under mu_
};

// A renderer's connection to a host's socket, or to a command's pipes (a
// host over ssh): a Client's Send writes here, and a reader thread of its
// own delivers the host's messages.
class HostLink {
public:
    // Connects to `path`: null, with why, when it cannot; `refused` when
    // nothing listens there (a socket a dead host left).
    static std::unique_ptr<HostLink> open(const std::string& path, std::string* error,
                                          bool* refused = nullptr);
    // Starts `argv` (argv[0] a path, or a name found in the PATH of `env`)
    // with `env` ("NAME=value", its whole environment) and speaks over its
    // standard input and output: a host on another machine through ssh
    // (stage 8), or a stand-in for ssh in the checks. Its standard error is
    // kept (the last 8 KB, stderrText) for saying why it ended; it runs in a
    // session of its own with no other descriptor of this process's and
    // signals at their defaults. Null, with why, when it cannot be started.
    static std::unique_ptr<HostLink> spawn(const std::vector<std::string>& argv,
                                           const std::vector<std::string>& env, std::string* error);
    ~HostLink();   // close()
    HostLink(const HostLink&) = delete;
    HostLink& operator=(const HostLink&) = delete;

    // Starts reading: each message to `deliver` (on the reader thread), and
    // `closed` once when the host goes or sends what is not a message.
    void start(std::function<void(Message&&)> deliver, std::function<void()> closed = {});
    // `client` speaks through this link: its sends written here, the host's
    // messages delivered to it, then its Hello; when the link closes, the
    // client hears the host is gone (Client::hostLost), so it waits no more.
    void connect(Client& client);
    // One message, written whole (any thread). False once the host is gone.
    bool send(const Message& m);
    // Detaches: the host forgets this renderer, and keeps everything else.
    // A command's link closes the command's input, which ends a `mutible
    // host --stdio` (and the ssh carrying it), and hangs the command up
    // when it is still there a second later.
    void close();
    bool closed() const { return closed_.load(); }
    // A command's: its process group killed now (SIGKILL), when it has not
    // ended yet, so the close() that follows does not wait for it to hang
    // up: a window over SSH whose connecting the user cancelled, or that
    // gave up on it. Nothing for a socket.
    void terminate();

    // A command's: its pid (0 for a socket), and once the link closed and
    // the command ended, its wait status (-1 until then, and for a socket).
    // What it wrote to its standard error, the last 8 KB (any thread).
    int pid() const { return pid_; }
    int exitStatus() const { return status_.load(); }
    std::string stderrText();

private:
    explicit HostLink(int fd) : fd_(fd) {}
    int fd_ = -1;     // a socket's, or the command's output (read)
    int out_ = -1;    // a command's input (written), -1 for a socket
    int err_ = -1;    // a command's standard error (read by errReader_)
    int pid_ = 0;
    std::atomic<int> status_{-1};
    std::mutex writeMu_;
    std::thread reader_, errReader_;
    std::atomic<bool> closed_{false};
    std::mutex errMu_;
    std::condition_variable errDone_;
    bool errEnded_ = false;   // under errMu_: the command's standard error closed
    std::string errText_;
    void reap(int waitMs);
};

// ---- a window on another machine (stage 8): `mutible ssh://host/path`

// What an ssh:// address names: ssh://[user@]host[:port][/path]. A path
// starting "/~" is the remote home's ("ssh://pi/~/proj" is ~/proj there),
// any other is taken as written, and none is the remote home ("~").
// %XX in the user and path are decoded.
struct SshTarget {
    std::string user;   // "" for ssh's own choice (~/.ssh/config, the local name)
    std::string host;   // a name or an address; an IPv6 address without its brackets
    int port = 0;       // 0 for ssh's own choice
    std::string path;   // "~", "~/x" or "/x" as the remote takes it
};
// False, with why, when `url` is not one: not ssh://, no host, a host or
// user that would read as an option to ssh (a leading '-') or that holds
// what no host name holds, a port that is not 1 to 65535, a path with a
// NUL or a newline.
bool parseSshUrl(const std::string& url, SshTarget* out, std::string* error);
// "host:path" as a window's title names it ("pi:~/proj", "eric@pi:/srv").
std::string sshLabel(const SshTarget& t);
// ssh://[user@]host[:port] (no path): which machine and account, the same
// for every folder there.
std::string sshMachineKey(const SshTarget& t);
// `s` in single quotes for a POSIX shell ('it'\''s'), as ssh hands the
// remote command to the remote user's shell.
std::string shellQuote(const std::string& s);
// The command line that runs `mutible host --stdio <path>` on the target:
// `ssh` (the program; the user's own, with their ~/.ssh/config, keys and
// agent), never asking anything on a terminal nobody sees (BatchMode, no
// pty), told when the connection stops answering (ServerAlive), with
// `-p` and `-l` from the target and `--` before the host, then the remote
// command as one argument, the path quoted for the remote shell.
std::vector<std::string> sshCommand(const SshTarget& t, const std::string& ssh);
// Why a host over ssh never answered, in words for the window: ssh's exit
// status (as waitpid gives it; -1 unknown) and what it wrote to its
// standard error. Says to run `ssh host` once in a terminal when ssh
// needed a password, a passphrase or a host key confirmed; that mutible is
// not installed there (exit 127) or too old (it did not know `--stdio`,
// or `host`); else ssh's own last line.
std::string explainSshFailure(const SshTarget& t, int status, const std::string& stderrText);

// ---- a window over SSH connecting (stage 8), as its progress line tells it
//
// `mutible host --stdio` says kStdioStartedLine on its standard error (which
// ssh carries back) as soon as it runs, so a window waiting for its first
// Welcome can tell ssh's connection made (mutible starting there) from ssh
// still connecting. A host from before this says nothing, and the window's
// line goes from connecting straight to opening. explainSshFailure leaves
// the line out of what it quotes.
extern const char* const kStdioStartedLine;
// Whether `stderrText` holds kStdioStartedLine as a line of its own.
bool stdioStarted(const std::string& stderrText);
// The first connect of a window over SSH: where it is, what its line says,
// and every stage it has been through (so a stage too short to be seen is
// still known to have passed). Connecting until mutible is heard starting
// there, Starting until the host's Welcome, Opening while the window is
// made on the folder the Welcome named, then Open; Failed or Cancelled end
// it from any stage before Open. Once ended it moves no more.
class ConnectProgress {
public:
    enum class Stage { Connecting, Starting, Opening, Open, Failed, Cancelled };
    Stage stage() const { return stage_; }
    bool passed(Stage s) const { return (passed_ & (1u << unsigned(s))) != 0; }
    bool ended() const { return stage_ == Stage::Open || stage_ == Stage::Failed || stage_ == Stage::Cancelled; }
    // What the command (ssh) has said on its standard error so far.
    void heard(const std::string& stderrText);
    // The host's Welcome, naming its folder.
    void welcomed(const std::string& folder);
    void opened();
    void failed();
    // False when it had ended already (opened, failed or cancelled).
    bool cancel();
    // The line a window shows for it: "Connecting to eric@pi…", "Starting
    // mutible on eric@pi…", "Opening /home/eric/proj on eric@pi…", ...
    std::string line(const SshTarget& t) const;

private:
    void moveTo(Stage s);
    Stage stage_ = Stage::Connecting;
    unsigned passed_ = 1u << unsigned(Stage::Connecting);
    std::string folder_;
};

// ---- a host that outlives the connection (stage 8)
//
// `mutible host --stdio PATH` finds the folder's host (one per folder, its
// socket in the user's private socket folder: hostSocketName,
// findHostSocket), starts one detached when there is none (`mutible host
// --folder DIR --lasting`, by startDetached), and relays bytes between its
// standard input and output and that host's socket (relayStdio) until
// either ends. A dropped connection ends only the relay: the host keeps
// the window's documents (text, unsaved state, undo) and its terminals
// (running, their screens) for the window's next connection, whose Hello
// names the old one (Client::setResume, Host::setDrop). The host ends once
// it has had no renderer for lastingIdleMs and nothing is unsaved or
// running, as every headless host does.

// How long a host started for a window over SSH waits with no renderer
// before it ends (when nothing is unsaved or running): `minutes`, the
// environment's MUTIBLE_HOST_IDLE_MINUTES, when it is a number of minutes
// (fractions allowed), else kLastingIdleMs, four hours: a laptop closed
// over lunch, or a train through a tunnel, finds its terminals still there.
constexpr long long kLastingIdleMs = 4LL * 60 * 60 * 1000;
long long lastingIdleMs(const char* minutes);
// Where `--stdio` looks for a running host: `folders` (ControlPaths.h's
// socketFolders, a host the user's own app started included) and then
// `lasting`, the folder a lasting host makes its socket in, when it is not
// one of them.
std::vector<std::string> withLastingFolder(std::vector<std::string> folders, const std::string& lasting);
// Starts `argv` (argv[0] a path) detached from this process, in this
// process's environment: in a session of its own (no controlling terminal,
// so the end of the login that started it hangs nothing up; SIGHUP is
// ignored besides), not this process's child (the go-between that starts
// it is reaped here), no descriptor of this process's, standard input
// /dev/null, standard output and error appended to `logPath` (made 0600;
// /dev/null when ""). Its pid, or 0 with why.
int startDetached(const std::vector<std::string>& argv, const std::string& logPath, std::string* error);
// The folder's host, connected (a socket descriptor): found in `dirs`
// (findHostSocket), else started by `start` (its pid, or 0 with why) and
// waited for, at most `timeoutMs`. A socket nothing listens on (a host
// that died) counts as none; one whose host is another user's is refused.
// -1, with why, when there is none to be had.
int connectOrStartHost(const std::string& folder, const std::vector<std::string>& dirs,
                       const std::function<int(std::string* why)>& start, int timeoutMs,
                       std::string* error);
// Relays bytes between a renderer's `in` and `out` (ssh's pipes) and a
// host's socket `sock`, until either side ends: what the renderer sends
// goes to the host as it is, and what the host sends goes to the renderer
// as it is, but for its Welcome, whose json names `askedFile` (and says
// "new" when `askedNew`) as a `--stdio` host's own Welcome did
// (Host::setAskedFile). Blocking writes both ways; closes nothing.
void relayStdio(int in, int out, int sock, const std::string& askedFile, bool askedNew);
// The wait before each try at connecting again after a window over SSH lost
// its connection (BatchMode: no try asks anything), `attempt` 0 for the
// first: half a second, then doubling, at most 30 s; -1 once
// kReconnectAttempts have been made (the window then offers Reconnect).
constexpr int kReconnectAttempts = 12;
long long reconnectDelayMs(int attempt);

// What `mutible attach --dump` prints: the session `client` is connected
// to, as text. The folder, then each document (path, version, unsaved or
// not) with its text, then each terminal (size, shell, what runs) with its
// screen as a renderer attaching would draw it from the host's replay. The
// terminals are attached to for that, so the client is left showing them.
// "" when the host does not answer.
std::string dumpSession(Client& client);

}  // namespace Shared
