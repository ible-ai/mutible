// SessionTypeset.cpp — the LaTeX preview for a window on another machine
// (SessionTypeset.h): the sibling written beside the file, tectonic run on it,
// the outputs read into the answer, everything removed.
#include "SessionTypeset.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "Backups.h"
#include "LatexScratch.h"
#include "SessionFormat.h"
#include "SessionLsp.h"

namespace Shared {

namespace {

// ---------------------------------------------------------------- small things

std::string envValue(const std::vector<std::string>& env, const std::string& name) {
    const std::string prefix = name + "=";
    for (const std::string& e : env)
        if (e.compare(0, prefix.size(), prefix) == 0) return e.substr(prefix.size());
    return std::string();
}

// The host's own environment first, this process's second.
std::string lookup(const std::vector<std::string>& env, const char* name) {
    std::string v = envValue(env, name);
    if (v.empty())
        if (const char* g = std::getenv(name)) v = g;
    return v;
}

std::string homeFor(const std::vector<std::string>& env) {
    const std::string h = lookup(env, "HOME");
    if (!h.empty()) return h;
    struct passwd pw, *found = nullptr;
    char buf[4096];
    if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_dir) return found->pw_dir;
    return std::string();
}

std::string dirNameOf(const std::string& p) {
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

std::string baseNameOf(const std::string& p) {
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool isRunnable(const std::string& p) {
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

std::string readFile(const std::string& path, std::size_t most, bool* tooBig, bool* exists = nullptr) {
    if (tooBig) *tooBig = false;
    if (exists) *exists = false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();
    if (exists) *exists = true;
    std::string out;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out.size() + (std::size_t)n > most) {
            if (tooBig) *tooBig = true;
            out.clear();
            break;
        }
        out.append(buf, (std::size_t)n);
    }
    ::close(fd);
    return out;
}

void removeTree(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = ::opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent* e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") names.push_back(n);
            }
            ::closedir(d);
            for (const std::string& n : names) removeTree(path + "/" + n);
        }
        ::rmdir(path.c_str());
    } else {
        ::unlink(path.c_str());
    }
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        cur = path.substr(0, end);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ---------------------------------------------------------------- SHA-256

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    std::size_t used = 0;
    std::uint64_t total = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const unsigned char* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (std::uint32_t)p[i * 4] << 24 | (std::uint32_t)p[i * 4 + 1] << 16 | (std::uint32_t)p[i * 4 + 2] << 8 |
                   (std::uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const std::string& s) {
        for (unsigned char ch : s) {
            block[used++] = ch;
            total++;
            if (used == 64) {
                compress(block);
                used = 0;
            }
        }
    }
    std::string hex() {
        const std::uint64_t bits = total * 8;
        std::string pad(1, (char)0x80);
        while ((used + pad.size()) % 64 != 56) pad.push_back('\0');
        for (int i = 7; i >= 0; i--) pad.push_back((char)((bits >> (i * 8)) & 0xff));
        add(pad);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; i++)
            for (int s = 28; s >= 0; s -= 4) out.push_back(digits[(h[i] >> s) & 0xf]);
        return out;
    }
};

std::string sha256Hex(const std::string& s) {
    Sha256 h;
    h.add(s);
    return h.hex();
}

// Text that is valid UTF-8 (JSON carries no other): a stray byte is U+FFFD.
std::string validUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + (std::size_t)n <= s.size();
        for (int k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + (std::size_t)k] & 0xC0) == 0x80;
        if (ok && n == 2) ok = c >= 0xC2;
        if (ok && n == 3) {
            const unsigned char c1 = (unsigned char)s[i + 1];
            ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);
        }
        if (ok && n == 4) {
            const unsigned char c1 = (unsigned char)s[i + 1];
            ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90) && c <= 0xF4;
        }
        if (ok) {
            out.append(s, i, (std::size_t)n);
            i += (std::size_t)n;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
        }
    }
    return out;
}

