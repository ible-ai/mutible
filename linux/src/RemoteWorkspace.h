// RemoteWorkspace.h — a window over SSH's folder, read through the remote
// host (stage 8, second slice; the Mac's MCRemoteWorkspace; docs/notes/
// session-host.md, "The GTK port over SSH"), never through this machine's
// disk:
//   - the tree's listings (FsRead "list", asked without waiting), with the
//     host's marks (what the ignore files ignore, a link out of the folder);
//   - the folders the tree has listed, told to the host (FsRead "watch",
//     once a turn of the loop, again after Reconnect), whose changes come
//     back as notices (FsChanged) naming the folders to list again: the
//     remote disk's notices in place of GFileMonitor;
//   - Quick Open's list of the folder's files ("files") and Find in Folder
//     ("search"), both on the host's workers, answered as they go
//     (SessionSearch.h).
// Its link is the window's, moved by Reconnect (attachLink). Main thread
// only; every callback is called on it, from a dispatch of the link's.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Json.h"

namespace SessionLink {
class Link;
}

class RemoteWorkspace : public std::enable_shared_from_this<RemoteWorkspace> {
public:
    // The folder as the host resolved it (its Welcome's), the root every
    // path of this window is spelled under.
    explicit RemoteWorkspace(std::string folder);
    ~RemoteWorkspace();
    RemoteWorkspace(const RemoteWorkspace&) = delete;
    RemoteWorkspace& operator=(const RemoteWorkspace&) = delete;

    const std::string& folder() const { return folder_; }
    // The window's link, now or again after Reconnect: its disk notices
    // heard, and the folders the tree has listed told to the new host.
    void attachLink(const std::shared_ptr<SessionLink::Link>& link);
    // The window closes: nothing more is heard or asked.
    void detach();
    bool live() const;

    // `path` (absolute, under folder()) as the host spells it, '/'-separated
    // from the folder ("" for the folder itself).
    bool relative(const std::string& path, std::string* rel) const;
    std::string absolute(const std::string& rel) const;

    struct Entry {
        std::string name;
        bool dir = false, link = false, ignored = false, outside = false;
    };
    // The folder's entries (dotfiles too: the tree's filter hides them),
    // unsorted. `done(ok, entries)` once, never inside this call; ok false
    // when the host could not be asked or was lost (the rows then stay as
    // they were), an empty list for a folder that is gone. False, calling
    // nothing, when there is no live link.
    bool listFolder(const std::string& path, std::function<void(bool ok, std::vector<Entry>&&)> done);

    // The folders listed in the tree now (absolute): the host watches them.
    // Sent when it differs from what was sent last, once per turn of the main
    // loop, and again to a new host.
    void watchFolders(std::vector<std::string> paths);
    // Each change the host saw: the folders (absolute) whose listing it
    // sent again, on a turn of the main loop.
    std::function<void(const std::vector<std::string>& folders)> onChanged;

    // Quick Open's list: the folder's files ('/'-separated from it).
    // `done` once (empty when the host cannot be asked).
    void listFiles(std::function<void(std::vector<std::string> files, bool truncated)> done);
    // Find in Folder (a "search" request): each batch as it comes, the last
    // with done true. 0, calling nothing, when the host cannot be asked.
    std::uint64_t search(Json request, std::function<void(std::vector<Json>&& answers, bool done)> answers);
    void cancelSearch(std::uint64_t req);

    // For checks: folders listed through the host, its notices taken, Quick
    // Open lists and searches asked.
    std::size_t listings = 0, notices = 0, fileLists = 0, searches = 0;

private:
    void sendWatch();
    void tookNotice(const Json& changed);

    std::string folder_;
    std::shared_ptr<SessionLink::Link> link_;
    int diskToken_ = 0;
    std::vector<std::string> watchPaths_;   // relative, sorted
    bool watchAsked_ = false;
    std::string watchSent_;
    unsigned watchIdle_ = 0;
};
