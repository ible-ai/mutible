// RemoteWorkspace.mm — see RemoteWorkspace.h.
#import "RemoteWorkspace.h"
#import "SessionLink.h"

#include <algorithm>
#include <string>

#include "GitStatus.h"
#include "SessionGit.h"

@interface MCRemoteWorkspace ()
// The link's "host:path", for what git's runner says off the main thread.
@property(atomic, copy) NSString *labelText;
@end

@implementation MCRemoteWorkspace {
    MCSessionLinkRef *_link;
    int _diskToken;
    std::string _watchSent;            // the last watch sent, as its paths and rule
    std::vector<std::string> _watchPaths;
    BOOL _watchHidden, _watchAsked;
    NSMutableSet<NSString *> *_new;
    // treePrefetchFolders:'s answers, by hidden and path, each taken once
    // by the next treeListFolder: of it, and let go at the turn's end.
    NSMutableDictionary<NSString *, NSArray<MCTreeEntry *> *> *_prefetched;
}

- (instancetype)initWithFolder:(NSString *)folder {
    if ((self = [super init])) {
        _folder = [folder copy];
        _new = [NSMutableSet set];
    }
    return self;
}

- (void)dealloc {
    [self detach];
}

- (MCSession::Link *)live {
    MCSession::Link *l = _link.link.get();
    return l && !l->lost() ? l : nullptr;
}

// '/'-separated from the folder, "" for the folder itself; NO for a path
// outside it.
- (BOOL)relative:(NSString *)path out:(std::string *)rel {
    if ([path isEqualToString:_folder]) {
        rel->clear();
        return YES;
    }
    NSString *prefix = [_folder hasSuffix:@"/"] ? _folder : [_folder stringByAppendingString:@"/"];
    if (![path hasPrefix:prefix] || path.length == prefix.length) return NO;
    *rel = [path substringFromIndex:prefix.length].UTF8String ?: "";
    return !rel->empty();
}

- (NSString *)absolute:(const std::string &)rel {
    if (rel.empty()) return _folder;
    NSString *r = [NSString stringWithUTF8String:rel.c_str()];
    return r ? [_folder stringByAppendingPathComponent:r] : nil;
}

- (void)attachLink:(MCSessionLinkRef *)link {
    if (_link.link && _diskToken) _link.link->removeDiskListener(_diskToken);
    _diskToken = 0;
    _link = link;
    _watchSent.clear();   // a new host knows nothing of the old one's watch
    if (!_link.link) return;
    self.labelText = @(_link.link->label().c_str());
    __weak MCRemoteWorkspace *weakSelf = self;
    _diskToken = _link.link->addDiskListener([weakSelf](const Json &changed) {
        // Told inside the link's dispatch, which may be inside a wait for a
        // read: the tree takes it on the next turn.
        Json copy = changed;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf tookNotice:copy]; });
    });
    if (_watchAsked) [self sendWatch];
}

- (void)detach {
    if (_link.link && _diskToken) _link.link->removeDiskListener(_diskToken);
    _diskToken = 0;
    _link = nil;
}

// ------------------------------------------------------------ the tree

- (NSArray<MCTreeEntry *> *)entriesOf:(const Json &)answer {
    const std::string rel = answer["path"].asString();
    NSString *dir = [self absolute:rel];
    if (!dir) return nil;
    if (!answer["error"].isNull()) return @[];   // gone, or not a folder: nothing in it
    NSMutableArray<MCTreeEntry *> *out = [NSMutableArray array];
    for (const Json &e : answer["entries"].elements()) {
        NSString *name = [NSString stringWithUTF8String:e["name"].asString().c_str()];
        if (!name.length) continue;
        MCTreeEntry *t = [MCTreeEntry new];
        t.path = [dir stringByAppendingPathComponent:name];
        t.isDir = e["dir"].asBool();
        t.isLink = e["link"].asBool();
        t.ignored = e["ignored"].asBool();
        t.outside = e["outside"].asBool();
        t.size = -1;   // the host's listing names no sizes or dates
        [out addObject:t];
    }
    return out;
}

static NSString *PrefetchKey(NSString *path, BOOL hidden) {
    return [(hidden ? @"h:" : @"-:") stringByAppendingString:path];
}

