// SessionLsp.cpp — see SessionLsp.h.
#include "SessionLsp.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define MC_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define MC_ENVIRON environ
#endif

namespace Shared {

// ---------------------------------------------------------------- the request

Json serverRequestJson(const ServerRequest& r) {
    Json j = Json::object();
    j.set("key", Json(r.key));
    Json commands = Json::array();
    for (const auto& words : r.commands) {
        Json w = Json::array();
        for (const std::string& s : words) w.push(Json(s));
        commands.push(std::move(w));
    }
    j.set("commands", std::move(commands));
    j.set("allowed", Json(r.allowed));
    if (!r.fileDir.empty()) j.set("fileDir", Json(r.fileDir));
    return j;
}

ServerRequest serverRequestFrom(const Json& j) {
    ServerRequest r;
    r.key = j["key"].asString();
    for (const Json& c : j["commands"].elements()) {
        std::vector<std::string> words;
        for (const Json& w : c.elements())
            if (w.isString()) words.push_back(w.asString());
        if (!words.empty() && !words[0].empty()) r.commands.push_back(std::move(words));
        if (r.commands.size() >= 8) break;
    }
    r.allowed = j["allowed"].asBool();
    r.fileDir = j["fileDir"].asString();
    return r;
}

// ---------------------------------------------------------------- finding a server

ServerFs realServerFs() {
    ServerFs fs;
    fs.isDir = [](const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };
    fs.isExecutable = [](const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
    };
    fs.list = [](const std::string& d) {
        std::vector<std::string> out;
        DIR* dir = opendir(d.c_str());
        if (!dir) return out;
        while (dirent* e = readdir(dir)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
            if (out.size() >= 1000) break;
        }
        closedir(dir);
        return out;
    };
    return fs;
}

namespace {

// Version folders, newest first (v22.17.0 before v18.20.4): each part
// compared as a number where both are numbers.
bool newerVersion(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit((unsigned char)a[ie])) ie++;
            while (je < b.size() && std::isdigit((unsigned char)b[je])) je++;
            const std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            const std::string ta = na.substr(std::min(na.find_first_not_of('0'), na.size()));
            const std::string tb = nb.substr(std::min(nb.find_first_not_of('0'), nb.size()));
            if (ta.size() != tb.size()) return ta.size() > tb.size();
            if (ta != tb) return ta > tb;
            i = ie;
            j = je;
            continue;
        }
        if (a[i] != b[j]) return a[i] > b[j];
        i++;
        j++;
    }
    return a.size() - i > b.size() - j;
}

std::string envValue(const std::vector<std::string>& env, const std::string& name) {
    const std::string prefix = name + "=";
    for (const std::string& e : env)
        if (e.compare(0, prefix.size(), prefix) == 0) return e.substr(prefix.size());
    return std::string();
}

std::string folderOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string realOr(const std::string& path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : path;
}

}  // namespace

std::vector<std::string> serverSearchDirs(const std::string& path, const std::string& home, const ServerFs& fs) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& d) {
        if (d.empty() || !seen.insert(d).second) return;
        if (fs.isDir && fs.isDir(d)) out.push_back(d);
    };
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t colon = path.find(':', start);
        const std::string d = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        add(d);
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    if (!home.empty()) {
        for (const char* sub : {"/.local/bin", "/.cargo/bin", "/go/bin", "/.npm-global/bin", "/.volta/bin",
                                "/.bun/bin", "/.deno/bin", "/.pyenv/shims", "/bin"})
            add(home + sub);
        // nvm's and fnm's Node versions, newest first.
        for (const std::string& root : {home + "/.nvm/versions/node", home + "/.local/share/fnm/node-versions"}) {
            std::vector<std::string> versions = fs.list ? fs.list(root) : std::vector<std::string>();
            std::sort(versions.begin(), versions.end(), newerVersion);
            const bool fnm = root.find("fnm") != std::string::npos;
            for (const std::string& v : versions)
                if (!v.empty() && v[0] != '.') add(root + "/" + v + (fnm ? "/installation/bin" : "/bin"));
        }
    }
    for (const char* d : {"/usr/local/bin", "/opt/homebrew/bin", "/home/linuxbrew/.linuxbrew/bin",
                          "/usr/local/go/bin", "/snap/bin", "/usr/bin", "/bin"})
        add(d);
    return out;
}

