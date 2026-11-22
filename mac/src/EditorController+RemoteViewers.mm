// EditorController+RemoteViewers.mm — EditorController: a window over SSH
// showing its files in the viewers (docs/notes/session-host.md, "Viewers
// over SSH"; the rules in the core's RemoteCache.h). A file is read through
// the host once (Link::readBytes); text goes to the editor as before (and
// to the viewers over or beside text: a CSV table, a JSON outline, a
// notebook), and anything a viewer reads from a file of its own (a
// picture, a PDF, a player, SQLite, Quick Look, a table too large to edit)
// is written to the window's folder of copies (MCRemoteFileCache) and the
// viewer pointed at the copy. The tab, its title and the tree keep the
// remote path. A file over the limit shows a placeholder saying so.
//
// The host's disk watching (FsChanged, the workspace's onListings) says
// which folders changed: the copy on show is read again without waiting
// (Link::fetchBytes) and shown again when its bytes differ, as a picture or
// PDF on this Mac is when its file changes; a copy in another tab is read
// again when that tab comes back. Pictures in a Markdown page come through
// the same folder of copies, fetched without waiting while their alt text
// stands in, as a web picture is.
//
// The window's state is the MCRemoteFileCache, an associated object, so the
// controller gains no instance variables. Every method does nothing (or NO)
// in a window on this Mac.
#import "EditorController+Private.h"
#import "MarkdownImage.h"
#import "RemoteFileCache.h"
#import "SessionLink.h"
#import <objc/runtime.h>

#include "FolderSearch.h"
#include "RemoteCache.h"

static char kRemoteFileCacheKey;

