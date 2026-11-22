// RemoteCopies.h — a window over SSH's private folder of copies (the Mac's
// MCRemoteFileCache; docs/notes/session-host.md, "Viewers over SSH"): the
// files it shows in a viewer that reads a file itself (a picture, a PDF, a
// player, a database) and the pictures its Markdown pages show, read
// through its host and written here, so the viewer opens the copy while the
// tab keeps the remote path. The rules (names, places, the limit, which
// folders' changes matter, the fingerprint) are the core's RemoteCache.h.
//
// The folder is <the user's runtime folder, else the temporary folder>/
// mutible-ssh-copies-<uid>/w<pid>-<n>, made 0700 on the first copy (the
// folder of folders too), each copy 0600, written through a temporary file
// renamed over, so a viewer never reads half a file. It is removed when the
// window closes (removeAll, also the destructor) and, for a process that
// ended without closing it (a crash), at the next launch (removeLeftovers).
// Main thread, but removeLeftovers.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class RemoteCopies {
public:
    // `remoteFolder`: the window's folder on the host, as its tabs spell
    // their paths.
    explicit RemoteCopies(std::string remoteFolder);
    ~RemoteCopies();
    RemoteCopies(const RemoteCopies&) = delete;
    RemoteCopies& operator=(const RemoteCopies&) = delete;

    const std::string& remoteFolder() const { return remote_; }
    // This window's folder of copies ("" until the first copy).
    const std::string& folder() const { return folder_; }

    // Where the copy of `remotePath` is; "" when it was not copied.
    std::string localPathOf(const std::string& remotePath) const;
    bool hasCopyOf(const std::string& remotePath) const { return copies_.count(remotePath) != 0; }
    // Whether the host's disk watching said the file's folder changed since
    // the copy was made.
    bool isStale(const std::string& remotePath) const { return stale_.count(remotePath) != 0; }
    // The copy written (folders 0700, the file 0600); "" when it could not be
    // (the path is not inside the window's folder, or the disk said no).
    std::string store(const std::string& bytes, const std::string& remotePath);
    // Whether `bytes` are what the copy holds (its fingerprint).
    bool localCopyHolds(const std::string& remotePath, const std::string& bytes) const;
    // The host saw these folders (absolute) change: the copies in them are
    // stale; returns the remote paths of those copies.
    std::vector<std::string> foldersChanged(const std::vector<std::string>& folders);
    void markFresh(const std::string& remotePath) { stale_.erase(remotePath); }
    void markStale(const std::string& remotePath) {
        if (copies_.count(remotePath)) stale_.insert(remotePath);
    }
    // The folders of the files copied (absolute remote), for the host to watch.
    std::vector<std::string> remoteFolders() const;
    // Every copy removed, and the folder.
    void removeAll();

    // The remote paths being read now (one read at a time for a path), those
    // whose read failed (asked again once their folder changes), and those
    // told changed while a read was in flight (read again once it ends).
    std::set<std::string> fetching, failed, again;
    // How many copies were refreshed because their file changed there.
    unsigned refreshes = 0;

    // The folder of every window's folder of copies.
    static std::string root();
    // At launch: the folders of processes that are not running any more.
    static void removeLeftovers();

private:
    struct Copy {
        std::string local;
        std::uint64_t print = 0;
    };
    std::string remote_, folder_;
    std::map<std::string, Copy> copies_;
    std::set<std::string> stale_;
};
