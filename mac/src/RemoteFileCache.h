// RemoteFileCache.h — Objective-C++. A window over SSH's private folder
// of copies (docs/notes/session-host.md, "Viewers over SSH"): the files it
// shows in a viewer that reads a file itself (a picture, a PDF, a player,
// SQLite, Quick Look, a table too large to edit) and the pictures its
// Markdown pages show, read through its host and written here, so the
// viewer opens the copy while the tab keeps the remote path. The rules
// (names, places, the limit, refreshing) are the core's RemoteCache.h.
//
// The folder is <the app's temporary folder>/mutible-ssh-copies/w<pid>-<n>,
// made 0700 on the first copy (the folder of folders too), each copy 0600.
// It is removed when the window closes (removeAll) and, for a process that
// ended without closing it (a crash, a quit), at the next launch
// (removeLeftovers). Main thread, but removeLeftovers.
#pragma once
#import <Cocoa/Cocoa.h>

#include <string>

@interface MCRemoteFileCache : NSObject
// `remoteFolder`: the window's folder on the remote host, as its tabs spell
// their paths.
- (instancetype)initWithRemoteFolder:(NSString *)remoteFolder;
@property(nonatomic, readonly, copy) NSString *remoteFolder;
// This window's folder of copies (made on the first copy; nil until then).
@property(nonatomic, readonly, copy) NSString *folder;

// Where the copy of `remotePath` is; nil when the path is not the folder's
// (RemoteCache::placeOf) or nothing was copied yet.
- (NSString *)localPathOf:(NSString *)remotePath;
// Whether there is a copy of it, and whether the host's disk watching has
// said it may have changed since (refreshed when it is shown next).
- (BOOL)hasCopyOf:(NSString *)remotePath;
- (BOOL)isStale:(NSString *)remotePath;
// The copy written (its folders made 0700, the file 0600, through a
// temporary file renamed over it, so a viewer never reads half a file) and
// its fingerprint kept; nil when it could not be written.
- (NSString *)store:(const std::string &)bytes of:(NSString *)remotePath;
// Whether `bytes` are what the copy holds (its fingerprint).
- (BOOL)localCopyOf:(NSString *)remotePath holds:(const std::string &)bytes;
// The host saw these folders change: the copies in them are stale; returns
// the remote paths of those copies.
- (NSArray<NSString *> *)foldersChanged:(NSArray<NSString *> *)folders;
// A stale copy refreshed (or found the same): fresh again; or stale again.
- (void)markFresh:(NSString *)remotePath;
- (void)markStale:(NSString *)remotePath;
// Whether the host said the file's folder changed while it was being read
// (its read may have caught the file before that change): asked once, as
// the read ends, which then reads it again.
- (BOOL)takeAgain:(NSString *)remotePath;
// The folders of the files copied (remote paths), for the host to watch.
- (NSArray<NSString *> *)remoteFolders;
// The remote paths being read now (Link::fetchBytes), so one is never
// asked twice at once; those that could not be read (a Markdown picture
// not there), asked again once their folder changes; and how many copies
// were refreshed because their file changed there (for checks).
@property(nonatomic, readonly) NSMutableSet<NSString *> *fetching, *failed;
@property(nonatomic, assign) NSUInteger refreshes;
// Every copy removed, and the folder (the window closed).
- (void)removeAll;

// The folder of every window's folder of copies.
+ (NSString *)root;
// At launch, on any queue: the folders of processes that are not running
// any more (RemoteCache::ownerOfFolder), and nothing else.
+ (void)removeLeftovers;
@end