std::string findServerProgram(const std::string& word, const std::vector<std::string>& dirs,
                              const std::string& home, const ServerFs& fs) {
    if (word.empty() || !fs.isExecutable) return std::string();
    if (word.find('/') != std::string::npos) {
        std::string p = word;
        if (p.compare(0, 2, "~/") == 0 && !home.empty()) p = home + p.substr(1);
        return p[0] == '/' && fs.isExecutable(p) ? p : std::string();
    }
    for (const std::string& d : dirs) {
        const std::string p = d + "/" + word;
        if (fs.isExecutable(p)) return p;
    }
    return std::string();
}

Lsp::ProjectFs realProjectFs() {
    Lsp::ProjectFs fs;
    fs.exists = [](const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0;
    };
    fs.subfolders = [](const std::string& d) {
        std::vector<std::string> out;
        DIR* dir = opendir(d.c_str());
        if (!dir) return out;
        while (dirent* e = readdir(dir)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
            if (out.size() >= 1000) break;
        }
        closedir(dir);
        return out;
    };
    return fs;
}

ServerPlan planServer(const ServerRequest& r, const std::string& root, const std::string& home,
                      const std::vector<std::string>& env, const ServerFs& fs, const Lsp::ProjectFs& pfs) {
    ServerPlan p;
    if (r.commands.empty()) {
        p.error = "empty";
        return p;
    }
    const std::vector<std::string> dirs = serverSearchDirs(envValue(env, "PATH"), home, fs);
    for (std::size_t i = 0; i < r.commands.size(); i++) {
        const std::vector<std::string>& words = r.commands[i];
        const std::string found = findServerProgram(words[0], dirs, home, fs);
        if (found.empty()) {
            p.tried.push_back(words[0]);
            continue;
        }
        p.argv = words;
        p.argv[0] = found;
        p.command = (int)i;
        break;
    }
    if (p.argv.empty()) {
        p.error = "missing";
        return p;
    }
    // What the folder's own code may do (rust-analyzer, the TypeScript
    // server), checked on this machine's disk with the window's answer.
    const std::string server = Lsp::languageOfKey(r.key);
    if (!Lsp::isAlsoKey(r.key) && Lsp::serverMayRunProjectCode(server)) {
        std::string ownTs;
        if (server == "typescript") {
            const std::string tsserver = findServerProgram("tsserver", dirs, home, fs);
            ownTs = Lsp::ownTypescript(realOr(p.argv[0]), tsserver.empty() ? std::string() : realOr(tsserver),
                                       root, pfs);
        }
        std::string fileDir = r.fileDir;
        if (fileDir.empty() || fileDir[0] != '/') fileDir = root;
        p.code = Lsp::checkProjectCode(server, r.allowed, fileDir, root, home.empty() ? std::string("/") : realOr(home),
                                       ownTs, pfs);
        if (p.code.verdict == Lsp::ProjectCode::Waits) {
            p.error = "waits";
            return p;
        }
    }
    // Its environment: the host's, its PATH the program's folder first.
    std::string path = folderOf(p.argv[0]);
    std::set<std::string> inPath = {path};
    for (const std::string& d : dirs)
        if (inPath.insert(d).second) path += ":" + d;
    p.env.clear();
    for (const std::string& e : env)
        if (e.compare(0, 5, "PATH=") != 0) p.env.push_back(e);
    p.env.push_back("PATH=" + path);
    return p;
}

Json startedJson(const ServerPlan& p) {
    Json j = Json::object();
    Json argv = Json::array();
    for (const std::string& a : p.argv) argv.push(Json(a));
    j.set("argv", std::move(argv));
    j.set("command", Json((std::int64_t)p.command));
    j.set("verdict", Json(p.code.verdict == Lsp::ProjectCode::Restricted ? "restricted" : "free"));
    if (!p.code.file.empty()) j.set("file", Json(p.code.file));
    if (!p.code.initializationOptions.isNull()) j.set("initializationOptions", p.code.initializationOptions);
    if (!p.code.settings.isNull()) j.set("settings", p.code.settings);
    return j;
}

ServerStarted serverStartedFrom(const Json& j) {
    ServerStarted s;
    for (const Json& a : j["argv"].elements())
        if (a.isString()) s.argv.push_back(a.asString());
    s.command = j["command"].isNumber() ? (int)j["command"].asInt() : -1;
    if (j["verdict"].asString() == "restricted") s.code.verdict = Lsp::ProjectCode::Restricted;
    s.code.file = j["file"].asString();
    s.code.initializationOptions = j["initializationOptions"];
    s.code.settings = j["settings"];
    return s;
}

// ---------------------------------------------------------------- the process

ServerProcess::~ServerProcess() {
    for (int* fd : {&in_, &out_, &err_})
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    if (pid_ > 0 && !reaped_) {
        int st = 0;
        if (waitpid(pid_, &st, WNOHANG) != pid_) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
            waitpid(pid_, &st, 0);
        }
        reaped_ = true;
    }
}

