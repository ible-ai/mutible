// EditorController+Remote.mm — EditorController: a window over SSH's folder
// through its host (docs/notes/session-host.md, "The window's tree, Quick
// Open and Find in Folder over SSH"). The tree lists through the host
// (MCRemoteWorkspace as the root's MCTreeSource) and follows the host's
// notices in place of FSEvents; Quick Open ranks the host's list of files;
// Find in Folder searches on the host and its Replace All replaces there;
// the tree's rename, move by drag, delete (permanent: the host has no
// Trash), New File and New Folder are the host's file operations (Welcome
// flag 64; on an older host they are unavailable, but New File, which
// opens a tab its first save makes there); what would read this Mac's disk
// at the folder's path (Reveal in Finder, drags out, Quick Look, the TODO
// list) says it does not work over SSH yet. Its git runs on the host too
// (stage 9, docs/notes/git.md, "Over SSH"): the tree's colors and the
// branch from startRemoteWorkspace, Source Control and the change bars as
// they are made (hostGitRunner), the host's notices refreshing them. So do
// its language servers (stage 9, docs/notes/lsp.md, "Over SSH"): the
// window's LspSession is given an MCLspRemoteHost on its link, through
// which every server it starts is the host's. Every
// method does nothing (or NO) in a window on this Mac. The
// window's state is the MCRemoteWorkspace, an associated object, so the
// controller gains no instance variables.
#import "EditorController+Private.h"
#import "LspRemote.h"
#import "RemoteWorkspace.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import <objc/runtime.h>

static char kRemoteWorkspaceKey;

@implementation EditorController (Remote)

- (MCRemoteWorkspace *)remoteWorkspace {
    return objc_getAssociatedObject(self, &kRemoteWorkspaceKey);
}

// From useRemoteSessionLink: (before showWindow): the root lists through
// the host, never this Mac's disk at that path.
- (void)startRemoteWorkspace {
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!link.link || [self remoteWorkspace]) return;
    MCRemoteWorkspace *ws = [[MCRemoteWorkspace alloc] initWithFolder:_root.path];
    objc_setAssociatedObject(self, &kRemoteWorkspaceKey, ws, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [ws attachLink:link];
    // The address named a file not there yet: its tab opens empty.
    if (link.link->askedFileNew()) [ws markNew:@(link.link->askedFile().c_str())];
    __weak EditorController *weakSelf = self;
    ws.onListings = ^(NSDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings) {
        // The host's notices, as FSEvents' listings are taken in: only the
        // folders the tree has listed, keeping what is open and selected.
        EditorController *s = weakSelf;
        if (!s || s->_closing) return;
        [s applyTreeListings:listings stats:@{} reshowRoot:NO];
        [s remoteFoldersChanged:listings.allKeys];   // a copy a viewer shows, read again (+RemoteViewers)
    };
    // git on the host (stage 9): the tree's colors and the branch now,
    // Source Control and the change bars with the panel (useHostGit); the
    // host's word that git may have changed in place of FSEvents.
    ws.onGitChanged = ^{ [weakSelf gitChangedOnHost]; };
    _root.source = ws;
    _root.children = nil;
    _root.shown = nil;
    _root.loaded = NO;
    [_root loadChildren];
    if (self.outline) [self reloadTreeKeepingState:NO];
    [self remoteTreeFoldersChanged];
    [self watchGitOnHost:ws.gitRunner];
    [self useHostGit];   // the panel made already, if it was
    [self giveLanguageServersTheHost];
}

// Language servers on the host (stage 9): the window's LspSession is given
// an MCLspRemoteHost on its link before its first file, so no server of
// this Mac's starts for a remote path. Called from startRemoteWorkspace
// and again once the session is made (Layout's showWindow builds it after
// useRemoteSessionLink:, so at first there is none: remote_ssh's first CI
// run on #358 found the session without its host); once.
- (void)giveLanguageServersTheHost {
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!link.link || !self.lsp || self.lsp.remoteHost) return;
    [self.lsp useRemoteHost:[[MCLspRemoteHost alloc] initWithLink:link
                                                           window:[self sessionWindow].windowId]];
    // A file already on show (its tab loaded before the host was given)
    // goes to the host's servers now.
    if (self.currentPath && !self.previewMode && self.textView.editable)
        [self.lsp documentOpened:[self languageServerPath:self.currentPath]];
}

