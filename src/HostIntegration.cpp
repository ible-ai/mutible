// HostIntegration.cpp — see HostIntegration.h.
#include "HostIntegration.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "Backups.h"
#include "ShellIntegration.h"

namespace Shared {

namespace {

bool isFile(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDir(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string baseName(const std::string& p) {
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool hasName(const std::string& e, const std::string& name) {
    return e.size() > name.size() && e.compare(0, name.size(), name) == 0 && e[name.size()] == '=';
}

// `env` without every entry named one of `names`; the value of the first
// named `keep` in `*kept` (when there was one, `*had`).
void dropNames(std::vector<std::string>& env, const std::vector<std::string>& names, const std::string& keep,
               std::string* kept, bool* had) {
    std::vector<std::string> out;
    for (const std::string& e : env) {
        bool drop = false;
        for (const std::string& n : names) drop = drop || hasName(e, n);
        if (drop && !keep.empty() && hasName(e, keep) && had && !*had) {
            *had = true;
            *kept = e.substr(keep.size() + 1);
        }
        if (!drop) out.push_back(e);
    }
    env.swap(out);
}

bool readAll(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool writeAll(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return (bool)out.flush();
}

const char* const kFolderPrefix = "Mutible-host-";
const char* const kFiles[] = {ShellIntegration::kZshScriptName, ".zshenv", ShellIntegration::kBashScriptName};

void removeFolder(const std::string& dir) {
    for (const char* f : kFiles) unlink((dir + "/" + f).c_str());
    rmdir(dir.c_str());
}

// "Mutible-host-<pid>-<start>": the pid and start, false for another name.
bool parseName(const std::string& name, long long* pid, long long* start) {
    const std::string p = kFolderPrefix;
    if (name.compare(0, p.size(), p) != 0) return false;
    const std::string rest = name.substr(p.size());
    const std::size_t dash = rest.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= rest.size() || rest.size() > 40) return false;
    for (std::size_t i = 0; i < rest.size(); i++)
        if (i != dash && (rest[i] < '0' || rest[i] > '9')) return false;
    *pid = std::strtoll(rest.c_str(), nullptr, 10);
    *start = std::strtoll(rest.c_str() + dash + 1, nullptr, 10);
    return *pid > 0;
}

}  // namespace

std::string ownProgramPath() {
    std::string exe;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size + 1, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) == 0) exe = buf.c_str();
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) exe.assign(buf, (std::size_t)n);
#endif
    if (exe.empty()) return "";
    char real[PATH_MAX];
    return realpath(exe.c_str(), real) ? std::string(real) : exe;
}

std::string shippedFile(const std::string& exe, const std::string& name) {
    if (exe.empty() || name.empty()) return "";
    char real[PATH_MAX];
    const std::string path = realpath(exe.c_str(), real) ? std::string(real) : exe;
    const std::size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    for (const std::string& p : {dir + "/" + name, dir + "/../Resources/" + name, dir + "/../share/mutible/" + name})
        if (isFile(p)) return p;
    return "";
}

std::string integrateShell(PtySpec& p, const std::string& folder, bool zsh, bool bash,
                           const std::vector<std::string>& accepted, const std::string& nonce) {
    if (folder.empty() || nonce.empty()) return "";
    const std::string name = baseName(p.program);
    bool wanted = false;
    for (const std::string& a : accepted) wanted = wanted || a == name;
    if (!wanted) return "";
    if (name == "zsh" && zsh) {
        // The user's ZDOTDIR handed back by the stub; one a mutible terminal
        // this host was started in left is not theirs.
        std::string theirs;
        bool had = false;
        dropNames(p.env, {"ZDOTDIR", "MUTIBLE_USER_ZDOTDIR", "MUTIBLE_NONCE", "MUTIBLE_BASH_LOGIN"}, "ZDOTDIR",
                  &theirs, &had);
        if (had) p.env.push_back("MUTIBLE_USER_ZDOTDIR=" + theirs);
        p.env.push_back("ZDOTDIR=" + folder);
        p.env.push_back("MUTIBLE_NONCE=" + nonce);
        return "zsh";
    }
    if (name == "bash" && bash) {
        const bool login = !p.argv.empty() && !p.argv[0].empty() && p.argv[0][0] == '-';
        dropNames(p.env, {"MUTIBLE_NONCE", "MUTIBLE_BASH_LOGIN"}, "", nullptr, nullptr);
        p.argv = {"bash", "--rcfile", folder + "/" + ShellIntegration::kBashScriptName};
        p.env.push_back("MUTIBLE_NONCE=" + nonce);
        if (login) p.env.push_back("MUTIBLE_BASH_LOGIN=1");
        return "bash";
    }
    return "";
}

HostIntegration::~HostIntegration() {
    if (!folder_.empty()) removeFolder(folder_);
}

void HostIntegration::setScripts(std::string zsh, std::string bash) {
    zshScript_ = std::move(zsh);
    bashScript_ = std::move(bash);
}

std::string HostIntegration::apply(PtySpec& p, const std::vector<std::string>& accepted,
                                   const std::string& nonce) {
    if (accepted.empty() || nonce.empty() || !any() || !prepare()) return "";
    return integrateShell(p, folder_, zsh_, bash_, accepted, nonce);
}

bool HostIntegration::prepare() {
    // A lasting host runs for days, and the system may clear its temporary
    // folder meanwhile: copies gone are made again.
    if (!folder_.empty() && (zsh_ ? isFile(folder_ + "/.zshenv") && isFile(folder_ + "/" + ShellIntegration::kZshScriptName)
                                  : isFile(folder_ + "/" + ShellIntegration::kBashScriptName)))
        return true;
    if (tried_ && folder_.empty()) return false;
    tried_ = true;
    folder_.clear();
    zsh_ = bash_ = false;
    std::string base = base_;
    // Not XDG_RUNTIME_DIR, which systemd removes when the user's last
    // session ends: on a machine reached only by ssh, the dropped
    // connection, while a lasting host runs on (session-host.md's traps).
    if (base.empty()) {
        const char* v = getenv("TMPDIR");
        if (v && *v && isDir(v)) base = v;
    }
    if (base.empty()) base = "/tmp";
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    // Those of hosts gone first.
    if (DIR* d = opendir(base.c_str())) {
        while (struct dirent* e = readdir(d)) {
            long long pid = 0, start = 0;
            if (!parseName(e->d_name, &pid, &start) || pid == (long long)getpid()) continue;
            if (ShellIntegration::ownerGone(start, Backups::processStart(pid)))
                removeFolder(base + "/" + e->d_name);
        }
        closedir(d);
    }
    const long long start = (long long)Backups::processStart(getpid());
    const std::string dir = base + "/" + kFolderPrefix + std::to_string((long long)getpid()) + "-" +
                            std::to_string(start);
    mkdir(dir.c_str(), 0700);
    // The user's own, and only theirs: never a folder someone else made
    // first in a shared /tmp.
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0)
        return false;
    std::string text;
    if (!zshScript_.empty() && readAll(zshScript_, &text) &&
        writeAll(dir + "/" + ShellIntegration::kZshScriptName, text))
        zsh_ = writeAll(dir + "/.zshenv", ShellIntegration::zshenvStub(true));
    if (!bashScript_.empty() && readAll(bashScript_, &text))
        bash_ = writeAll(dir + "/" + ShellIntegration::kBashScriptName, text);
    if (!zsh_ && !bash_) {
        removeFolder(dir);
        return false;
    }
    folder_ = dir;
    return true;
}

}  // namespace Shared