bool ServerProcess::start(const std::vector<std::string>& argvIn, const std::vector<std::string>& env,
                          const std::string& cwd, std::string* error) {
    if (argvIn.empty() || argvIn[0].empty()) {
        if (error) *error = "no command to run";
        return false;
    }
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    auto closeAll = [&] {
        for (int* p : {in, out, err})
            for (int i = 0; i < 2; i++)
                if (p[i] >= 0) ::close(p[i]);
    };
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
        if (error) *error = std::string("no pipe: ") + std::strerror(errno);
        closeAll();
        return false;
    }
    for (int* p : {in, out, err})
        for (int i = 0; i < 2; i++) fcntl(p[i], F_SETFD, FD_CLOEXEC);
    std::vector<std::string> args = argvIn;
    std::vector<std::string> envCopy = env;
    if (envCopy.empty())
        for (char** e = MC_ENVIRON; e && *e; e++) envCopy.push_back(*e);
    std::vector<char*> argv, envp;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    for (std::string& e : envCopy) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
#if defined(__APPLE__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)))
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#else
    (void)cwd;
#endif
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;   // a group of its own: signalled as one
#else
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(&at, 0);
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;   // nothing else of the host's
#endif
    posix_spawnattr_setflags(&at, flags);
    pid_t pid = 0;
    const int r = posix_spawn(&pid, argv[0], &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (r != 0) {
        if (error) *error = "could not run " + argvIn[0] + ": " + std::strerror(r);
        ::close(in[1]);
        ::close(out[0]);
        ::close(err[0]);
        return false;
    }
    for (int fd : {in[1], out[0], err[0]}) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef F_SETNOSIGPIPE
    fcntl(in[1], F_SETNOSIGPIPE, 1);   // a server gone is EPIPE, not a signal (macOS)
#endif
    pid_ = (int)pid;
    in_ = in[1];
    out_ = out[0];
    err_ = err[0];
    return true;
}

void ServerProcess::write(const std::string& bytes) {
    if (in_ < 0 || closing_ || bytes.empty()) return;
    pending_ += bytes;
    flush();
}

void ServerProcess::flush() {
    while (in_ >= 0 && !pending_.empty()) {
        const ssize_t n = ::write(in_, pending_.data(), pending_.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            // EPIPE: it is gone; its end comes through its output.
            pending_.clear();
            break;
        }
        pending_.erase(0, (std::size_t)n);
    }
    if (closing_ && pending_.empty() && in_ >= 0) {
        ::close(in_);
        in_ = -1;
    }
}

void ServerProcess::closeInput() {
    closing_ = true;
    flush();
}