static NSString *Str(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

@implementation EditorController (RemoteViewers)

- (MCRemoteFileCache *)remoteFileCache {
    MCRemoteFileCache *cache = objc_getAssociatedObject(self, &kRemoteFileCacheKey);
    if (cache || ![self sessionIsRemote] || !_root.path) return cache;
    cache = [[MCRemoteFileCache alloc] initWithRemoteFolder:_root.path];
    objc_setAssociatedObject(self, &kRemoteFileCacheKey, cache, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return cache;
}

- (MCRemoteFileCache *)existingRemoteFileCache {
    return objc_getAssociatedObject(self, &kRemoteFileCacheKey);
}

- (NSString *)viewerPathOf:(MCTab *)t {
    if (![self sessionIsRemote]) return t.currentPath;
    return [[self existingRemoteFileCache] localPathOf:t.currentPath];
}

- (BOOL)remoteCopyIsStaleFor:(MCTab *)t {
    MCRemoteFileCache *cache = [self existingRemoteFileCache];
    return cache && t.currentPath && [cache isStale:t.currentPath];
}

- (NSArray<NSString *> *)remoteCopiedFolders {
    return [[self existingRemoteFileCache] remoteFolders] ?: @[];
}

- (void)removeRemoteCopies {
    [[self existingRemoteFileCache] removeAll];
}

// ------------------------------------------------------------ opening

- (BOOL)loadRemoteFile:(MCDiskText **)textOut viewer:(MCViewerKind *)viewerOut {
    MCDiskText *none = [MCDiskText new];
    *textOut = none;
    *viewerOut = MCViewerNone;
    MCSessionLinkRef *ref = [self sessionRemoteLink];
    NSString *path = self.currentPath;
    if (!ref.link || !path) return NO;
    // One read decides: the editor's text or a viewer's copy. Its own answer
    // is all that is waited for (one round trip under 4 MB).
    std::string bytes;
    bool tooLarge = false;
    const bool ok = ref.link->readBytes(path.fileSystemRepresentation, RemoteCache::kMaxBytes, &bytes, &tooLarge);
    if (!ok && !tooLarge) return NO;   // not there, refused, or the host gone: the text path says so
    std::string text = bytes;
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    const bool decodes = ok && FolderSearch::decodeFileText(&text, &encoding, &bom);
    MCDiskText *asText = none;
    if (decodes) {
        asText = [MCDiskText new];
        asText.bom = bom;
        asText.encoding = (MCTextEncoding)encoding;
        asText.text = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    }
    const RemoteCache::Show show =
        RemoteCache::showFor(path.fileSystemRepresentation, tooLarge, bytes, decodes, MCCsvEditableBytes);
    if (show == RemoteCache::Show::TooLarge) {
        [self showRemoteFileTooLarge];
        return YES;
    }
    if (show == RemoteCache::Show::Text) {
        *textOut = asText;
        // A CSV's table, a JSON outline, a notebook: they show the tab's
        // text. An HTML page loads the file itself, which is not here: its
        // source shows.
        MCViewerKind kind = MCViewerKindForHead(path, bytes.data(), bytes.size(), bytes.size());
        if (MCViewerKindWithoutText(kind) || kind == MCViewerHtml) kind = MCViewerNone;
        *viewerOut = kind;
        return NO;
    }
    MCRemoteFileCache *cache = [self remoteFileCache];
    NSString *copy = [cache store:bytes of:path];
    if (!copy) {
        [self setPlainMessage:[NSString stringWithFormat:@"Cannot display “%@”.\n\n(It could not be copied from %@ to "
                                                         @"this Mac's temporary folder.)",
                                                         path.lastPathComponent, self.sessionRemoteLabel]];
        [self relayoutRightArea];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        return YES;
    }
    BOOL shown = NO;
    switch (show) {
    case RemoteCache::Show::Image: shown = [self showImageAtPath:copy]; break;
    case RemoteCache::Show::Pdf: shown = [self showPDFAtPath:copy]; break;
    case RemoteCache::Show::Media: shown = [self showMediaAtPath:copy]; break;
    default: {
        MCViewerKind kind = MCViewerKindForFile(copy);
        if (!MCViewerKindWithoutText(kind)) kind = MCViewerQuickLook;
        shown = [self showViewer:kind];
        break;
    }
    }
    // The host watches the copy's folder from now on.
    [self remoteTreeFoldersChanged];
    if (shown) return YES;
    // A picture, PDF or player that does not decode: as on this Mac, its
    // text if it is text, else "Cannot display".
    *textOut = asText;
    return NO;
}

- (void)showRemoteFileTooLarge {
    NSString *name = self.currentPath.lastPathComponent ?: @"";
    [self setPlainMessage:Str(RemoteCache::tooLargeMessage(name.UTF8String ?: "",
                                                           self.sessionRemoteLabel.UTF8String ?: ""))];
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
}

// ------------------------------------------------------------ following the host's disk

// Whether the tab shows a copy (not text).
static BOOL MCTabShowsCopy(MCTab *t) {
    return t.isImage || t.isPDF || t.isMedia || MCViewerKindWithoutText(t.viewerKind);
}

- (void)remoteFoldersChanged:(NSArray<NSString *> *)folders {
    MCRemoteFileCache *cache = [self existingRemoteFileCache];
    if (!cache || _closing || !folders.count) return;
    NSArray<NSString *> *stale = [cache foldersChanged:folders];
    MCTab *t = _activeTab;
    if (t.currentPath && [stale containsObject:t.currentPath] && MCTabShowsCopy(t)) [self refreshRemoteCopyOf:t];
}

// The copy on show read again without waiting, and shown again when it
// changed; one read at a time for a path.
- (void)refreshRemoteCopyOf:(MCTab *)t {
    MCRemoteFileCache *cache = [self existingRemoteFileCache];
    MCSessionLinkRef *ref = [self sessionRemoteLink];
    NSString *path = [t.currentPath copy];
    if (!cache || !ref.link || !path || [cache.fetching containsObject:path]) return;
    __weak EditorController *weakSelf = self;
    __weak MCTab *weakTab = t;
    const std::uint64_t id = ref.link->fetchBytes(
        path.fileSystemRepresentation, RemoteCache::kMaxBytes, [weakSelf, weakTab, cache, path](bool ok, bool, std::string &&bytes) {
            [cache.fetching removeObject:path];
            const BOOL again = [cache takeAgain:path];
            EditorController *s = weakSelf;
            MCTab *tab = weakTab;
            // Gone, too large now, or the host lost: the copy stays as shown.
            if (!s || s->_closing || !ok) return;
            const BOOL onShow = tab && s->_activeTab == tab && [tab.currentPath isEqualToString:path];
            if ([cache localCopyOf:path holds:bytes]) {
                [cache markFresh:path];
            } else if ([cache store:bytes of:path]) {
                cache.refreshes++;
                if (onShow) [s showRemoteCopyAgain];
            }
            // Told of another change while it read (an editor's save is a
            // write and a rename): read again, so the last one is shown.
            if (again) {
                [cache markStale:path];
                if (onShow) [s refreshRemoteCopyOf:tab];
            }
        });
    if (id) [cache.fetching addObject:path];
}

// As checkExternalChange shows a changed picture, PDF, player or viewer
// again: a PDF keeps its page and zoom.
- (void)showRemoteCopyAgain {
    NSString *copy = [self viewerPathOf:_activeTab];
    if (!copy) return;
    if (self.isImage) [self showImageAtPath:copy];
    else if (self.isPDF) [self showPDFAtPath:copy];
    else if (self.isMedia) [self showMediaAtPath:copy];
    else if (MCViewerKindWithoutText(self.viewerKind)) [self showViewer:self.viewerKind];
}

// ------------------------------------------------------------ Markdown pictures

- (NSImage *)remoteMarkdownPicture:(NSString *)src waiting:(BOOL *)waiting {
    if (waiting) *waiting = NO;
    if (!src.length) return nil;
    // A data: URI holds its picture: nothing is read.
    if ([src.lowercaseString hasPrefix:@"data:"]) return MCMarkdownLoadImage(src, nil);
    MCRemoteFileCache *cache = [self remoteFileCache];
    NSString *doc = self.currentPath.stringByDeletingLastPathComponent;
    if (!cache || !doc) return nil;
    const std::string remote =
        RemoteCache::markdownPicture(src.UTF8String ?: "", doc.fileSystemRepresentation, _root.path.fileSystemRepresentation);
    if (remote.empty()) return nil;   // nothing of this Mac's disk, nothing outside the folder
    NSString *path = Str(remote);
    if ([cache hasCopyOf:path]) {
        // Shown as copied; read again in the background when the host said
        // its folder changed, the page drawn again if it did.
        if ([cache isStale:path]) [self fetchRemotePicture:path src:src];
        return MCMarkdownLoadImage([cache localPathOf:path], nil);
    }
    if ([cache.failed containsObject:path]) return nil;
    const BOOL asked = [self fetchRemotePicture:path src:src];
    if (waiting) *waiting = asked;
    return nil;
}

// YES while the picture is on its way. When it comes (and differs from
// the copy), the page is drawn again as for a web picture
// (markdownWebImageArrived:).
- (BOOL)fetchRemotePicture:(NSString *)path src:(NSString *)src {
    MCRemoteFileCache *cache = [self existingRemoteFileCache];
    MCSessionLinkRef *ref = [self sessionRemoteLink];
    if (!cache || !ref.link) return NO;
    if ([cache.fetching containsObject:path]) return YES;
    __weak EditorController *weakSelf = self;
    NSString *source = [src copy];
    const std::uint64_t id = ref.link->fetchBytes(
        path.fileSystemRepresentation, RemoteCache::kMaxBytes, [weakSelf, cache, path, source](bool ok, bool, std::string &&bytes) {
            [cache.fetching removeObject:path];
            const BOOL again = [cache takeAgain:path];
            EditorController *s = weakSelf;
            if (!s || s->_closing) return;
            BOOL changed = NO;
            if (!ok) {
                [cache.failed addObject:path];   // asked again once its folder changes
            } else if ([cache localCopyOf:path holds:bytes]) {
                [cache markFresh:path];
            } else {
                changed = [cache store:bytes of:path] != nil;
                if (changed) [s remoteTreeFoldersChanged];   // its folder watched
            }
            if (again) [cache markStale:path];   // read again at the next render
            if (changed) [s->_awaitedWebImages addObject:source];
            [[NSNotificationCenter defaultCenter]
                postNotificationName:MCMarkdownWebImageNotification
                              object:nil
                            userInfo:@{MCMarkdownWebImageURLKey: source, MCMarkdownWebImageOKKey: @(changed)}];
        });
    if (id) [cache.fetching addObject:path];
    return id != 0;
}

@end