std::string hostName() {
    char buf[256];
    if (::gethostname(buf, sizeof buf) != 0) return "this host";
    buf[sizeof buf - 1] = '\0';
    return buf[0] ? std::string(buf) : std::string("this host");
}

LatexScratch::Owner thisOwner(unsigned long long view) {
    static const long long start = (long long)Backups::processStart((int64_t)::getpid());
    LatexScratch::Owner o;
    o.pid = (long long)::getpid();
    o.start = start;
    o.view = view;
    return o;
}

std::string managedTectonic(const std::vector<std::string>& env) {
    const std::string home = homeFor(env);
#ifdef __APPLE__
    if (home.empty()) return std::string();
    return home + "/Library/Application Support/Mutible/bin/tectonic";
#else
    std::string data = lookup(env, "XDG_DATA_HOME");
    if (data.empty() || data[0] != '/') {
        if (home.empty()) return std::string();
        data = home + "/.local/share";
    }
    return data + "/mutible/bin/tectonic";
#endif
}

std::string minutesText(double seconds) {
    if (seconds >= 60 && std::fabs(seconds / 60 - std::round(seconds / 60)) < 1e-9) {
        const long m = std::lround(seconds / 60);
        return std::to_string(m) + (m == 1 ? " minute" : " minutes");
    }
    const long s = std::lround(seconds);
    return std::to_string(s) + (s == 1 ? " second" : " seconds");
}

}  // namespace

std::string typesetSha256Hex(const std::string& bytes) { return sha256Hex(bytes); }

// ---------------------------------------------------------------- finding tectonic

std::string findTectonic(const std::vector<std::string>& env, std::string* tried) {
    std::string note;
    const std::string forced = lookup(env, "MUTIBLE_TECTONIC");
    if (!forced.empty()) {
        if (isRunnable(forced)) return forced;
        note += "MUTIBLE_TECTONIC (" + forced + "), ";
    }
    const std::string managed = managedTectonic(env);
    if (!managed.empty()) {
        if (isRunnable(managed)) return managed;
        note += managed + ", ";
    }
    const std::string home = homeFor(env);
    const std::vector<std::string> dirs = serverSearchDirs(lookup(env, "PATH"), home, realServerFs());
    const std::string onPath = findServerProgram("tectonic", dirs, home, realServerFs());
    if (!onPath.empty()) return onPath;
    note += "PATH and the usual folders";
    if (tried) *tried = note;
    return std::string();
}

// ---------------------------------------------------------------- the folders

