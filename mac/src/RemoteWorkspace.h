// RemoteWorkspace.h — Objective-C++. A window over SSH's folder, read
// through the remote host (stage 8, docs/notes/session-host.md, "The
// window's tree, Quick Open and Find in Folder over SSH"), never through
// this Mac's disk:
//   - the tree's listings (MCTreeSource: FsRead "list", waited for, as the
//     Mac's own tree lists a folder as it opens), with the host's marks
//     (what the ignore files ignore, a link out of the folder); the
//     folders on the way to a file revealed (Quick Open) asked together,
//     one round trip for them all (treePrefetchFolders:hidden:);
//   - the folders the tree has listed, told to the host (FsRead "watch"),
//     whose listings come back by themselves when they change there
//     (FsChanged, onListings): the remote disk's notices in place of
//     FSEvents;
//   - Quick Open's list of the folder's files (FsRead "files") and Find in
//     Folder (FsRead "search"), both on the host's workers, answered as
//     they go (SessionSearch.h);
//   - the tree's file operations (rename, move, delete, a new file or
//     folder), done by the host (Client::requestFileOp), and Replace in
//     Folder's Replace All (the host's "replace");
//   - the files the window opened that are not there yet (the address's
//     new file, New File's on a host without file operations), which their
//     first save makes.
// Its link is the window's, moved by Reconnect (attachLink:). Main thread.
#pragma once
#import <Cocoa/Cocoa.h>

#include <vector>

#include "Json.h"
#import "FileTree.h"
#import "GitPanel.h"

@class MCSessionLinkRef;
@class MCFormatResult;

@interface MCRemoteWorkspace : NSObject <MCTreeSource>
- (instancetype)initWithFolder:(NSString *)folder;
@property(nonatomic, readonly, copy) NSString *folder;
// The window's link, now or again after Reconnect: its disk notices heard,
// and the folders open in the tree told to the new host.
- (void)attachLink:(MCSessionLinkRef *)link;
// The window closes: nothing more is heard or asked.
- (void)detach;

// Each change the host saw: the listings of the folders it named, by path
// (each MCTreeList-shaped), on the main queue's next turn.
@property(nonatomic, copy) void (^onListings)(NSDictionary<NSString *, NSArray<MCTreeEntry *> *> *listings);
// The folders the tree has listed (absolute, under the folder), with
// dotfiles when `hidden`: the host watches them. Sent when it differs from
// what was sent last (and again on a new link).
- (void)watchFolders:(NSArray<NSString *> *)paths hidden:(BOOL)hidden;
// The window's: a watch is to be sent on the main queue's next turn.
@property(nonatomic, assign) BOOL watchQueued;

// Quick Open's list: the folder's files from it ('/'-separated), as the
// Mac's own walk lists them, dotfiles too. `done` on the main thread once
// (an empty list when the host cannot be asked).
- (void)listFiles:(void (^)(NSArray<NSString *> *files, BOOL truncated))done;
// Find in Folder on the host (a "search" request, SessionSearch.h): each
// batch of answers as it comes, `done` with the last. 0, calling nothing,
// when the host cannot be asked.
- (uint64_t)search:(Json)request answers:(void (^)(const std::vector<Json> &answers, BOOL done))answers;
- (void)cancelSearch:(uint64_t)req;

// Replace in Folder on the host (SessionSearch.h's "replace", the rows the
// search showed): its answer as a stream's. 0, calling nothing, when the
// host cannot be asked or does not replace (replacesInFolder).
- (BOOL)replacesInFolder;
- (uint64_t)replace:(Json)request answers:(void (^)(const std::vector<Json> &answers, BOOL done))answers;

// The tree's file operations on the host (Welcome flag 64; paths absolute,
// under the folder). Each waits for the host's answer and returns nil when
// it was done, else the sentence why not (the host's refusal, or that the
// host did not answer). `reason` gets the host's reason ("exists", ...),
// "" when done. The tree hears the change from the host (FsChanged).
@property(nonatomic, readonly) BOOL doesFileOps;
- (NSString *)renamePath:(NSString *)path to:(NSString *)name reason:(NSString **)reason;
- (NSString *)movePath:(NSString *)path into:(NSString *)folder reason:(NSString **)reason;
// Permanently: the host has no Trash, so the window asks first.
- (NSString *)deletePath:(NSString *)path reason:(NSString **)reason;
- (NSString *)makePath:(NSString *)path folder:(BOOL)folder reason:(NSString **)reason;

// git for the window (stage 9, Welcome flag 256, SessionGit.h): a runner
// for MCGitSetQueueRunner (GitPanel.h), which the Source Control panel, the
// change bars and the tree's git colors put on their queues. Called on
// such a queue, it asks the host on the main queue (Link::git) and waits
// for the answer there, never on the main thread; with a host too old for
// it, or the connection lost, its result is status -1 with the sentence
// saying so (Shared::gitTooOldText, gitLostText). Fetch, pull and push
// (stage 9's third slice) go the same way: the host runs them with that
// machine's own credentials and never a prompt (SessionGit.h).
@property(nonatomic, readonly) MCGitRunner gitRunner;
// The work tree's files for the review pages and comparisons (GitPanel.h's
// MCGitWorkReader): on such a queue, a file under the folder read through
// the host (Link::fetchBytes) and waited for there; nil when it cannot be
// read, the connection is lost, or it is called on the main thread.
@property(nonatomic, readonly) MCGitWorkReader workReader;
// The host said Source Control may have changed (FsChanged's "git": a
// write in the folder, or in .git's index, HEAD or refs).
@property(nonatomic, copy) void (^onGitChanged)(void);

// Format Document for the window (stage 9, Welcome flag 1024,
// SessionFormat.h): `text`, the tab's text at `path` (the remote's), sent
// to the host, which plans the formatter on its own disk with `setting`
// (format.<lang>) and `consent` (the folder's answer, kept per host and
// folder), runs it there in the file's folder, and answers. `done` once,
// on the main thread, with the result as MCFormatText gives one: the text,
// or the note (a host too old, the connection lost, a formatter missing
// there named with the host and no install commands, the folder's own
// program waiting for an answer: needsConsent).
- (void)formatText:(NSString *)text
              path:(NSString *)path
           setting:(NSString *)setting
           consent:(NSString *)consent
         firstLine:(NSInteger)firstLine
          lastLine:(NSInteger)lastLine
       projectOnly:(BOOL)projectOnly
           timeout:(NSTimeInterval)timeout
              done:(void (^)(MCFormatResult *result))done;

// Files opened here that are not on the host yet: shown empty, made by
// their first save.
- (void)markNew:(NSString *)path;
- (BOOL)isNew:(NSString *)path;

// For tests: folders listed through the host, its notices taken, Quick
// Open lists and searches asked for.
@property(nonatomic, readonly) NSUInteger listings, notices, fileLists, searches, fileOps, replaces;
// git commands the host ran for the window, and its notices that git may
// have changed.
@property(nonatomic, readonly) NSUInteger gitRuns, gitNotices;
// Work tree files read through the host for git's pages (workReader).
@property(nonatomic, readonly) NSUInteger workReads;
// Formats the host was asked for.
@property(nonatomic, readonly) NSUInteger formats;
@end
