// RemoteCache.cpp — see RemoteCache.h.
#include "RemoteCache.h"

#include "FileDetails.h"
#include "FileSniff.h"

#include <algorithm>
#include <cctype>

namespace RemoteCache {

std::string windowFolderName(long pid, std::uint64_t serial) {
    return "w" + std::to_string(pid) + "-" + std::to_string(serial);
}

bool ownerOfFolder(const std::string& name, long* pid) {
    if (name.size() < 4 || name[0] != 'w') return false;
    const std::size_t dash = name.find('-', 1);
    if (dash == std::string::npos || dash == 1 || dash + 1 >= name.size() || dash > 12) return false;
    long p = 0;
    for (std::size_t i = 1; i < dash; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        p = p * 10 + (name[i] - '0');
    }
    for (std::size_t i = dash + 1; i < name.size(); i++)
        if (name[i] < '0' || name[i] > '9') return false;
    if (p <= 0) return false;
    if (pid) *pid = p;
    return true;
}

namespace {

// Each '/'-separated name of `rel` is one: not empty, ".", ".." or with a NUL.
bool cleanNames(const std::string& rel) {
    if (rel.empty() || rel.front() == '/') return false;
    std::size_t at = 0;
    for (;;) {
        const std::size_t slash = rel.find('/', at);
        const std::string name = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (name.empty() || name == "." || name == ".." || name.find('\0') != std::string::npos) return false;
        if (slash == std::string::npos) return true;
        at = slash + 1;
    }
}

std::string withoutTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string parentOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "." and ".." taken out of an absolute path by name; "" when ".." would
// climb past "/".
std::string normalized(const std::string& path) {
    std::vector<std::string> names;
    std::size_t at = 1;
    while (at <= path.size()) {
        std::size_t slash = path.find('/', at);
        if (slash == std::string::npos) slash = path.size();
        const std::string name = path.substr(at, slash - at);
        if (name == "..") {
            if (names.empty()) return "";
            names.pop_back();
        } else if (!name.empty() && name != ".") {
            names.push_back(name);
        }
        at = slash + 1;
    }
    std::string out;
    for (const std::string& n : names) out += "/" + n;
    return out.empty() ? "/" : out;
}

}  // namespace

std::string placeOf(const std::string& root, const std::string& path) {
    const std::string r = withoutTrailingSlash(root);
    if (r.empty() || r.front() != '/' || path.size() <= r.size()) return "";
    const std::string prefix = r == "/" ? "/" : r + "/";
    if (path.compare(0, prefix.size(), prefix) != 0) return "";
    const std::string rel = path.substr(prefix.size());
    return cleanNames(rel) ? rel : "";
}

Show showFor(const std::string& path, bool tooLarge, const std::string& bytes, bool decodesAsText,
             std::uint64_t tableEditableBytes) {
    if (tooLarge) return Show::TooLarge;
    const std::string ext = FileSniff::extensionOf(path);
    if (FileIcon::isImageExtension(ext)) return Show::Image;
    if (ext == "pdf") return Show::Pdf;
    if (FileIcon::isVideoExtension(ext) || FileIcon::isAudioExtension(ext)) return Show::Media;
    const std::size_t headLen = std::min(bytes.size(), FileSniff::kHeadBytes);
    switch (FileSniff::classify(path, bytes.data(), headLen)) {
    case FileSniff::Kind::Sqlite:
    case FileSniff::Kind::QuickLook: return Show::Viewer;
    case FileSniff::Kind::Csv:
    case FileSniff::Kind::Tsv:
        // The large table reads the file's bytes as UTF-8 itself; a UTF-16
        // one that size opens as text (FileViewers.mm's rule).
        if (bytes.size() > tableEditableBytes && !FileSniff::isUtf16Text(bytes.data(), headLen))
            return Show::Viewer;
        break;
    default: break;
    }
    return decodesAsText ? Show::Text : Show::Viewer;
}

const char* showName(Show s) {
    switch (s) {
    case Show::Text: return "text";
    case Show::Image: return "image";
    case Show::Pdf: return "pdf";
    case Show::Media: return "media";
    case Show::Viewer: return "viewer";
    case Show::TooLarge: return "too-large";
    }
    return "?";
}

std::string tooLargeMessage(const std::string& name, const std::string& place) {
    return "“" + name + "” is too large to open over SSH.\n\nIt is over " +
           std::to_string(kMaxBytes >> 20) + " MB, and a file that size is not copied from " + place +
           " to this Mac. The terminal runs there.";
}

bool mayHaveChanged(const std::vector<std::string>& changedFolders, const std::string& path) {
    const std::string folder = parentOf(path);
    if (folder.empty()) return false;
    for (const std::string& f : changedFolders)
        if (withoutTrailingSlash(f) == folder) return true;
    return false;
}

std::uint64_t fingerprint(const std::string& bytes) {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string markdownPicture(const std::string& src, const std::string& docFolder, const std::string& root) {
    if (src.empty() || src.front() == '~' || src.find("://") != std::string::npos) return "";
    std::string lower = src.substr(0, 5);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower == "data:") return "";
    // The query or fragment dropped, then %XX decoded (a "%20" in a name).
    std::string cut = src.substr(0, src.find_first_of("?#"));
    std::string path;
    for (std::size_t i = 0; i < cut.size(); i++) {
        if (cut[i] == '%' && i + 2 < cut.size() && hexValue(cut[i + 1]) >= 0 && hexValue(cut[i + 2]) >= 0) {
            path += (char)(hexValue(cut[i + 1]) * 16 + hexValue(cut[i + 2]));
            i += 2;
        } else {
            path += cut[i];
        }
    }
    if (path.empty() || path.find('\0') != std::string::npos) return "";
    if (path.front() != '/') {
        if (docFolder.empty() || docFolder.front() != '/') return "";
        path = withoutTrailingSlash(docFolder) + "/" + path;
    }
    path = normalized(path);
    if (path.empty()) return "";
    return placeOf(root, path).empty() ? "" : path;
}

}  // namespace RemoteCache
