// GitRun.cpp — see GitRun.h. Moved out of GitPanel.cpp when the panel gained
// its verbs (PARITY.md 2.9): git apply's patch on stdin, and a session of
// its own for git talking to a remote.
#include "GitRun.h"
#include "EnvLock.h"
#include "RemoteWorkspace.h"
#include "SessionGit.h"
#include "StartupLog.h"

#include <gio/gio.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>

namespace {

std::string bytesOf(GBytes* b) {
    if (!b) return std::string();
    gsize n = 0;
    const char* p = static_cast<const char*>(g_bytes_get_data(b, &n));
    return std::string(p ? p : "", n);
}

// Run in the child between fork and exec, so async-signal-safe only. git
// leads a process group of its own, so a time limit stops what it started
// along with it.
void ownProcessGroup(gpointer) { setpgid(0, 0); }
// A command that talks to a remote also leaves mutible's session, so ssh
// finds no terminal to ask on (its group is then the same, led by git).
void ownSession(gpointer) { setsid(); }

// What the two callbacks of one run hand back. Each holds a reference, so a
// callback that comes late (or never, for a git stuck in the kernel) finds
// it still there.
struct Run {
    bool waited = false;        // the wait callback ran
    bool communicated = false;  // the communicate callback ran
    GBytes* out = nullptr;
    GBytes* err = nullptr;
    GError* error = nullptr;
    ~Run() {
        if (out) g_bytes_unref(out);
        if (err) g_bytes_unref(err);
        g_clear_error(&error);
    }
};

void waited(GObject* src, GAsyncResult* res, gpointer data) {
    auto* run = static_cast<std::shared_ptr<Run>*>(data);
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, nullptr);
    (*run)->waited = true;
    delete run;
}

void communicated(GObject* src, GAsyncResult* res, gpointer data) {
    auto* run = static_cast<std::shared_ptr<Run>*>(data);
    Run& r = **run;
    g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &r.out, &r.err, &r.error);
    r.communicated = true;
    delete run;
}

// Iterates `ctx` until `done` holds or `deadline` (monotonic microseconds)
// passes; says whether it held.
bool iterateUntil(GMainContext* ctx, gint64 deadline, const std::function<bool()>& done) {
    while (!done()) {
        const gint64 left = deadline - g_get_monotonic_time();
        if (left <= 0) return false;
        GSource* t = g_timeout_source_new((guint)std::min<gint64>((left + 999) / 1000, G_MAXINT));
        g_source_set_callback(t, [](gpointer) -> gboolean { return G_SOURCE_REMOVE; }, nullptr,
                              nullptr);
        g_source_attach(t, ctx);
        g_main_context_iteration(ctx, TRUE);
        g_source_destroy(t);
        g_source_unref(t);
    }
    return true;
}

thread_local const GitRunner* tRunner = nullptr;

GitResult hostFailed(const std::string& why) {
    GitResult r;
    r.err = why;
    return r;
}

// What one request crossing to the main loop and back holds.
struct HostWait {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    GitResult result;
    void finish(GitResult r) {
        std::lock_guard<std::mutex> lock(m);
        if (done) return;
        result = std::move(r);
        done = true;
        cv.notify_all();
    }
};

struct HostAsk {
    std::weak_ptr<RemoteWorkspace> ws;
    std::string dir;
    std::vector<std::string> args;
    double limit;
    std::string input;
    std::shared_ptr<HostWait> wait;
};

// On the main loop: the request goes to the host.
gboolean askHost(gpointer p) {
    std::unique_ptr<HostAsk> a(static_cast<HostAsk*>(p));
    std::shared_ptr<RemoteWorkspace> ws = a->ws.lock();
    std::shared_ptr<HostWait> w = a->wait;
    if (!ws || !ws->live()) {
        w->finish(hostFailed(Shared::gitLostText(ws ? ws->label() : "the other machine")));
        return G_SOURCE_REMOVE;
    }
    const std::string label = ws->label();
    if (!ws->hostRunsGit()) {
        w->finish(hostFailed(Shared::gitTooOldText(label)));
        return G_SOURCE_REMOVE;
    }
    const bool sent = ws->git(a->dir, a->args, std::move(a->input), a->limit,
                              [w, label](bool answered, const Shared::GitReply& reply) {
                                  if (!answered) return w->finish(hostFailed(Shared::gitLostText(label)));
                                  GitResult r;
                                  r.status = reply.status;
                                  r.out = reply.out;
                                  r.err = reply.err;
                                  r.timedOut = reply.timedOut;
                                  w->finish(std::move(r));
                              });
    if (!sent) w->finish(hostFailed(Shared::gitLostText(label)));
    return G_SOURCE_REMOVE;
}

