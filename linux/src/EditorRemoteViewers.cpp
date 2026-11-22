// EditorRemoteViewers.cpp — Editor: a window over SSH showing its files in
// the viewers (docs/notes/session-host.md, "Viewers over SSH"; the rules in
// the core's RemoteCache.h, the Mac's EditorController+RemoteViewers.mm).
//
// A file of the window is read through its host without waiting
// (Document::beginLoading, Link::readTextAsync); before its bytes are read
// as text, documentBytesRead asks whether a viewer that reads a file itself
// shows it (a picture, a PDF, a player, a SQLite database): then the bytes
// are written to the window's folder of copies (RemoteCopies.h) and the
// viewer is pointed at the copy, while the tab, its title and the tree keep
// the remote path (Document::setViewerPath). CSV and TSV tables, the JSON
// outline and notebooks show the tab's text, so they need nothing here.
//
// The host's disk watching (RemoteWorkspace::onFiles) says which folders
// changed: the copies in them are read again and, when their bytes differ,
// written again; the viewers watch their file and show it again (a PDF
// keeps its page). Pictures in a Markdown page come through the same
// folder, fetched without waiting while their alt text stands in, and the
// page is drawn again when one arrives.
#include "Editor.h"

#include "FileSniff.h"
#include "MarkdownSide.h"
#include "MediaView.h"
#include "RemoteCache.h"
#include "RemoteCopies.h"
#include "RemoteWorkspace.h"
#include "SessionLink.h"
#include "SqliteView.h"

#include <algorithm>

namespace {
std::string nameOf(const std::string& path) {
    char* b = g_path_get_basename(path.c_str());
    std::string out = b ? b : path;
    g_free(b);
    return out;
}
}  // namespace

void Editor::setRemoteWorkspace(std::shared_ptr<RemoteWorkspace> ws) {
    if (remoteWs_) remoteWs_->onFiles = nullptr;
    remoteWs_ = std::move(ws);
    if (!remoteWs_) return;
    std::weak_ptr<int> alive = alive_;
    remoteWs_->onFiles = [this, alive](const std::vector<std::string>& folders) {
        if (!alive.expired()) remoteFoldersChanged(folders);
    };
}

RemoteCopies* Editor::copiesForWrite() {
    if (!copies_ && remote() && !link_->folder().empty()) copies_ = std::make_unique<RemoteCopies>(link_->folder());
    return copies_.get();
}

void Editor::removeRemoteCopies() {
    if (remoteWs_) remoteWs_->onFiles = nullptr;
    if (copies_) copies_->removeAll();
}

// The copies' folders join the tree's in the host's watch.
void Editor::watchCopies() {
    if (remoteWs_ && copies_) remoteWs_->watchAlso(copies_->remoteFolders());
}

// ------------------------------------------------------------ opening

bool Editor::documentBytesRead(Document* d, std::string& bytes) {
    if (!remote() || !d) return false;
    const std::string path = d->path();
    const bool media = MediaView::handles(path);
    const bool database =
        !media && FileSniff::classify(path, bytes.data(), std::min(bytes.size(), FileSniff::kHeadBytes)) ==
                      FileSniff::Kind::Sqlite;
    if (!media && !database) return false;   // text, or what the text path says it is not
    RemoteCopies* copies = copiesForWrite();
    const std::string copy = copies ? copies->store(bytes, path) : std::string();
    if (copy.empty()) {
        d->setMessage(path, "\n  Cannot display “" + nameOf(path) + "”.\n\n  (It could not be copied from " +
                                link_->label() + " to this machine's temporary folder.)");
    } else {
        if (media) d->setMedia(path);
        else d->setDatabase(path);
        d->setViewerPath(copy);
        watchCopies();
    }
    d->place().haveRestore = false;
    if (d == active_) showActive();
    else refreshStrip();
    return true;
}

// ------------------------------------------------------------ following the host's disk

void Editor::remoteFoldersChanged(const std::vector<std::string>& folders) {
    if (!copies_ || folders.empty()) return;
    for (const std::string& path : copies_->foldersChanged(folders)) {
        Document* d = documentFor(path);
        const bool viewer = d && d->showsCopy();
        // A page's picture is drawn again only when the page is on show.
        if (!viewer && !inMarkdownPage()) continue;
        const std::weak_ptr<int> alive = alive_;
        fetchCopy(path, [this, alive, path, viewer](bool changed) {
            if (alive.expired() || !changed || viewer) return;   // a viewer watches its copy
            if (inMarkdownPage()) rerenderPreview();
            if (side_ && gtk_widget_get_visible(side_->widget())) {
                side_->invalidate();
                side_->schedule(0);
            }
        });
    }
}

void Editor::fetchCopy(const std::string& path, std::function<void(bool changed)> done) {
    RemoteCopies* copies = copiesForWrite();
    if (!copies || !link_ || copies->fetching.count(path)) return;
    std::weak_ptr<int> alive = alive_;
    copies->fetching.insert(path);
    const bool asked = link_->readTextAsync(path, nullptr, [this, alive, path, done](SessionLink::Link::RemoteBytes&& r) {
        if (alive.expired() || !copies_) return;
        copies_->fetching.erase(path);
        const bool again = copies_->again.erase(path) != 0;
        bool changed = false;
        if (!r.ok) {
            copies_->failed.insert(path);   // asked again once its folder changes
        } else if (copies_->localCopyHolds(path, r.bytes)) {
            copies_->markFresh(path);
        } else if (!copies_->store(r.bytes, path).empty()) {
            copies_->refreshes++;
            changed = true;
        }
        // Told of another change while it read (a save is a write and a
        // rename): read again, so the last one is shown.
        if (again) {
            copies_->markStale(path);
            fetchCopy(path, done);
        }
        done(changed);
    });
    if (!asked) copies->fetching.erase(path);
}

// ------------------------------------------------------------ Markdown pictures

// The local file a Markdown picture `src` stands for in a window over SSH:
// its copy, when there is one (read again in the background when the host
// said its folder changed); else "" while it is fetched (the page is drawn
// again when it comes) or when it names nothing of the remote folder.
std::string Editor::remotePicture(const std::string& src, const std::string& docFolder) {
    RemoteCopies* copies = copiesForWrite();
    if (!copies) return "";
    const std::string path = RemoteCache::markdownPicture(src, docFolder, link_->folder());
    if (path.empty()) return "";   // nothing of this machine's disk, nothing outside the folder
    const std::weak_ptr<int> alive = alive_;
    auto onArrival = [this, alive](bool changed) {
        if (alive.expired() || !changed) return;
        if (copies_) watchCopies();
        if (inMarkdownPage()) rerenderPreview();
        if (side_ && gtk_widget_get_visible(side_->widget())) {   // the side preview's page
            side_->invalidate();
            side_->schedule(0);
        }
    };
    if (copies->hasCopyOf(path)) {
        if (copies->isStale(path)) fetchCopy(path, onArrival);
        return copies->localPathOf(path);
    }
    if (copies->failed.count(path)) return "";
    fetchCopy(path, onArrival);
    return "";
}
