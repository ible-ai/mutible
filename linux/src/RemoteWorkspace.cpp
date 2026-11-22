// RemoteWorkspace.cpp — see RemoteWorkspace.h.
#include "RemoteWorkspace.h"

#include <glib.h>

#include <algorithm>

#include "SessionLink.h"

using SessionLink::Link;

RemoteWorkspace::RemoteWorkspace(std::string folder) : folder_(std::move(folder)) {
    while (folder_.size() > 1 && folder_.back() == '/') folder_.pop_back();
}

RemoteWorkspace::~RemoteWorkspace() { detach(); }

bool RemoteWorkspace::live() const { return link_ && !link_->lost(); }

void RemoteWorkspace::attachLink(const std::shared_ptr<Link>& link) {
    if (link_ && diskToken_) link_->removeDiskListener(diskToken_);
    diskToken_ = 0;
    link_ = link;
    watchSent_.clear();   // a new host knows nothing of the old one's watch
    if (!link_) return;
    std::weak_ptr<RemoteWorkspace> weak = shared_from_this();
    diskToken_ = link_->addDiskListener([weak](std::uint64_t, const Json& changed) {
        if (auto self = weak.lock()) self->tookNotice(changed);
    });
    if (watchAsked_) sendWatch();
}

void RemoteWorkspace::detach() {
    if (link_ && diskToken_) link_->removeDiskListener(diskToken_);
    diskToken_ = 0;
    link_.reset();
    if (watchIdle_) g_source_remove(watchIdle_);
    watchIdle_ = 0;
}

bool RemoteWorkspace::relative(const std::string& path, std::string* rel) const {
    if (path == folder_) {
        rel->clear();
        return true;
    }
    const std::string base = folder_ == "/" ? std::string() : folder_;
    if (path.size() <= base.size() + 1 || path.compare(0, base.size(), base) != 0 || path[base.size()] != '/')
        return false;
    *rel = path.substr(base.size() + 1);
    return true;
}

std::string RemoteWorkspace::absolute(const std::string& rel) const {
    if (rel.empty()) return folder_;
    return (folder_ == "/" ? std::string() : folder_) + "/" + rel;
}

bool RemoteWorkspace::listFolder(const std::string& path, std::function<void(bool, std::vector<Entry>&&)> done) {
    std::string rel;
    if (!live() || !relative(path, &rel)) return false;
    Json ask = Json::object();
    ask.set("op", Json("list"));
    ask.set("path", Json(rel));
    ask.set("hidden", Json(true));   // the tree's own filter hides dotfiles
    std::weak_ptr<RemoteWorkspace> weak = shared_from_this();
    return link_->readAsync(std::move(ask), [weak, done](bool ok, const Json& answer) {
        std::vector<Entry> out;
        if (auto self = weak.lock()) self->listings++;
        if (ok && answer["error"].isNull())
            for (const Json& e : answer["entries"].elements()) {
                Entry en;
                en.name = e["name"].asString();
                if (en.name.empty()) continue;
                en.dir = e["dir"].asBool();
                en.link = e["link"].asBool();
                en.ignored = e["ignored"].asBool();
                en.outside = e["outside"].asBool();
                out.push_back(std::move(en));
            }
        done(ok, std::move(out));   // a folder gone is an error answer: nothing in it
    });
}

bool RemoteWorkspace::hostDoesFileOps() const { return live() && link_->client().hostDoesFileOps(); }

bool RemoteWorkspace::fileOp(Json request, std::function<void(bool, const Json&)> done) {
    if (!hostDoesFileOps()) return false;
    return link_->fileOpAsync(std::move(request), std::move(done));
}

namespace {
std::vector<std::string> relatives(const RemoteWorkspace& ws, const std::vector<std::string>& paths) {
    std::vector<std::string> rels;
    for (const std::string& p : paths) {
        std::string rel;
        if (ws.relative(p, &rel)) rels.push_back(rel);
    }
    return rels;
}
}  // namespace

void RemoteWorkspace::watchFolders(std::vector<std::string> paths) {
    treeRels_ = relatives(*this, paths);
    askWatch();
}

