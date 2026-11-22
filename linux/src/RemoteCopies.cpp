// RemoteCopies.cpp — see RemoteCopies.h.
#include "RemoteCopies.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>

#include <cerrno>

#include "RemoteCache.h"

namespace {
std::uint64_t windowSerial = 0;

// A folder made 0700 (or found, ours, not a link, and private).
bool privateFolder(const std::string& path) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           (st.st_mode & 077) == 0;
}

// The folders of `rel` under `base`, each made 0700.
bool makeFolders(const std::string& base, const std::string& rel) {
    std::string at = base;
    size_t pos = 0;
    while (pos < rel.size()) {
        size_t slash = rel.find('/', pos);
        if (slash == std::string::npos) slash = rel.size();
        if (slash > pos) {
            at += "/" + rel.substr(pos, slash - pos);
            if (mkdir(at.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        pos = slash + 1;
    }
    return true;
}

void removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (GDir* d = g_dir_open(path.c_str(), 0, nullptr)) {
            std::vector<std::string> names;
            while (const char* n = g_dir_read_name(d)) names.push_back(n);
            g_dir_close(d);
            for (const std::string& n : names) removeTree(path + "/" + n);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}
}  // namespace

RemoteCopies::RemoteCopies(std::string remoteFolder) : remote_(std::move(remoteFolder)) {}

RemoteCopies::~RemoteCopies() { removeAll(); }

std::string RemoteCopies::root() {
    const char* run = g_get_user_runtime_dir();
    const std::string base = run && *run && g_file_test(run, G_FILE_TEST_IS_DIR) ? run : g_get_tmp_dir();
    return base + "/mutible-ssh-copies-" + std::to_string((long)getuid());
}

std::string RemoteCopies::localPathOf(const std::string& remotePath) const {
    const auto it = copies_.find(remotePath);
    return it == copies_.end() ? std::string() : it->second.local;
}

std::string RemoteCopies::store(const std::string& bytes, const std::string& remotePath) {
    const std::string place = RemoteCache::placeOf(remote_, remotePath);
    if (place.empty()) return "";
    if (folder_.empty()) {
        const std::string top = root();
        if (!privateFolder(top)) return "";
        const std::string mine = top + "/" + RemoteCache::windowFolderName((long)getpid(), ++windowSerial);
        if (mkdir(mine.c_str(), 0700) != 0) return "";
        folder_ = mine;
    }
    const std::string local = folder_ + "/" + place;
    const size_t cut = place.rfind('/');
    if (cut != std::string::npos && !makeFolders(folder_, place.substr(0, cut))) return "";
    // Through a temporary file of the same folder, renamed over.
    std::string tmp = local + ".XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back('\0');
    const int fd = g_mkstemp_full(name.data(), O_RDWR, 0600);
    if (fd < 0) return "";
    size_t at = 0;
    bool ok = true;
    while (at < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        at += (size_t)n;
    }
    ok = close(fd) == 0 && ok;
    if (!ok || rename(name.data(), local.c_str()) != 0) {
        unlink(name.data());
        return "";
    }
    copies_[remotePath] = Copy{local, RemoteCache::fingerprint(bytes)};
    stale_.erase(remotePath);
    failed.erase(remotePath);
    return local;
}

bool RemoteCopies::localCopyHolds(const std::string& remotePath, const std::string& bytes) const {
    const auto it = copies_.find(remotePath);
    return it != copies_.end() && it->second.print == RemoteCache::fingerprint(bytes);
}

std::vector<std::string> RemoteCopies::foldersChanged(const std::vector<std::string>& folders) {
    std::vector<std::string> out;
    for (const auto& c : copies_)
        if (RemoteCache::mayHaveChanged(folders, c.first)) {
            stale_.insert(c.first);
            out.push_back(c.first);
            if (fetching.count(c.first)) again.insert(c.first);
        }
    // A picture that was not there is asked again once its folder changes.
    for (auto it = failed.begin(); it != failed.end();) {
        if (RemoteCache::mayHaveChanged(folders, *it)) it = failed.erase(it);
        else ++it;
    }
    return out;
}

std::vector<std::string> RemoteCopies::remoteFolders() const {
    std::set<std::string> out;
    for (const auto& c : copies_) {
        const size_t cut = c.first.rfind('/');
        if (cut != std::string::npos && cut > 0) out.insert(c.first.substr(0, cut));
    }
    return std::vector<std::string>(out.begin(), out.end());
}

void RemoteCopies::removeAll() {
    if (!folder_.empty()) removeTree(folder_);
    folder_.clear();
    copies_.clear();
    stale_.clear();
}

void RemoteCopies::removeLeftovers() {
    const std::string top = root();
    GDir* d = g_dir_open(top.c_str(), 0, nullptr);
    if (!d) return;
    std::vector<std::string> names;
    while (const char* n = g_dir_read_name(d)) names.push_back(n);
    g_dir_close(d);
    for (const std::string& n : names) {
        long pid = 0;
        if (!RemoteCache::ownerOfFolder(n, &pid)) continue;
        if (kill((pid_t)pid, 0) == 0 || errno == EPERM) continue;   // running
        removeTree(top + "/" + n);
    }
}