- (NSArray<MCTreeEntry *> *)treeListFolder:(NSString *)path hidden:(BOOL)hidden {
    NSString *key = PrefetchKey(path, hidden);
    if (NSArray<MCTreeEntry *> *ready = _prefetched[key]) {
        [_prefetched removeObjectForKey:key];
        return ready;
    }
    std::string rel;
    MCSession::Link *l = [self live];
    if (!l || ![self relative:path out:&rel]) return nil;
    Json ask = Json::object();
    ask.set("op", Json("list"));
    ask.set("path", Json(rel));
    ask.set("hidden", Json((bool)hidden));
    const Json answer = l->client().read(0, std::move(ask), 20000);
    if (answer.isNull()) return nil;   // no answer: what the tree has stays
    _listings++;
    return [self entriesOf:answer];
}

- (void)treePrefetchFolders:(NSArray<NSString *> *)paths hidden:(BOOL)hidden {
    MCSession::Link *l = [self live];
    if (!l || paths.count < 2) return;   // one folder gains nothing: it is listed as it comes
    std::vector<Json> asks;
    NSMutableArray<NSString *> *asked = [NSMutableArray array];
    for (NSString *p in paths) {
        std::string rel;
        if (![self relative:p out:&rel]) continue;
        Json ask = Json::object();
        ask.set("op", Json("list"));
        ask.set("path", Json(rel));
        ask.set("hidden", Json((bool)hidden));
        asks.push_back(std::move(ask));
        [asked addObject:p];
    }
    const std::vector<Json> answers = l->client().readAll(0, std::move(asks), 20000);
    if (!_prefetched) _prefetched = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < asked.count && i < answers.size(); i++) {
        if (answers[i].isNull()) continue;   // no answer: listed as it comes, as before
        NSArray<MCTreeEntry *> *entries = [self entriesOf:answers[i]];
        if (!entries) continue;
        _listings++;
        _prefetched[PrefetchKey(asked[i], hidden)] = entries;
    }
    // Only for the listings that follow at once: never a stale one later.
    __weak MCRemoteWorkspace *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCRemoteWorkspace *s = weakSelf;
        if (s) [s->_prefetched removeAllObjects];
    });
}

- (void)tookNotice:(const Json &)changed {
    if (!_link) return;
    _notices++;
    NSMutableDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings = [NSMutableDictionary dictionary];
    for (const Json &answer : changed["listings"].elements()) {
        NSString *dir = [self absolute:answer["path"].asString()];
        NSArray<MCTreeEntry *> *entries = [self entriesOf:answer];
        // A folder gone comes as its error: its parent's listing drops it.
        if (dir && entries && answer["error"].isNull()) listings[dir] = entries;
    }
    if (listings.count && self.onListings) self.onListings(listings);
    if (changed["git"].asBool()) {
        _gitNotices++;
        if (self.onGitChanged) self.onGitChanged();
    }
}

- (void)watchFolders:(NSArray<NSString *> *)paths hidden:(BOOL)hidden {
    std::vector<std::string> rels;
    for (NSString *p in paths) {
        std::string rel;
        if ([self relative:p out:&rel]) rels.push_back(rel);
    }
    std::sort(rels.begin(), rels.end());
    rels.erase(std::unique(rels.begin(), rels.end()), rels.end());
    _watchPaths = rels;
    _watchHidden = hidden;
    _watchAsked = YES;
    [self sendWatch];
}

- (void)sendWatch {
    MCSession::Link *l = [self live];
    if (!l || !l->client().hostWatches()) return;
    std::string key = _watchHidden ? "h" : "-";
    for (const std::string &r : _watchPaths) key += "\n" + r;
    if (key == _watchSent) return;
    const std::uint64_t req = l->client().watchFolders(0, _watchPaths, _watchHidden);
    if (!req) return;
    l->client().forgetRead(req);   // its answer says nothing the listings will not
    _watchSent = key;
}

// ------------------------------------------------- Quick Open, Find in Folder