// A file's text as a tab reads it, for the language servers' edits across
// files no tab has open: the host's in a window over SSH (Link::readText),
// else this Mac's disk.
- (NSString *)languageServerDiskText:(NSString *)path {
    if (![self sessionIsRemote]) return MCReadDiskText(path, ULLONG_MAX).text;
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!link.link || link.link->lost()) return nil;
    return link.link->readText(path.fileSystemRepresentation ?: "").text;
}

- (MCGitRunner)hostGitRunner {
    return [self remoteWorkspace].gitRunner;
}

- (NSString *)folderToolsKey {
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if ([self sessionIsRemote] && link.link) return @(link.link->key().c_str());
    return self.rootPath;
}

// Reconnect: the new host lists the tree again (what changed there while
// the connection was gone shows) and watches what it has open.
- (void)remoteWorkspaceMovedToLink {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!ws || !link.link) return;
    [ws attachLink:link];
    [self reloadTreeKeepingState:YES];
    [self gitChangedOnHost];   // what changed there meanwhile
    // The language servers the drop ended start again there.
    [self.lsp.remoteHost attachLink:link];
    [self.lsp remoteHostReconnected];
}

- (void)remoteTreeFoldersChanged {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || ws.watchQueued) return;
    ws.watchQueued = YES;
    __weak EditorController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EditorController *s = weakSelf;
        MCRemoteWorkspace *w = [s remoteWorkspace];
        if (!w) return;
        w.watchQueued = NO;
        if (s->_closing) return;
        // Every folder the tree has listed, open or closed since, and those
        // of the files a viewer shows from a copy: the host sends each
        // one's listing again when it changes there.
        NSMutableOrderedSet<NSString *> *folders =
            [NSMutableOrderedSet orderedSetWithArray:[s loadedTreeFolders].allKeys];
        [folders addObjectsFromArray:[s remoteCopiedFolders]];
        [w watchFolders:folders.array hidden:gShowHidden];
    });
}

- (BOOL)refuseOverSsh:(NSString *)what {
    if (![self sessionIsRemote]) return NO;
    NSBeep();
    [self showStatusNote:[NSString stringWithFormat:@"%@ does not work in a window over SSH yet: the files are on %@. "
                                                    @"The terminal runs there.",
                                                    what, self.sessionRemoteLabel]];
    return YES;
}

// ------------------------------------------------- Quick Open, Find in Folder

- (void)addRemoteFilesToPaletteContext:(MCPaletteContext *)context {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !context) return;
    __weak MCRemoteWorkspace *weakWs = ws;
    context.listFiles = ^(void (^done)(NSArray<NSString *> *, BOOL)) {
        MCRemoteWorkspace *w = weakWs;
        if (w) [w listFiles:done];
        else done(@[], NO);
    };
}

- (void)searchOnHost:(SearchPanel *)panel {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !panel) return;
    __weak MCRemoteWorkspace *weakWs = ws;
    panel.hostSearch = ^uint64_t(Json request, void (^answers)(const std::vector<Json> &, BOOL)) {
        return [weakWs search:std::move(request) answers:answers];
    };
    panel.cancelHostSearch = ^(uint64_t req) { [weakWs cancelSearch:req]; };
    [self replaceOnHost:panel];
}

// ------------------------------------------------------------ the tree's file operations
// docs/notes/session-host.md, "The Mac window's tree changes files on the
// host": each waits for the host's answer (as the tree's listings do), and
// open tabs follow it as they follow a local rename or move.

- (BOOL)remoteDoesFileOps {
    return [self remoteWorkspace].doesFileOps;
}

- (BOOL)refuseFileOpsOverSsh:(NSString *)what {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || ws.doesFileOps) return NO;
    NSBeep();
    MCSessionLinkRef *link = [self sessionRemoteLink];
    if (!link.link || link.link->lost())
        [self showStatusNote:[NSString stringWithFormat:@"%@ waits for the connection to %@: Reconnect, then try "
                                                        @"again.",
                                                        what, self.sessionRemoteLabel]];
    else
        [self showStatusNote:[NSString stringWithFormat:@"%@ does not work over SSH with this host: mutible on %@ "
                                                        @"is too old for it. Upgrade it there.",
                                                        what, self.sessionRemoteLabel]];
    return YES;
}

