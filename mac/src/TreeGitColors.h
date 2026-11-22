// TreeGitColors.h — Objective-C++. The file tree's git colors for one
// window: one `git status` (Git::decorationArgs) per refresh on a queue of
// its own, read into Git::Decorations, which the tree asks per row, and
// into the status bar's branch item (Git::statusBranch). The
// rules are in the core (GitStatus.h); this runs git and keeps the answer.
//
// It looks when the folder is set, then after changes on disk, gathered
// over half a second, at most one status at a time; changes only inside
// what git ignores (build output, node_modules) or in .git's objects and
// logs need none. A folder outside any repository costs one rev-parse when
// set, and another only when its own top folder changes (a git init). git
// runs through MCGitRunSync, so it never writes the index (no FSEvents
// loop) and stops at the read limit.
#import <Cocoa/Cocoa.h>
#import "GitPanel.h"
#include "GitStatus.h"

@interface MCTreeGitColors : NSObject
// Called on the main thread when the colors changed.
@property(nonatomic, copy) void (^onChange)(void);
// The status bar's branch item from the same status (its --branch
// headers, Git::statusBranch), and a call on the main thread when it
// changed (to not shown, too, when the folder leaves a repository).
@property(nonatomic, copy) void (^onBranchChange)(void);
@property(nonatomic, readonly) Git::StatusBranch statusBranch;
// The window's folder, as the tree spells it (nil: none). Forgets the old
// folder's colors and looks at once.
- (void)setRoot:(NSString *)root;
// A window over SSH (GitPanel.h's MCGitRunner), before setRoot:: git runs
// on the other machine, and the folder is taken as the host spells it
// (resolved there), never resolved on this Mac's disk. changedOnDisk:
// then comes from the host's notices.
- (void)useGitRunner:(MCGitRunner)runner;
// FSEvents named these folders (as the disk spells them).
- (void)changedOnDisk:(NSArray<NSString *> *)paths;
// The host said git may have changed (over SSH): a status half a second
// from now, as for a change on disk.
- (void)lookSoon;
// A row's color, by its path as the tree spells it. None outside the
// repository, under the folder's .claude/worktrees (other repositories),
// and before the first status.
- (Git::Decoration)decorationForPath:(NSString *)path isDir:(BOOL)isDir;
// The window is closing: nothing more runs or arrives.
- (void)stop;
// For tests: statuses finished (each run of git status), and whether the
// folder is in a repository.
@property(nonatomic, readonly) NSUInteger runs;
@property(nonatomic, readonly) BOOL inRepository;
// How many paths the last status found changed (Git::Decorations'
// changeCount), 0 outside a repository and before the first status.
@property(nonatomic, readonly) NSUInteger changeCount;
@end