bool ServerProcess::readOut(std::string& out) {
    if (out_ < 0) return false;
    char buf[16 << 10];
    while (out.size() < kServerReadChunk) {
        const ssize_t n = ::read(out_, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, (std::size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        ::close(out_);
        out_ = -1;
        return false;
    }
    return true;
}

void ServerProcess::readErr() {
    if (err_ < 0) return;
    char buf[4096];
    for (int i = 0; i < 64; i++) {
        const ssize_t n = ::read(err_, buf, sizeof buf);
        if (n > 0) {
            errTail_.append(buf, (std::size_t)n);
            if (errTail_.size() > kServerErrTail) errTail_.erase(0, errTail_.size() - kServerErrTail);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        ::close(err_);
        err_ = -1;
        return;
    }
}

void ServerProcess::signal(int sig) {
    if (pid_ <= 0 || reaped_) return;
    if (::kill(-pid_, sig) != 0) ::kill(pid_, sig);
}

bool ServerProcess::reap(int* status) {
    if (pid_ <= 0) return true;
    if (!reaped_) {
        int st = 0;
        pid_t r;
        do r = waitpid(pid_, &st, WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == 0) return false;
        reaped_ = true;
        status_ = r == pid_ ? st : -1;
    }
    if (status) *status = status_;
    return true;
}

// ---------------------------------------------------------------- watched files

bool listStamps(const std::string& dir, std::vector<StampEntry>* out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        if (lstat((dir + "/" + n).c_str(), &st) != 0) continue;
        StampEntry s;
        s.name = n;
        s.dir = S_ISDIR(st.st_mode);
#ifdef __APPLE__
        s.mtimeNs = (std::int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
        s.mtimeNs = (std::int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
        s.size = (std::int64_t)st.st_size;
        out->push_back(std::move(s));
        if (out->size() > FolderStamps::kMaxEntries) break;
    }
    closedir(d);
    return true;
}

void FolderStamps::remember(const std::string& dir, const Lister& list) {
    std::vector<StampEntry> now;
    if (!list(dir, &now) || now.size() > kMaxEntries) {
        seen_.erase(dir);
        return;
    }
    if (seen_.size() >= kMaxFolders && !seen_.count(dir)) seen_.clear();
    auto& m = seen_[dir];
    m.clear();
    for (StampEntry& e : now) m[e.name] = std::move(e);
}

void FolderStamps::look(const std::string& dir, const Lister& list, std::vector<Lsp::FileChange>* out) {
    std::vector<StampEntry> now;
    const bool there = list(dir, &now);
    auto was = seen_.find(dir);
    const std::string prefix = dir == "/" ? dir : dir + "/";
    auto report = [&](const std::string& name, Lsp::FileChangeType t) {
        const std::string path = prefix + name;
        if (!Lsp::watchExcluded(path)) out->push_back({path, t});
    };
    if (!there) {
        // The folder itself went: everything it held, then it.
        if (was != seen_.end()) {
            for (const auto& [name, e] : was->second) report(name, Lsp::FileChangeType::Deleted);
            seen_.erase(was);
        }
        return;
    }
    if (now.size() > kMaxEntries) {
        if (was != seen_.end()) seen_.erase(was);
        return;
    }
    std::map<std::string, StampEntry> next;
    for (StampEntry& e : now) next[e.name] = std::move(e);
    if (was == seen_.end()) {
        // Never looked at: what changed since the first server started.
        for (const auto& [name, e] : next)
            if (e.mtimeNs >= since_) report(name, Lsp::FileChangeType::Changed);
    } else {
        for (const auto& [name, e] : next) {
            auto old = was->second.find(name);
            if (old == was->second.end()) report(name, Lsp::FileChangeType::Created);
            else if (old->second.dir != e.dir) {
                report(name, Lsp::FileChangeType::Deleted);
                report(name, Lsp::FileChangeType::Created);
            } else if (!e.dir && (old->second.mtimeNs != e.mtimeNs || old->second.size != e.size))
                report(name, Lsp::FileChangeType::Changed);
        }
        for (const auto& [name, e] : was->second)
            if (!next.count(name)) report(name, Lsp::FileChangeType::Deleted);
    }
    if (seen_.size() >= kMaxFolders && was == seen_.end()) seen_.clear();
    seen_[dir] = std::move(next);
}

Json filesJson(const std::vector<Lsp::FileChange>& changes) {
    Json a = Json::array();
    for (const Lsp::FileChange& c : changes) {
        Json e = Json::array();
        e.push(Json(c.path));
        e.push(Json((std::int64_t)c.type));
        a.push(std::move(e));
    }
    Json j = Json::object();
    j.set("changes", std::move(a));
    return j;
}

std::vector<Lsp::FileChange> filesFrom(const Json& j) {
    std::vector<Lsp::FileChange> out;
    for (const Json& e : j["changes"].elements()) {
        if (!e.isArray() || e.size() != 2 || !e[0].isString() || !e[1].isNumber()) continue;
        const std::string path = e[0].asString();
        const int t = (int)e[1].asInt();
        if (path.empty() || path[0] != '/' || t < 1 || t > 3) continue;
        out.push_back({path, (Lsp::FileChangeType)t});
    }
    return out;
}

// ---------------------------------------------------------------- words

std::string serversTooOldText(const std::string& host) {
    return "Language servers need a newer mutible on " + host +
           ": install the current one there, then open the address again.";
}

std::string serverMissingText(const std::string& name, const std::string& host,
                              const std::vector<std::string>& tried) {
    std::string s = (name.empty() ? std::string("The language server") : name) + " is not installed on " + host;
    if (!tried.empty()) {
        s += " (looked for ";
        for (std::size_t i = 0; i < tried.size(); i++) {
            if (i) s += i + 1 == tried.size() ? (tried.size() > 2 ? ", and " : " and ") : ", ";
            s += tried[i];
        }
        s += ")";
    }
    return s + ".";
}

std::string serversLostText(const std::string& host) {
    return "The connection to " + host + " was lost; the language server starts again when it is back.";
}

std::string serverExitText(int status) {
    if (status < 0) return "ended";
    if (WIFSIGNALED(status)) return "killed by signal " + std::to_string(WTERMSIG(status));
    if (WIFEXITED(status)) return "exited with status " + std::to_string(WEXITSTATUS(status));
    return "ended";
}

}  // namespace Shared