// The tree's file items in a window over SSH: available when the host does
// file operations; Move to Trash is Delete Permanently there.
- (BOOL)validateRemoteTreeItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    SEL a = item.action;
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (a == @selector(deleteSelected:)) item.title = ws ? @"Delete Permanently…" : @"Move to Trash";
    if (!ws) return NO;
    if (a == @selector(newFolder:) || a == @selector(renameSelected:) || a == @selector(deleteSelected:)) {
        *enabled = ws.doesFileOps;
        return YES;
    }
    return NO;
}

// The host refused, or did not answer: its sentence, in an alert.
- (void)remoteFileOpFailed:(NSString *)what why:(NSString *)why {
    NSAlert *a = [NSAlert new];
    a.messageText = what;
    a.informativeText = why ?: @"";
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

- (void)remoteRenamePath:(NSString *)path to:(NSString *)name {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !path.length || !name.length) return;
    if ([name isEqualToString:path.lastPathComponent]) return;   // the same name: nothing to do
    NSString *why = [ws renamePath:path to:name reason:nil];
    if (why) {
        [self remoteFileOpFailed:[NSString stringWithFormat:@"“%@” was not renamed.", path.lastPathComponent]
                             why:why];
        return;
    }
    NSString *dst = [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:name];
    [self pathMovedFrom:path to:dst];   // open tabs follow, a folder's too, their text kept
    [self refreshTree:nil];
    [self revealPath:dst andOpen:NO];
}

- (BOOL)remoteMovePaths:(NSArray<NSString *> *)paths into:(NSString *)dir {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !dir.length) return NO;
    NSMutableArray<NSString *> *problems = [NSMutableArray array];
    NSString *last = nil;
    for (NSString *src in paths) {
        NSString *why = [ws movePath:src into:dir reason:nil];
        if (why) {
            [problems addObject:why];
            continue;
        }
        NSString *dst = [dir stringByAppendingPathComponent:src.lastPathComponent];
        [self pathMovedFrom:src to:dst];
        last = dst;
    }
    [self refreshTree:nil];
    if (last) [self revealPath:last andOpen:NO];
    // Not a modal alert inside the drop, as on this Mac.
    if (problems.count) {
        NSString *why = [problems componentsJoinedByString:@"\n"];
        __weak EditorController *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf remoteFileOpFailed:problems.count == paths.count ? @"Nothing was moved." : @"Some were not moved."
                                     why:why];
        });
    }
    return last != nil;
}

// After asking: the host has no Trash, so what goes is gone. Open tabs of
// what went stay, marked deleted and unsaved, as when a file is deleted on
// disk: saving one writes it again.
- (void)remoteDeletePath:(NSString *)path {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !path.length) return;
    BOOL dir = NO;
    for (MCTreeEntry *e in [ws treeListFolder:path.stringByDeletingLastPathComponent hidden:YES] ?: @[])
        if ([e.path isEqualToString:path]) dir = e.isDir && !e.isLink;
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"Delete “%@” permanently?", path.lastPathComponent];
    a.informativeText =
        dir ? [NSString stringWithFormat:@"The folder and everything in it are on %@, which has no Trash: "
                                         @"they cannot be put back.",
                                         self.sessionRemoteLabel]
            : [NSString stringWithFormat:@"It is on %@, which has no Trash: it cannot be put back.",
                                         self.sessionRemoteLabel];
    [a addButtonWithTitle:@"Delete Permanently"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    NSString *why = [ws deletePath:path reason:nil];
    if (why) {
        [self remoteFileOpFailed:[NSString stringWithFormat:@"“%@” was not deleted.", path.lastPathComponent]
                             why:why];
        return;
    }
    NSString *prefix = [path stringByAppendingString:@"/"];
    BOOL any = NO;
    for (MCTab *t in [self tabs]) {
        if (!t.currentPath || t.untitled || t.gone) continue;
        if (![t.currentPath isEqualToString:path] && ![t.currentPath hasPrefix:prefix]) continue;
        t.gone = YES;
        // Only text has anything to save.
        if (!t.isImage && !t.isPDF && !t.isMedia && !t.isDiff && !t.showingMessage && t.loaded) t.dirty = YES;
        any = YES;
    }
    if (any) {
        [self reloadTabStrip];
        [self updateTitle];
    }
    [self refreshTree:nil];
}