void RemoteWorkspace::watchAlso(std::vector<std::string> paths) {
    std::vector<std::string> rels = relatives(*this, paths);
    std::sort(rels.begin(), rels.end());
    if (rels == alsoRels_) return;
    alsoRels_ = std::move(rels);
    askWatch();
}

void RemoteWorkspace::askWatch() {
    std::vector<std::string> rels = treeRels_;
    rels.insert(rels.end(), alsoRels_.begin(), alsoRels_.end());
    std::sort(rels.begin(), rels.end());
    rels.erase(std::unique(rels.begin(), rels.end()), rels.end());
    watchPaths_ = std::move(rels);
    watchAsked_ = true;
    if (watchIdle_) return;
    // Once a turn of the loop: a folder opening makes several changes.
    auto* weak = new std::weak_ptr<RemoteWorkspace>(weak_from_this());
    watchIdle_ = g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        if (auto self = static_cast<std::weak_ptr<RemoteWorkspace>*>(p)->lock()) {
            self->watchIdle_ = 0;
            self->sendWatch();
        }
        return G_SOURCE_REMOVE;
    }, weak, [](gpointer p) { delete static_cast<std::weak_ptr<RemoteWorkspace>*>(p); });
}

void RemoteWorkspace::sendWatch() {
    if (!live() || !link_->client().hostWatches()) return;
    std::string key = "h";
    for (const std::string& r : watchPaths_) key += "\n" + r;
    if (key == watchSent_) return;
    const std::uint64_t req = link_->client().watchFolders(0, watchPaths_, true);
    if (!req) return;
    link_->client().forgetRead(req);   // its answer says nothing the notices will not
    watchSent_ = key;
}

void RemoteWorkspace::tookNotice(const Json& changed) {
    std::vector<std::string> folders;
    for (const Json& l : changed["listings"].elements()) folders.push_back(absolute(l["path"].asString()));
    notices++;
    if (changed["git"].asBool()) {
        gitNotices++;
        if (onGit) onGit();
    }
    if (!folders.empty() && onChanged) onChanged(folders);
    if (!folders.empty() && onFiles) onFiles(folders);
}

void RemoteWorkspace::listFiles(std::function<void(std::vector<std::string>, bool)> done) {
    Json ask = Json::object();
    ask.set("op", Json("files"));
    ask.set("hidden", Json(true));   // as the local Quick Open lists them
    std::weak_ptr<RemoteWorkspace> weak = shared_from_this();
    const std::uint64_t req = live() ? link_->stream(std::move(ask), [weak, done](std::vector<Json>&& answers, bool last) {
        if (!last) return;
        std::vector<std::string> files;
        bool truncated = false;
        for (const Json& a : answers) {
            for (const Json& f : a["files"].elements())
                if (!f.asString().empty()) files.push_back(f.asString());
            truncated = truncated || a["truncated"].asBool();
        }
        if (auto self = weak.lock()) self->fileLists++;
        done(std::move(files), truncated);
    }) : 0;
    if (!req) done({}, false);
}

std::uint64_t RemoteWorkspace::search(Json request, std::function<void(std::vector<Json>&&, bool)> answers) {
    if (!live()) return 0;
    const std::uint64_t req = link_->stream(std::move(request), std::move(answers));
    if (req) searches++;
    return req;
}

std::uint64_t RemoteWorkspace::replace(Json request, std::function<void(std::vector<Json>&&, bool)> answers) {
    if (!live()) return 0;
    return link_->replace(std::move(request), std::move(answers));
}

void RemoteWorkspace::cancelSearch(std::uint64_t req) {
    if (link_ && req) link_->cancelStream(req);
}

bool RemoteWorkspace::hostRunsGit() const { return live() && link_->runsGit(); }

std::string RemoteWorkspace::label() const { return link_ ? link_->label() : std::string(); }

bool RemoteWorkspace::git(const std::string& dir, const std::vector<std::string>& args, std::string input,
                          double timeout, std::function<void(bool, const Shared::GitReply&)> done) {
    if (!hostRunsGit()) return false;
    gitRuns++;
    return link_->gitAsync(Shared::gitRequest(dir, args, timeout), std::move(input),
                           [done](bool ok, const Json& answer, std::string&& out) {
                               if (!ok) return done(false, Shared::GitReply());
                               done(true, Shared::gitReplyFrom(answer, std::move(out)));
                           });
}