// One work tree file read crossing to the main loop and back.
struct FileWait {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    FolderSearch::ReadStatus status = FolderSearch::ReadStatus::Unreadable;
    std::string bytes;
    void finish(FolderSearch::ReadStatus s, std::string b) {
        std::lock_guard<std::mutex> lock(m);
        if (done) return;
        status = s;
        bytes = std::move(b);
        done = true;
        cv.notify_all();
    }
};
struct FileAsk {
    std::weak_ptr<RemoteWorkspace> ws;
    std::string path;
    std::shared_ptr<FileWait> wait;
};
gboolean askFile(gpointer p) {
    std::unique_ptr<FileAsk> a(static_cast<FileAsk*>(p));
    std::shared_ptr<RemoteWorkspace> ws = a->ws.lock();
    std::shared_ptr<FileWait> w = a->wait;
    if (!ws || !ws->live() ||
        !ws->readFile(a->path, [w](bool ok, bool tooLarge, std::string&& bytes) {
            w->finish(tooLarge ? FolderSearch::ReadStatus::TooLarge
                      : ok     ? FolderSearch::ReadStatus::Ok
                               : FolderSearch::ReadStatus::Missing,
                      std::move(bytes));
        }))
        w->finish(FolderSearch::ReadStatus::Unreadable, "");
    return G_SOURCE_REMOVE;
}

}  // namespace

GitRunScope::GitRunScope(const GitRunnerPtr& runner) : previous_(tRunner) {
    if (runner) tRunner = runner.get();
}
GitRunScope::~GitRunScope() { tRunner = previous_; }
bool gitRunsOnHost() { return tRunner != nullptr; }
std::function<FolderSearch::ReadStatus(const std::string&, std::string*)> hostWorkReader() {
    return tRunner ? tRunner->readFile : nullptr;
}

GitRunnerPtr hostGitRunner(const std::shared_ptr<RemoteWorkspace>& wsShared) {
    std::weak_ptr<RemoteWorkspace> weak = wsShared;
    auto runner = std::make_shared<GitRunner>(
        [weak](const std::string& dir, const std::vector<std::string>& args, double limit,
                      const std::string* input) -> GitResult {
            // The answer comes on the main loop: never wait for it there.
            if (g_main_context_is_owner(g_main_context_default()))
                return hostFailed("git for a window over SSH runs off the main thread.");
            auto wait = std::make_shared<HostWait>();
            g_idle_add_full(G_PRIORITY_DEFAULT, askHost,
                            new HostAsk{weak, dir, args, limit, input ? *input : std::string(), wait},
                            nullptr);
            // The host stops git at its own limit; this waits that long and a
            // little more for the answer to cross.
            const double seconds = (limit > 0 ? limit : Git::timeoutFor(args)) + 30;
            std::unique_lock<std::mutex> lock(wait->m);
            if (!wait->cv.wait_for(lock, std::chrono::milliseconds((long long)(seconds * 1000)),
                                   [&wait] { return wait->done; })) {
                GitResult r = hostFailed("The other machine did not answer within " +
                                         Git::durationText(seconds) + ": " + Git::commandLine(args));
                r.timedOut = true;
                wait->done = true;   // a late answer is dropped
                return r;
            }
            return std::move(wait->result);
        });
    runner->readFile = [weak](const std::string& path, std::string* out) -> FolderSearch::ReadStatus {
        if (g_main_context_is_owner(g_main_context_default())) return FolderSearch::ReadStatus::Unreadable;
        auto wait = std::make_shared<FileWait>();
        g_idle_add_full(G_PRIORITY_DEFAULT, askFile, new FileAsk{weak, path, wait}, nullptr);
        std::unique_lock<std::mutex> lock(wait->m);
        if (!wait->cv.wait_for(lock, std::chrono::seconds(60), [&wait] { return wait->done; })) {
            wait->done = true;   // a late answer is dropped
            return FolderSearch::ReadStatus::Unreadable;
        }
        *out = std::move(wait->bytes);
        return wait->status;
    };
    return runner;
}

const std::string& gitExecutable() {
    static std::string git;
    static std::once_flag once;
    std::call_once(once, [] {
        EnvLock::EnvRead env;
        if (char* p = g_find_program_in_path("git")) {
            git = p;
            g_free(p);
        }
    });
    return git;
}