- (void)remoteMakePath:(NSString *)path folder:(BOOL)folder {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !path.length) return;
    NSString *why = [ws makePath:path folder:folder reason:nil];
    if (why) {
        [self remoteFileOpFailed:[NSString stringWithFormat:folder ? @"The folder “%@” was not made."
                                                                   : @"The file “%@” was not made.",
                                                            path.lastPathComponent]
                             why:why];
        return;
    }
    [self refreshTree:nil];
    [self revealPath:path andOpen:!folder];   // a new file opens, read through the host
}

// The host moved a tab's file (DocDirty's "moved": a rename or move, this
// window's or another renderer's): the tab follows, its text kept.
- (void)remoteTab:(MCTab *)t movedTo:(NSString *)path {
    if (![self remoteWorkspace] || !t.currentPath || !path.length || [t.currentPath isEqualToString:path]) return;
    [self pathMovedFrom:t.currentPath to:path];
}

// ------------------------------------------------------------ Replace in Folder

- (void)replaceOnHost:(SearchPanel *)panel {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !panel) return;
    __weak MCRemoteWorkspace *weakWs = ws;
    panel.hostReplaces = ^BOOL { return [weakWs replacesInFolder]; };
    panel.hostReplace = ^uint64_t(Json request, void (^answers)(const std::vector<Json> &, BOOL)) {
        return [weakWs replace:std::move(request) answers:answers];
    };
    panel.hostPlace = self.sessionRemoteLabel;
}

// ------------------------------------------------------------ new files

- (BOOL)remotePathIsNew:(NSString *)path {
    return [[self remoteWorkspace] isNew:path];
}

- (BOOL)showRemoteFile:(NSString *)path isNew:(BOOL)isNew {
    if (isNew) [[self remoteWorkspace] markNew:path];
    return [self showFile:path];
}

// Asked by openRemoteAddress: before it opens another window, as a local
// file goes to the window whose folder holds it. Same machine and account
// as the link compares them (sshMachineKey), then the host's own answer.
- (NSString *)remotePathOfAddress:(NSString *)address isRoot:(BOOL *)isRoot isDir:(BOOL *)isDir isNew:(BOOL *)isNew {
    *isRoot = *isDir = *isNew = NO;
    MCSessionLinkRef *ref = [self sessionRemoteLink];
    if (!ref.link || ref.link->lost() || _closing) return nil;
    Shared::SshTarget t;
    std::string error;
    if (!Shared::parseSshUrl(address.UTF8String ?: "", &t, &error)) return nil;
    if (Shared::sshMachineKey(t) != ref.link->machine()) return nil;
    MCSession::Link::Located where;
    if (!ref.link->locate(t.path, &where) || !where.inside) return nil;
    *isRoot = where.path == ref.link->folder();
    *isDir = where.dir;
    *isNew = where.isNew;
    return @(where.path.c_str());
}

// New File in the tree over SSH: a kept tab on the file, empty and
// untitled at its path, which its first save (Cmd+S) makes on the host.
// A name the folder has already is refused, as on this Mac.
- (void)openNewRemoteFile:(NSString *)path {
    MCRemoteWorkspace *ws = [self remoteWorkspace];
    if (!ws || !path.length) return;
    NSString *dir = path.stringByDeletingLastPathComponent;
    for (MCTreeEntry *e in [ws treeListFolder:dir hidden:YES] ?: @[])
        if ([e.path isEqualToString:path]) {
            [self warn:@"A file with that name already exists."];
            return;
        }
    [ws markNew:path];
    [self openFileAtPath:path from:EditorTabs::OpenFrom::TreeReturn];
    [self focusEditor:nil];
    [self flashStatus:[NSString stringWithFormat:@"%@ is new: saving it makes it on %@",
                                                 path.lastPathComponent, self.sessionRemoteLabel]];
}

@end