- (void)listFiles:(void (^)(NSArray<NSString *> *, BOOL))done {
    MCSession::Link *l = [self live];
    Json ask = Json::object();
    ask.set("op", Json("files"));
    ask.set("hidden", Json(true));   // as the Mac's own Quick Open lists them
    void (^finish)(NSArray<NSString *> *, BOOL) = [done copy];
    const std::uint64_t req = l ? l->stream(std::move(ask), [finish, self](std::vector<Json> &&answers, bool last) {
        if (!last) return;
        NSMutableArray<NSString *> *files = [NSMutableArray array];
        BOOL truncated = NO;
        for (const Json &a : answers) {
            for (const Json &f : a["files"].elements()) {
                NSString *s = [NSString stringWithUTF8String:f.asString().c_str()];
                if (s.length) [files addObject:s];
            }
            truncated = truncated || a["truncated"].asBool();
        }
        self->_fileLists++;
        finish(files, truncated);
    }) : 0;
    if (!req) finish(@[], NO);
}

- (uint64_t)search:(Json)request answers:(void (^)(const std::vector<Json> &, BOOL))answers {
    MCSession::Link *l = [self live];
    if (!l) return 0;
    request.set("op", Json("search"));
    void (^take)(const std::vector<Json> &, BOOL) = [answers copy];
    const std::uint64_t req =
        l->stream(std::move(request), [take](std::vector<Json> &&got, bool last) { take(got, last); });
    if (req) _searches++;
    return req;
}

- (void)cancelSearch:(uint64_t)req {
    if (req && _link.link) _link.link->cancelStream(req);
}

- (BOOL)replacesInFolder {
    MCSession::Link *l = [self live];
    return l && l->replacesInFolder();
}

- (uint64_t)replace:(Json)request answers:(void (^)(const std::vector<Json> &, BOOL))answers {
    MCSession::Link *l = [self live];
    if (!l) return 0;
    void (^take)(const std::vector<Json> &, BOOL) = [answers copy];
    const std::uint64_t req =
        l->replace(std::move(request), [take](std::vector<Json> &&got, bool last) { take(got, last); });
    if (req) _replaces++;
    return req;
}

// ------------------------------------------------------------ file operations

- (BOOL)doesFileOps {
    MCSession::Link *l = [self live];
    return l && l->doesFileOps();
}

// One operation, its paths already relative: nil when the host did it.
- (NSString *)fileOp:(Json)request reason:(NSString **)reason {
    if (reason) *reason = @"";
    MCSession::Link *l = [self live];
    if (!l) return @"The connection to the other machine is lost: Reconnect, then try again.";
    if (!l->doesFileOps())
        return @"mutible on the other machine is too old to change files from here: upgrade it there.";
    Json answer;
    _fileOps++;
    if (!l->fileOp(std::move(request), &answer)) {
        if (reason) *reason = @"noanswer";
        return @"The other machine did not answer.";
    }
    if (answer["ok"].asBool()) return nil;
    if (reason) *reason = [NSString stringWithUTF8String:answer["reason"].asString().c_str()] ?: @"";
    NSString *why = [NSString stringWithUTF8String:answer["error"].asString().c_str()];
    return why.length ? why : @"The other machine could not do it.";
}

- (NSString *)renamePath:(NSString *)path to:(NSString *)name reason:(NSString **)reason {
    std::string rel;
    if (![self relative:path out:&rel] || rel.empty()) return @"That is not in the folder.";
    Json ask = Json::object();
    ask.set("op", Json("rename"));
    ask.set("path", Json(rel));
    ask.set("name", Json(std::string(name.UTF8String ?: "")));
    return [self fileOp:std::move(ask) reason:reason];
}

- (NSString *)movePath:(NSString *)path into:(NSString *)folder reason:(NSString **)reason {
    std::string rel, to;
    if (![self relative:path out:&rel] || rel.empty() || ![self relative:folder out:&to])
        return @"That is not in the folder.";
    Json ask = Json::object();
    ask.set("op", Json("move"));
    ask.set("path", Json(rel));
    ask.set("to", Json(to));   // "" for the folder itself
    return [self fileOp:std::move(ask) reason:reason];
}

- (NSString *)deletePath:(NSString *)path reason:(NSString **)reason {
    std::string rel;
    if (![self relative:path out:&rel] || rel.empty()) return @"That is not in the folder.";
    Json ask = Json::object();
    ask.set("op", Json("delete"));
    ask.set("path", Json(rel));
    ask.set("permanent", Json(true));   // asked first: the host has no Trash
    return [self fileOp:std::move(ask) reason:reason];
}