// A git that exits while something it started keeps its pipes open gets the
// time limit's treatment too, with its own message. A git that exits without
// reading all of `input` leaves a broken pipe, which GIO reports and does not
// raise as SIGPIPE; what git printed then is still its answer.
GitResult runGitWithin(const std::string& dir, const std::vector<std::string>& args,
                       double limit, const std::string* input) {
    if (tRunner) return (*tRunner)(dir, args, limit, input);
    GitResult r;
    const std::string& git = gitExecutable();
    if (git.empty()) {
        r.err = "Git was not found. Install it with your package manager, "
                "for example sudo apt install git.";
        return r;
    }
    int flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (input) flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    // The launcher copies the process environment when it is made; a setenv
    // on another thread meanwhile corrupts the heap (EnvLock.h).
    GSubprocessLauncher* l;
    {
        EnvLock::EnvRead env;
        l = g_subprocess_launcher_new((GSubprocessFlags)flags);
    }
    for (const GitUi::EnvVar& e : GitUi::gitEnvironment())
        g_subprocess_launcher_setenv(l, e.name, e.value, TRUE);
    g_subprocess_launcher_set_cwd(l, dir.c_str());
    g_subprocess_launcher_set_child_setup(
        l, Git::talksToRemote(args) ? ownSession : ownProcessGroup, nullptr, nullptr);
    std::vector<const char*> argv;
    argv.push_back(git.c_str());
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* err = nullptr;
    StartupLog::once("git", "git: first command (git %s)", args.empty() ? "" : args[0].c_str());
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), &err);
    g_object_unref(l);
    if (!p) {
        r.err = err ? err->message : "Git could not be started.";
        g_clear_error(&err);
        return r;
    }
    // The group's id is git's pid. Never 0 below: kill(0, ...) would signal
    // mutible's own group.
    const char* id = g_subprocess_get_identifier(p);
    const pid_t pid = id ? (pid_t)std::atol(id) : 0;
    auto killGroup = [pid](int sig) {
        if (pid > 0) kill(-pid, sig);
    };

    // The callbacks come to a context of this thread's own, iterated only
    // here, so nothing waits on the main loop.
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GCancellable* cancel = g_cancellable_new();
    auto run = std::make_shared<Run>();
    GBytes* in = input ? g_bytes_new(input->data(), input->size()) : nullptr;
    g_subprocess_wait_async(p, cancel, waited, new std::shared_ptr<Run>(run));
    g_subprocess_communicate_async(p, in, cancel, communicated, new std::shared_ptr<Run>(run));
    if (in) g_bytes_unref(in);

    auto at = [](double seconds) {
        return g_get_monotonic_time() + (gint64)(seconds * G_USEC_PER_SEC);
    };
    const gint64 deadline = at(limit);
    // A wait that failed for any reason but cancelling counts as over too.
    auto exited = [&run] { return run->waited; };
    auto drained = [&run] { return run->communicated; };
    if (!iterateUntil(ctx, deadline, exited)) {
        r.timedOut = true;
        r.err = Git::timeoutMessage(args, limit);
        killGroup(SIGTERM);
        if (!iterateUntil(ctx, at(2), exited)) {
            killGroup(SIGKILL);
            iterateUntil(ctx, at(2), exited);
        } else {
            killGroup(SIGKILL);   // what git started and left in its group
        }
    } else if (!iterateUntil(ctx, deadline, drained) && !iterateUntil(ctx, at(2), drained)) {
        // git exited, but something it started (outside its group, or still
        // running in it) holds the pipes open, which is a hang of its own.
        r.timedOut = true;
        r.err = Git::heldOutputMessage(args, limit);
        killGroup(SIGKILL);
    } else if (run->error && !(input && g_error_matches(run->error, G_IO_ERROR,
                                                         G_IO_ERROR_BROKEN_PIPE))) {
        r.err = run->error->message;
    } else {
        r.out = bytesOf(run->out);
        r.err = bytesOf(run->err);
        if (g_subprocess_get_if_exited(p))
            r.status = g_subprocess_get_exit_status(p);
        else if (g_subprocess_get_if_signaled(p))
            r.status = 128 + g_subprocess_get_term_sig(p);
    }
    // Whatever is still pending ends now, so neither callback is left for a
    // context nobody iterates. One for a git stuck in the kernel may never
    // come; its Run then simply stays allocated.
    g_cancellable_cancel(cancel);
    iterateUntil(ctx, at(2), [&run] { return run->waited && run->communicated; });
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    g_object_unref(cancel);
    g_object_unref(p);
    return r;
}

GitResult runGit(const std::string& dir, const std::vector<std::string>& args,
                 const std::string* input) {
    return runGitWithin(dir, args, Git::timeoutFor(args), input);
}

std::string gitFailure(const GitResult& r) { return GitUi::failureText(r.status, r.out, r.err); }

GitUi::Snapshot gitSnapshotAt(const std::string& root) {
    GitUi::Snapshot s;
    if (!gitRunsOnHost() && gitExecutable().empty()) {
        s.notice = "Git was not found. Install it with your package manager, "
                   "for example sudo apt install git.";
        return s;
    }
    GitResult top = runGit(root, GitUi::topLevelArgs());
    if (!top.ok()) {
        if (top.err.find("not a git repository") != std::string::npos)
            s.notice = "This folder is not in a git repository.";
        else
            s.errorText = gitFailure(top);
        return s;
    }
    if (!GitUi::parseTopLevel(top.out, s.topLevel, s.gitDir)) {
        s.errorText = gitFailure(top);
        return s;
    }
    s.inRepository = true;
    GitResult st = runGit(s.topLevel, GitUi::statusArgs());
    if (!st.ok()) {
        s.errorText = gitFailure(st);
        return s;
    }
    GitUi::applyStatus(s, Git::parseStatus(st.out));
    // The remotes, for Publish Branch and for what Fetch and autofetch use.
    GitResult remotes = runGit(s.topLevel, GitUi::remoteArgs());
    if (remotes.ok()) s.remotes = GitUi::parseRemotes(remotes.out);
    return s;
}