std::string typesetBaseFolder() {
    std::string tmp;
    if (const char* t = std::getenv("TMPDIR")) tmp = t;
    struct stat st;
    if (tmp.empty() || ::stat(tmp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) tmp = "/tmp";
    while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
    const std::string dir = tmp + "/mutible-host-latex-" + std::to_string((unsigned long)::getuid());
    ::mkdir(dir.c_str(), 0700);   // fails harmlessly when it is there
    if (::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0)
        return dir;
    return std::string();
}

void sweepTypesetFolders() {
    std::string base;
    {
        std::string tmp;
        if (const char* t = std::getenv("TMPDIR")) tmp = t;
        struct stat st;
        if (tmp.empty() || ::stat(tmp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) tmp = "/tmp";
        while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
        base = tmp + "/mutible-host-latex-" + std::to_string((unsigned long)::getuid());
    }
    DIR* d = ::opendir(base.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) names.push_back(e->d_name);
    ::closedir(d);
    for (const std::string& n : names) {
        LatexScratch::Owner o;
        if (!LatexScratch::parseFolderName(n, &o)) continue;
        if (!LatexScratch::ownerGone(o, Backups::processStart((int64_t)o.pid))) continue;
        const std::string dir = base + "/" + n;
        LatexScratch::Record r;
        if (LatexScratch::parse(readFile(dir + "/sibling.record", 1 << 20, nullptr), &r)) {
            const double start = Backups::processStart((int64_t)r.pid);
            const bool alive = start > 0 && std::fabs(start - r.start) <= 1.0;
            bool exists = false;
            const std::string now = readFile(r.sibling, std::size_t(256) << 20, nullptr, &exists);
            if (LatexScratch::decide(r, alive, exists, now.size(), sha256Hex(now)) == LatexScratch::Action::RemoveSibling &&
                LatexScratch::isPreviewCopyName(r.sibling))
                ::unlink(r.sibling.c_str());
        }
        removeTree(dir);
    }
}

// ---------------------------------------------------------------- the previews

std::shared_ptr<TypesetSlot> TypesetPreviews::begin(ClientId c, const std::string& view, DocId doc, std::uint64_t* seq) {
    std::shared_ptr<TypesetSlot>& s = slots_[{c, view}];
    if (!s) {
        s = std::make_shared<TypesetSlot>();
        s->view = ++views_;
    }
    s->doc = doc;
    const std::uint64_t mine = ++seq_;
    s->latest.store(mine);   // whatever ran for the key is no longer the newest
    if (seq) *seq = mine;
    return s;
}

void TypesetPreviews::stop(ClientId c, const std::string& view) {
    auto it = slots_.find({c, view});
    if (it == slots_.end()) return;
    it->second->closed.store(true);
    it->second->latest.store(++seq_);
    slots_.erase(it);
}

void TypesetPreviews::stopDoc(ClientId c, DocId doc) {
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->first.first == c && it->second->doc == doc) {
            it->second->closed.store(true);
            it->second->latest.store(++seq_);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void TypesetPreviews::stopAll(ClientId c) {
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->first.first == c) {
            it->second->closed.store(true);
            it->second->latest.store(++seq_);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- a run

TypesetAnswer typesetRefused(const std::string& why, const std::string& reason) {
    TypesetAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("typeset"));
    a.json.set("error", Json(why));
    a.json.set("reason", Json(reason));
    return a;
}

TypesetAnswer runTypeset(const TypesetJob& job, TypesetSlot& slot, std::uint64_t seq,
                         const std::vector<std::string>& env, const std::atomic<bool>& cancel) {
    TypesetAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("typeset"));
    a.json.set("view", Json(job.view));
    a.json.set("gen", Json((int64_t)job.gen));
    a.json.set("ok", Json(false));
    auto say = [&](const char* flag, const std::string& error) {
        if (flag) a.json.set(flag, Json(true));
        a.json.set("error", Json(error));
    };
    auto notNewest = [&] { return cancel.load() || slot.latest.load() != seq; };
    auto ended = [&] {
        if (cancel.load() || slot.closed.load()) say("stopped", "The preview was closed.");
        else say("superseded", "A newer typeset replaced this one.");
    };

    // One run at a time for the preview: the one before it ends first (it is
    // not the newest, so it is stopping), and may have taken the sibling's
    // name, which is the same for the preview's every run.
    std::unique_lock<std::mutex> lock(slot.run);
    if (notNewest()) {
        ended();
        return a;
    }

    std::string tried;
    const std::string tool = findTectonic(env, &tried);
    if (tool.empty()) {
        say("missing", "tectonic is not installed on " + hostName() + ". Looked for it in " + tried + ".");
        return a;
    }
    if (job.path.empty() || job.path[0] != '/') {
        say(nullptr, "The document has no file on the host to typeset beside.");
        return a;
    }
    const std::string base = typesetBaseFolder();
    if (base.empty()) {
        say(nullptr, "The host has no private folder for the typeset's files (TMPDIR).");
        return a;
    }

    const LatexScratch::Owner owner = thisOwner(slot.view);
    const std::string outDir = base + "/" + LatexScratch::folderName(sha256Hex(job.path).substr(0, 16), owner);
    const std::string folder = dirNameOf(job.path);
    const std::string siblingFile = LatexScratch::siblingName(baseNameOf(job.path), owner);
    const std::string sibling = (folder == "/" ? std::string() : folder) + "/" + siblingFile;
    std::string stem = siblingFile;
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".tex") == 0) stem.resize(stem.size() - 4);

    // Whatever happens next, the sibling and the work folder go.
    struct Cleanup {
        std::string outDir, sibling, siblingFile;
        int dirFd = -1;
        bool wrote = false;
        ~Cleanup() {
            if (wrote) {
                // Through the folder's descriptor, which names it still after a rename.
                if (dirFd < 0 || ::unlinkat(dirFd, siblingFile.c_str(), 0) != 0) ::unlink(sibling.c_str());
            }
            if (dirFd >= 0) ::close(dirFd);
            removeTree(outDir);
        }
    } cleanup{outDir, sibling, siblingFile};

    if (!makeDirs(outDir)) {
        say(nullptr, "The host could not make a folder for the typeset's files (" + outDir + ").");
        return a;
    }
    // The record first, so a crash after leaves the means to remove the sibling.
    LatexScratch::Record rec;
    rec.sibling = sibling;
    rec.pid = owner.pid;
    rec.start = Backups::processStart((int64_t)owner.pid);
    rec.size = job.text.size();
    rec.sha256 = sha256Hex(job.text);
    const std::string recText = LatexScratch::format(rec);
    {
        const int fd = ::open((outDir + "/sibling.record").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            const ssize_t ignore = ::write(fd, recText.data(), recText.size());
            (void)ignore;
            ::close(fd);
        }
    }
    cleanup.dirFd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ::unlink(sibling.c_str());   // a leftover of this very name is ours
    {
        const int fd = ::open(sibling.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        std::size_t done = 0;
        bool ok = fd >= 0;
        const int why = errno;
        cleanup.wrote = ok;
        while (ok && done < job.text.size()) {
            const ssize_t n = ::write(fd, job.text.data() + done, job.text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) ok = false;
            else done += (std::size_t)n;
        }
        if (fd >= 0) ::close(fd);
        if (!ok) {
            say(nullptr, std::string("Cannot write the preview copy next to the file on the host: ") +
                             std::strerror(fd >= 0 ? errno : why));
            return a;
        }
    }
    a.json.set("scratch", Json(sibling));
    a.json.set("stem", Json(stem));

    // The apps' arguments (linux/src/Latex.cpp, mac/src/Latex.mm), in the file's folder.
    const std::vector<std::string> argv = {tool,         "--untrusted", "--synctex", "--keep-logs", "--chatter", "minimal",
                                           "--color",    "never",       "--outdir",  outDir,        sibling};
    const std::function<bool()> stopWhen = [&] { return slot.latest.load() != seq; };
    const double seconds = std::min(std::max(job.timeout, 0.1), kTypesetSeconds);
    const ProgramRun r = runProgram(argv, env, folder, std::string(), (int)(seconds * 1000), &cancel, &stopWhen);

    std::string log = r.out;
    if (!r.err.empty()) log += r.err;
    if (log.size() > kMaxTypesetLog) log.erase(0, log.size() - kMaxTypesetLog);
    log = validUtf8(log);
    a.json.set("log", Json(log));
    a.json.set("status", Json((int64_t)r.status));

    if (!r.launchError.empty()) {
        say(nullptr, "tectonic did not run on " + hostName() + ": " + r.launchError);
        return a;
    }
    if (r.cancelled) {
        ended();
        return a;
    }
    if (r.timedOut) {
        say("stopped", "tectonic ran longer than " + minutesText(seconds) + " and was stopped.");
        return a;
    }
    // TeX's transcript is read whatever came of the run (--keep-logs).
    bool big = false;
    const std::string transcript = readFile(outDir + "/" + stem + ".log", kMaxTypesetTranscript, &big);
    if (r.status != 0) {
        say(nullptr, log.empty() ? "tectonic did not run." : "The document did not typeset.");
        a.bytes = transcript;
        a.json.set("pdf", Json((int64_t)0));
        a.json.set("synctex", Json((int64_t)0));
        a.json.set("transcript", Json((int64_t)transcript.size()));
        return a;
    }
    bool exists = false;
    const std::string pdf = readFile(outDir + "/" + stem + ".pdf", kMaxTypesetPdf, &big, &exists);
    if (big) {
        say(nullptr, "The PDF is past the " + std::to_string(kMaxTypesetPdf >> 20) +
                         " MB the host sends, so it is not shown.");
        return a;
    }
    if (!exists || pdf.empty()) {
        say(nullptr, "tectonic finished but wrote no PDF.");
        return a;
    }
    bool synctexBig = false;
    const std::string synctex = readFile(outDir + "/" + stem + ".synctex.gz", kMaxTypesetSynctex, &synctexBig);
    a.bytes = pdf + synctex + transcript;
    a.json.set("pdf", Json((int64_t)pdf.size()));
    a.json.set("synctex", Json((int64_t)synctex.size()));
    a.json.set("transcript", Json((int64_t)transcript.size()));
    a.json.set("ok", Json(true));
    return a;
}

// ---------------------------------------------------------------- the window's side

TypesetReply typesetReplyFrom(const Json& answer, std::string&& bytes) {
    TypesetReply r;
    r.view = answer["view"].asString();
    r.gen = (std::uint64_t)std::max<int64_t>(0, answer["gen"].asInt());
    r.ok = answer["ok"].asBool();
    r.missing = answer["missing"].asBool();
    r.superseded = answer["superseded"].asBool();
    r.stopped = answer["stopped"].asBool();
    r.error = answer["error"].asString();
    r.log = answer["log"].asString();
    r.scratch = answer["scratch"].asString();
    r.stem = answer["stem"].asString();
    r.status = (int)answer["status"].asInt(-1);
    const std::size_t pdf = (std::size_t)std::max<int64_t>(0, answer["pdf"].asInt());
    const std::size_t sync = (std::size_t)std::max<int64_t>(0, answer["synctex"].asInt());
    const std::size_t tr = (std::size_t)std::max<int64_t>(0, answer["transcript"].asInt());
    if (pdf + sync + tr == bytes.size()) {
        r.pdf = bytes.substr(0, pdf);
        r.synctex = bytes.substr(pdf, sync);
        r.transcript = bytes.substr(pdf + sync, tr);
    } else if (r.ok) {
        r.ok = false;
        r.error = "The host's answer was damaged.";
    }
    if (r.ok && r.pdf.empty()) {
        r.ok = false;
        r.error = "The host sent no PDF.";
    }
    return r;
}

namespace {
bool writeWhole(const std::string& path, const std::string& bytes, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    const bool ok = f && (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    const bool closed = !f || std::fclose(f) == 0;
    if (ok && closed) return true;
    if (error) *error = "Could not write " + path + ": " + std::strerror(errno);
    return false;
}
}  // namespace

bool writeTypesetFiles(const TypesetReply& r, const std::string& folder, bool withTranscript, TypesetFiles* out,
                       std::string* error) {
    if (r.stem.empty() || r.stem.find('/') != std::string::npos) {
        if (error) *error = "The host named its output oddly.";
        return false;
    }
    if (!makeDirs(folder)) {
        if (error) *error = "Could not make " + folder + ".";
        return false;
    }
    TypesetFiles f;
    if (!r.pdf.empty()) {
        f.pdf = folder + "/" + r.stem + ".pdf";
        if (!writeWhole(f.pdf, r.pdf, error)) return false;
    }
    if (!r.synctex.empty()) {
        f.synctex = folder + "/" + r.stem + ".synctex.gz";
        if (!writeWhole(f.synctex, r.synctex, error)) return false;
    }
    if (withTranscript && !r.transcript.empty()) {
        f.transcript = folder + "/" + r.stem + ".log";
        if (!writeWhole(f.transcript, r.transcript, error)) return false;
    }
    if (out) *out = std::move(f);
    return true;
}

std::string typesetTooOldText(const std::string& host) {
    return "The LaTeX preview needs a newer mutible on " + host +
           ": the one there cannot typeset for this window. Upgrade it there, then Reconnect.";
}

std::string typesetLostText(const std::string& host) {
    return "The connection to " + host + " is lost: Reconnect, then typeset again.";
}

}  // namespace Shared