- (NSString *)makePath:(NSString *)path folder:(BOOL)folder reason:(NSString **)reason {
    std::string rel;
    if (![self relative:path out:&rel] || rel.empty()) return @"That is not in the folder.";
    Json ask = Json::object();
    ask.set("op", Json(folder ? "newFolder" : "newFile"));
    ask.set("path", Json(rel));
    return [self fileOp:std::move(ask) reason:reason];
}

// ------------------------------------------------------------ git

namespace {
MCGitResult *GitFailed(const std::string &why) {
    MCGitResult *r = [MCGitResult new];
    r.status = -1;
    r.output = [NSData data];
    r.errorText = [NSString stringWithUTF8String:why.c_str()] ?: @"git did not run.";
    return r;
}
}  // namespace


- (MCGitRunner)gitRunner {
    __weak MCRemoteWorkspace *weakSelf = self;
    return ^MCGitResult *(NSString *dir, NSArray<NSString *> *args, NSData *input, NSTimeInterval timeout) {
        std::vector<std::string> argv;
        for (NSString *a in args) argv.push_back(a.UTF8String ?: "");
        // What the window's title calls the folder ("host:path"), kept on
        // the main thread as each link is attached.
        NSString *labelText = weakSelf.labelText;
        const std::string label = labelText.length ? std::string(labelText.UTF8String ?: "") : "the other machine";
        // They would want credentials nobody can type there.
        if (Git::talksToRemote(argv)) return GitFailed(Shared::gitRemoteOpText(label));
        // The answer comes on the main queue: never wait for it there.
        if ([NSThread isMainThread]) return GitFailed("git for a window over SSH runs off the main thread.");
        const std::string where = dir.UTF8String ?: "";
        std::string stdinBytes = input.length ? std::string((const char *)input.bytes, input.length) : std::string();
        // Filled on the main queue, read here once the semaphore says so.
        NSMutableArray<MCGitResult *> *result = [NSMutableArray arrayWithCapacity:1];
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCRemoteWorkspace *ws = weakSelf;
            MCSession::Link *l = ws ? [ws live] : nullptr;
            if (!l) {
                [result addObject:GitFailed(Shared::gitLostText(label))];
                dispatch_semaphore_signal(answered);
                return;
            }
            if (!l->runsGit()) {
                [result addObject:GitFailed(Shared::gitTooOldText(l->label()))];
                dispatch_semaphore_signal(answered);
                return;
            }
            ws->_gitRuns++;
            const std::string lostText = Shared::gitLostText(l->label());
            const std::uint64_t req =
                l->git(where, argv, std::move(stdinBytes), timeout,
                       [answered, lostText, result](bool got, Json &&answer, std::string &&out) {
                           if (!got) {
                               [result addObject:GitFailed(lostText)];
                           } else {
                               Shared::GitReply reply = Shared::gitReplyFrom(answer, std::move(out));
                               MCGitResult *r = [MCGitResult new];
                               r.status = reply.status;
                               r.output = [NSData dataWithBytes:reply.out.data() length:reply.out.size()];
                               r.errorText = [[NSString stringWithUTF8String:reply.err.c_str()]
                                   stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                               r.timedOut = reply.timedOut;
                               [result addObject:r];
                           }
                           dispatch_semaphore_signal(answered);
                       });
            if (!req) {
                [result addObject:GitFailed(lostText)];
                dispatch_semaphore_signal(answered);
            }
        });
        // The host stops git at its own limit; this waits that long and a
        // little more for the answer to cross.
        const double wait = (timeout > 0 ? timeout : Git::timeoutFor(argv)) + 30;
        if (dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC))) != 0) {
            MCGitResult *r = GitFailed("The other machine did not answer within " + Git::durationText(wait) + ": " +
                                       Git::commandLine(argv));
            r.timedOut = YES;
            return r;
        }
        return result.firstObject ?: GitFailed("git did not run.");
    };
}

// ------------------------------------------------------------ new files

- (void)markNew:(NSString *)path {
    if (path) [_new addObject:path];
}
- (BOOL)isNew:(NSString *)path { return path && [_new containsObject:path]; }

@end
