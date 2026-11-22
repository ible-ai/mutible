// RemoteSshSuite.mm — `make guitest SUITES=remote_ssh`: a window over SSH
// (stage 8, docs/notes/session-host.md, "The Mac over SSH"), with a
// stand-in for ssh. MUTIBLE_SSH names a script that records its arguments,
// skips ssh's options to the "--", takes the host, and runs the remote
// command as ssh's remote shell would, with this checkout's mutible-cli as
// `mutible` and a variable of its own exported (so what the shell sees is
// the "remote" environment, not the Mac's). The folder it serves is a
// scratch folder of this Mac's, but the window reads, writes and stamps
// nothing of it itself: every read and write goes through the host.
//   - Connecting off the main thread, with the stand-in slow (it sleeps
//     before it starts the host): `ssh://standin/<folder>/notes.txt`
//     through application:openURLs: shows the connecting window at once
//     (no editor window yet), its line "Connecting to standin…", while a
//     timer goes on firing; Esc (its Cancel) kills the stand-in by its pid,
//     and nothing opens and nothing is asked. With the stand-in refusing,
//     the alert says why on the connecting window, which then closes.
//   - Let through after a 2 s sleep: the same address again while it
//     connects is that connect's (one window, one ssh), and src/util.c's
//     waits for it; the line goes to "Starting mutible on standin…" (the
//     host's word on ssh's standard error) and "Opening <folder> on
//     standin…", then the editor window opens as before and util.c's
//     address opens in it through its host, no ssh run for it.
//   - `ssh://standin/<folder>/notes.txt` (as the launcher's request
//     delivers it): one window, on a remote link,
//     its title "notes.txt — standin:<folder> — mutible", the file's tab
//     with its text, clean; ssh run with BatchMode, no pty, `--` before
//     the host, and `mutible host --stdio '<file>'` as the remote command.
//   - Typing and Cmd+S: the host writes the file (the tab clean, the text
//     on disk), and the window's own write never runs (a remote tab has no
//     stamp, and the window refuses to write one itself).
//   - The tree lists the folder through the host (folders first, what
//     .gitignore ignores marked; this Mac's MCTreeList never lists it), a
//     folder opens through it, files made on disk there come into the tree
//     by the host's notices, and a row opens its file through the host
//     waiting on it once at most (Client::waits: the read's own answer,
//     no Ping behind it). Quick Open's open of a file two folders down
//     that the tree has not listed waits twice: both folders' listings
//     asked together, then the read.
//   - Viewers through copies (RemoteCache.h, RemoteFileCache.h): a PNG and
//     a PDF made on disk there open from the tree in the picture and PDF
//     viewers (its pixel size, its page count), read with one wait on the
//     host, from copies in the window's private folder (0700, the copy
//     0600), while the tab, the title and the tree's selection keep the
//     remote path; the PNG rewritten there at another size is shown again
//     by the host's notice; a file over 64 MB shows the placeholder, no
//     alert and no copy; the folder of copies is gone once the window
//     closes.
//   - The tree's file operations, done by the host (Welcome flag 64) and
//     checked on the stand-in's disk: Shift+Cmd+N in the tree makes a
//     folder and New File a file (opened in a tab); Rename (its prompt
//     answered) renames a file whose tab has unsaved text, which follows
//     it, its text kept, and saves to the new name; renaming onto a name
//     there already is refused with the host's sentence in an alert; a row
//     dragged onto a folder (validateDrop's Move, acceptDrop) moves it, its
//     tab following, and no file URL of this Mac's is on the drag; Delete
//     asks "Delete ... permanently?" (no Trash there: Cancel keeps it), then
//     deletes, the tab staying, marked deleted.
//   - Cmd+P lists the host's files (the ignored one left out) and opens one;
//     Find in Folder searches on the host and its rows open their files;
//     Replace All replaces on the host: a closed file rewritten on disk, a
//     clean tab edited there (unsaved, the disk as it was), a tab with
//     unsaved text left alone and named, in the local question's words.
//   - Addresses the open window's folder holds, as its host resolves them:
//     src/util.c's opens in that window through its host (no new window, no
//     ssh run), main.c's through a link to the folder and by ".." lands on
//     its tab, and the folder's own address brings the window to front.
//   - `ssh://standin/<folder>/src/fresh.txt`, a file not there yet: an
//     empty untitled tab of the open window at that path, which Cmd+S makes
//     through its host.
//   - `ssh://other/...` (another machine) and a file outside the folder:
//     each a window of its own on the file's folder, closed with its host;
//     the first window shows notes.txt again.
//   - A terminal: its shell is the host's child (`ps` of its parent says
//     mutible) in the host's environment (the stand-in's variable). The
//     host is the folder's lasting one, which `--stdio` started detached
//     and relays to (its pid is not the stand-in's).
//   - The remote's own shell integration (the stand-in's SHELL is zsh; the
//     host sets up build/'s copy of the script, Welcome flag 128): a marked
//     prompt, the nonce the view's and kept under the remote link's key,
//     the harness's ZDOTDIR and no MUTIBLE_NONCE in the shell; `sleep 5`
//     busy with its command named, `false` queued meanwhile, then statuses
//     0 and 1 by the marks; a local terminal on the app's own host beside
//     it with a nonce of its own under its own key.
//   - A remote whose login shell is bash (the stand-in's shell file names
//     Homebrew's bash 4.4 or later; skipped without one): a window on a
//     folder of its own, its terminal's marks believed as a bash's, the
//     nonce and MUTIBLE_BASH_LOGIN out of its environment, `sleep 5` busy,
//     `false` queued then status 1, a half-typed line set aside while the
//     app runs a command and given back at the next prompt (the pop key),
//     `sh run.sh` run as Run File types it, the host ending with the window.
//   - The connection drops (the stand-in's process group killed: the relay
//     only), with the stand-in refusing to connect for a while: the window
//     says it is reconnecting (its title, no Reconnect button), the text
//     stays and takes more typing, and its tries fail. Once the stand-in
//     connects again, the window takes up the same host by itself: the
//     terminal under its id, its earlier output replayed and the program
//     it ran still running, its marks still believed (then Ctrl+C, the
//     prompt again, and the same shell's pid); the
//     document with everything typed, unsaved, the host's text the tab's,
//     and undo going back past the drop into what the host recorded
//     before it, then redo; Cmd+S saves it there; the tree follows the
//     host's notices.
//   - The window closes: its relay ends, and the lasting host, its
//     terminal and document closed with the window, ends after its idle
//     time (MUTIBLE_HOST_IDLE_MINUTES, a few seconds here).
#import <Carbon/Carbon.h>
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "FileTree.h"
#import "PalettePanel.h"
#import "RemoteConnect.h"
#import "RemoteFileCache.h"
#import "RemoteWorkspace.h"
#import <Quartz/Quartz.h>
#import "Search.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"

#include "HostIntegration.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <string>

// A drag of the tree's own rows, as validateDrop: and acceptDrop: read it
// (its source; a drag from the tree reads no pasteboard).
@interface MCRemoteTreeDrag : NSObject
@property(nonatomic, weak) id draggingSource;
@end
@implementation MCRemoteTreeDrag
@end

namespace {

NSString *gFolder, *gFile, *gSsh, *gArgs, *gPidFile, *gBlock, *gTries, *gSlow;
NSString *gOps;              // the folder the tree's file operations work in
NSUInteger gAlertsAt;        // the alerts before a step's own
MCRemoteConnect *gConnect;   // a connect under way
NSTimer *gTicker;            // fires while it connects: the main thread turns
NSUInteger gTicks, gTicksAtStart;
using Stage = Shared::ConnectProgress::Stage;
EditorController *gC, *gFreshWindow;
TerminalView *gTerm;
pid_t gHostA, gHostB, gHostFresh;
pid_t gLasting;              // the folder's lasting host, the terminal's shell's parent
NSString *gShellPid;         // that shell's pid, as it said it
unsigned long long gTermId;  // the terminal's id on the host
NSUInteger gTriesBefore;
NSString *gShellFile;        // names the stand-in's login shell when not empty (zsh when none)
NSString *gBash;             // a bash 4.4 or later on this Mac, nil when none
NSString *gBashFolder;       // the folder its window is on
EditorController *gBashC;
TerminalView *gBashTerm;
pid_t gBashHost;             // that folder's lasting host, the bash's parent
NSString *gRemoteNonce;      // the remote terminal's integration nonce, this view's
int gEndedBefore;            // commands the remote terminal's marks ended before a step's
EditorController *gLocalC;   // a local window's terminal on its own host, beside it
TerminalView *gLocalTerm;
NSUInteger gWindowsBefore;   // the windows before an address the open one holds
NSString *gPng, *gPdf, *gHuge;   // files the viewers show through copies
NSString *gCopies;               // the window's folder of copies
NSUInteger gRefreshes;           // its copies refreshed before a change there

NSString *RealPath(NSString *path) {
    char buf[PATH_MAX];
    return realpath(path.fileSystemRepresentation, buf) ? @(buf) : path;
}

NSString *Read(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
}

EditorController *RemoteWindow() {
    for (EditorController *c in MCGuiControllers())
        if (c.sessionIsRemote) return c;
    return nil;
}

EditorController *RemoteWindowOn(NSString *folder) {
    for (EditorController *c in MCGuiControllers())
        if (c.sessionIsRemote && [c.rootPath isEqualToString:folder]) return c;
    return nil;
}

MCTab *TabFor(EditorController *c, NSString *path) {
    for (MCTab *t in [c valueForKey:@"tabs"])
        if ([t.currentPath isEqualToString:path]) return t;
    return nil;
}

// The tree's rows: every row's item, and the names of those at the top
// level (dotfiles left out: an earlier suite may have shown them).
NSArray<FileItem *> *TreeRows(EditorController *c) {
    NSMutableArray<FileItem *> *rows = [NSMutableArray array];
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++) {
        id item = [c.outline itemAtRow:r];
        if ([item isKindOfClass:FileItem.class]) [rows addObject:item];
    }
    return rows;
}
NSArray<NSString *> *TopNames(EditorController *c) {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (FileItem *f in TreeRows(c))
        if ([c.outline levelForItem:f] == 0 && ![f.name hasPrefix:@"."]) [names addObject:f.name];
    return names;
}
FileItem *TreeRow(EditorController *c, NSString *path) {
    for (FileItem *f in TreeRows(c))
        if ([f.path isEqualToString:path]) return f;
    return nil;
}
NSView *Palette(EditorController *c) { return MCCommandPaletteIn(c.window); }
NSTextView *PaletteEditor(EditorController *c) { return (NSTextView *)MCCommandPaletteField(Palette(c)).currentEditor; }

void TypeAtEnd(EditorController *c, NSString *s) {
    NSTextView *tv = [c valueForKey:@"textView"];
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(tv.string.length, 0)]; });
}

// How many times the window's client has waited for its host (Client::
// waits): each wait is a round trip on the main thread, over SSH the
// network's.
unsigned long long HostWaits(EditorController *c) {
    MCSessionLinkRef *ref = [c sessionRemoteLink];
    return ref.link ? (unsigned long long)ref.link->client().waits() : 0;
}
unsigned long long gWaitsBefore;

// How many times the stand-in refused to connect (gBlock there).
NSUInteger RefusedTries() {
    NSString *t = Read(gTries);
    return t.length ? [t componentsSeparatedByString:@"\n"].count - 1 : 0;
}

// What follows the last `prefix` on a terminal's screen that a digit
// follows (output, not the command line that printed it), up to the next
// space or line's end; "" when there is none.
NSString *ScreenWordIn(TerminalView *term, NSString *prefix) {
    NSString *text = [term recentText:60];
    NSRange within = NSMakeRange(0, text.length);
    for (;;) {
        NSRange r = [text rangeOfString:prefix options:NSBackwardsSearch range:within];
        if (r.location == NSNotFound) return @"";
        const NSUInteger at = NSMaxRange(r);
        if (at < text.length && [NSCharacterSet.decimalDigitCharacterSet characterIsMember:[text characterAtIndex:at]]) {
            NSScanner *sc = [NSScanner scannerWithString:[text substringFromIndex:at]];
            sc.charactersToBeSkipped = nil;
            NSString *word = nil;
            [sc scanUpToCharactersFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet intoString:&word];
            return word ?: @"";
        }
        within = NSMakeRange(0, r.location);
    }
}

NSString *ScreenWord(NSString *prefix) { return ScreenWordIn(gTerm, prefix); }

// A picture of `w` by `h` pixels, written as a PNG (through a temporary
// file renamed over it, as an editor saves).
BOOL WritePng(NSString *path, NSInteger w, NSInteger h) {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
                                                                 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                                                                      isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:0 bitsPerPixel:0];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

// A PDF of `pages` pages.
BOOL WritePdf(NSString *path, size_t pages) {
    NSMutableData *data = [NSMutableData data];
    CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData((__bridge CFMutableDataRef)data);
    CGRect box = CGRectMake(0, 0, 200, 300);
    CGContextRef ctx = CGPDFContextCreate(consumer, &box, NULL);
    for (size_t i = 0; i < pages; i++) {
        CGPDFContextBeginPage(ctx, NULL);
        CGContextFillRect(ctx, CGRectMake(10, 10, 20 + 10 * i, 20));
        CGPDFContextEndPage(ctx);
    }
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    CGDataConsumerRelease(consumer);
    return data.length > 0 && [data writeToFile:path atomically:YES];
}

int ModeOf(NSString *path) {
    struct stat st;
    return path && stat(path.fileSystemRepresentation, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

// The stand-in's pid, as it wrote it (the process group ssh's spawn made).
pid_t StandInPid() { return (pid_t)Read(gPidFile).intValue; }

BOOL Gone(pid_t pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }

// The drag's session, which the tree's delegate methods do not read.
NSDraggingSession *NoSession() {
    static NSDraggingSession *none;
    return none;
}

BOOL IsDir(NSString *path) {
    BOOL dir = NO;
    return [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&dir] && dir;
}
BOOL Exists(NSString *path) { return [NSFileManager.defaultManager fileExistsAtPath:path]; }
NSString *Op(NSString *name) { return [gOps stringByAppendingPathComponent:name]; }

// The tree's row for `path` selected, as a click leaves it.
BOOL SelectRow(EditorController *c, NSString *path) {
    const NSInteger row = [c.outline rowForItem:TreeRow(c, path)];
    if (row < 0) return NO;
    [c.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    return YES;
}

// The alerts shown since `gAlertsAt`.
NSArray<NSString *> *AlertsSinceStep() {
    NSArray<NSString *> *all = MCGuiAlertsShown();
    return all.count > gAlertsAt ? [all subarrayWithRange:NSMakeRange(gAlertsAt, all.count - gAlertsAt)] : @[];
}

// The alerts shown since this suite began: MCGuiAlertsShown is every alert
// of the run so far, the suites before this one's included (CI, #314's
// first run: fourteen of theirs, none of this suite's).
NSUInteger gAlertsBefore;
NSArray<NSString *> *NewAlerts() {
    NSArray<NSString *> *all = MCGuiAlertsShown();
    return all.count > gAlertsBefore ? [all subarrayWithRange:NSMakeRange(gAlertsBefore, all.count - gAlertsBefore)]
                                     : @[];
}

// Commands a terminal's marks say ended, and the last that ran (its status).
int Ended(TerminalView *t) {
    TerminalGridView *g = [t valueForKey:@"grid"];
    int n = 0;
    for (const TermCommand &c : g.screen->commands()) n += c.ended;
    return n;
}
TermCommand LastEnded(TerminalView *t) {
    TerminalGridView *g = [t valueForKey:@"grid"];
    TermCommand last{};
    for (const TermCommand &c : g.screen->commands())
        if (c.ended) last = c;
    return last;
}
NSString *LinkKey(MCSessionLinkRef *ref) { return ref.link ? @(ref.link->key().c_str()) : @""; }

// A bash 4.4 or later (the bash script's marks need PS0): the Mac's
// /bin/bash is 3.2, so Homebrew's; nil when there is none.
NSString *FindBash() {
    for (NSString *b in @[ @"/opt/homebrew/bin/bash", @"/usr/local/bin/bash", @"/bin/bash" ])
        if ([NSFileManager.defaultManager isExecutableFileAtPath:b] && Shared::bashVersion(b.UTF8String) >= 404)
            return b;
    return nil;
}

// What follows the last "pushed-6" on a terminal's screen.
NSString *AfterPushed(TerminalView *t) {
    NSString *text = [t recentText:30];
    NSRange r = [text rangeOfString:@"pushed-6" options:NSBackwardsSearch];
    return r.location == NSNotFound ? @"" : [text substringFromIndex:NSMaxRange(r)];
}

// `command` typed into the terminal until `expect` shows on its screen,
// again every 3 s: a shell starting may drop what came before it (and,
// before the remote's integration, no mark said its line editor was
// ready).
void RunUntil(NSString *what, NSString *command, NSString *expect) {
    __block double sent = 0;
    MCGuiWait(what, 30, ^BOOL {
        if ([[gTerm recentText:30] containsString:expect]) return YES;
        if (MCGuiNowMs() - sent > 3000) {
            sent = MCGuiNowMs();
            [gTerm runCommand:command];
        }
        return NO;
    });
}

}  // namespace

MC_GUI_SUITE(remote_ssh) {
    MCGuiStep(@"a folder for the remote, and a stand-in for ssh", ^{
        gAlertsBefore = MCGuiAlertsShown().count;
        // The session-host setting stays off: a window over SSH needs none.
        MCGuiSetSettings(@"lsp.enabled = false\n");
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *scratch = MCGuiScratch();
        NSString *made = [scratch stringByAppendingPathComponent:@"remote-proj"];
        [fm createDirectoryAtPath:made withIntermediateDirectories:YES attributes:nil error:nil];
        gFolder = RealPath(made);   // as the host names it
        gFile = [gFolder stringByAppendingPathComponent:@"notes.txt"];
        MCGuiWriteFile(gFile, @"first line\n");
        // What the tree, Quick Open and Find in Folder find there.
        [fm createDirectoryAtPath:[gFolder stringByAppendingPathComponent:@"src"]
            withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/main.c"], @"int needle = 1;\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/util.c"], @"// a needle here\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@".gitignore"], @"*.log\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"debug.log"], @"a needle in a log\n");
        NSString *bin = [scratch stringByAppendingPathComponent:@"ssh-bin"];
        [fm createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *cli = [MCGuiBuildDir() stringByAppendingPathComponent:@"mutible-cli"];
        GCHECK([fm isExecutableFileAtPath:cli]);
        [fm createSymbolicLinkAtPath:[bin stringByAppendingPathComponent:@"mutible"] withDestinationPath:cli error:nil];
        gArgs = [scratch stringByAppendingPathComponent:@"ssh-args.txt"];
        gPidFile = [scratch stringByAppendingPathComponent:@"ssh-pid.txt"];
        gBlock = [scratch stringByAppendingPathComponent:@"ssh-refuse"];
        gTries = [scratch stringByAppendingPathComponent:@"ssh-refused.txt"];
        gSlow = [scratch stringByAppendingPathComponent:@"ssh-slow"];
        gSsh = [scratch stringByAppendingPathComponent:@"ssh-stand-in"];
        gShellFile = [scratch stringByAppendingPathComponent:@"ssh-shell"];
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"# A stand-in for ssh (RemoteSshSuite.mm): its arguments recorded,\n"
            @"# its options skipped to the --, then the host, then the remote\n"
            @"# command run as ssh's remote shell runs it.\n"
            @"printf '%%s\\n' \"$@\" > '%@'\n"
            @"# While the suite's file is there, it fails as ssh does when the\n"
            @"# machine refuses the connection.\n"
            @"if [ -e '%@' ]; then\n"
            @"  echo refused >> '%@'\n"
            @"  echo 'ssh: connect to host standin port 22: Connection refused' >&2\n"
            @"  exit 255\n"
            @"fi\n"
            @"echo $$ > '%@'\n"
            @"# While the suite's slow file holds a number, ssh takes that many\n"
            @"# seconds to connect.\n"
            @"if [ -s '%@' ]; then sleep \"$(cat '%@')\"; fi\n"
            @"while [ $# -gt 0 ] && [ \"$1\" != -- ]; do shift; done\n"
            @"shift\n"
            @"shift\n"
            @"SSH_STANDIN=yes\n"
            @"export SSH_STANDIN\n"
            @"# The login shell sshd would name: zsh, or the one the suite's\n"
            @"# shell file names (a bash), whose integration the remote's host\n"
            @"# sets up from its own scripts (build/'s copies).\n"
            @"if [ -s '%@' ]; then SHELL=\"$(cat '%@')\"; else SHELL=/bin/zsh; fi\n"
            @"export SHELL\n"
            @"PATH='%@':$PATH\n"
            @"export PATH\n"
            @"exec /bin/sh -c \"$*\"\n",
            gArgs, gBlock, gTries, gPidFile, gSlow, gSlow, gShellFile, gShellFile, bin];
        MCGuiWriteFile(gSsh, script);
        chmod(gSsh.fileSystemRepresentation, 0755);
        MCSetEnv("MUTIBLE_SSH", gSsh.fileSystemRepresentation);
        // The lasting hosts this suite starts end a few seconds after their
        // last window, not four hours (each is busy, and stays, while a
        // document is unsaved or a program runs).
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", "0.05");
    });
    // ---- connecting off the main thread
    MCGuiStep(@"ssh://standin/<folder>/notes.txt with a slow ssh: the connecting window, at once", ^{
        MCGuiWriteFile(gSlow, @"30\n");
        [NSFileManager.defaultManager removeItemAtPath:gPidFile error:nil];
        gWindowsBefore = MCGuiControllers().count;
        gTicks = 0;
        gTicker = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *) { gTicks++; }];
        [NSRunLoop.mainRunLoop addTimer:gTicker forMode:NSRunLoopCommonModes];
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gFile]];
        GCHECK(url != nil);
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        // Returned without waiting for ssh: no editor window yet, the
        // connecting one instead.
        GCHECK(RemoteWindow() == nil && MCGuiControllers().count == gWindowsBefore);
        GCHECK(MCRemoteConnect.pending.count == 1);
        gConnect = MCRemoteConnect.pending.firstObject;
        GCHECK(gConnect != nil && gConnect.window != nil);
        GCHECK(MCGuiWindowMadeKey() == gConnect.window);   // shown, in front
        GCHECK_EQ_STR(gConnect.progressLine, @"Connecting to standin…");
        GCHECK_EQ_STR(gConnect.window.title, ([NSString stringWithFormat:@"standin:%@ — mutible", gFile]));
        gTicksAtStart = gTicks;
    });
    MCGuiWait(@"ssh started (the stand-in sleeping), and the main thread turns meanwhile", 15, ^BOOL {
        return StandInPid() > 0 && gConnect.commandPid == StandInPid() && gTicks >= gTicksAtStart + 5;
    });
    MCGuiStep(@"Esc cancels: the stand-in killed by its pid", ^{
        gHostA = StandInPid();
        GCHECK(!Gone(gHostA) && RemoteWindow() == nil);
        GCHECK_EQ_STR(gConnect.progressLine, @"Connecting to standin…");
        GCHECK(gConnect.progress.stage() == Stage::Connecting);
        // Esc in the connecting window, the key one (its Cancel's key).
        MCGuiSetKeyWindow(gConnect.window);
        const BOOL took = MCGuiPerformMenuKey(gConnect.window, 53, 0, @"\033", @"\033");
        MCGuiSetKeyWindow(nil);
        GCHECK(took);
        if (!took) [gConnect cancel:nil];
        GCHECK(gConnect.progress.stage() == Stage::Cancelled);
        GCHECK(MCRemoteConnect.pending.count == 0);
    });
    MCGuiWait(@"the stand-in gone", 10, ^BOOL { return Gone(gHostA); });
    MCGuiStep(@"no window, no alert; then the stand-in refusing", ^{
        GCHECK(RemoteWindow() == nil && MCGuiControllers().count == gWindowsBefore);
        GCHECK(NewAlerts().count == 0);
        gHostA = 0;
        // The failure's alert, on the connecting window.
        [NSFileManager.defaultManager removeItemAtPath:gSlow error:nil];
        MCGuiWriteFile(gBlock, @"");
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        gConnect = MCRemoteConnect.pending.firstObject;
        GCHECK(gConnect != nil);
    });
    MCGuiWait(@"it could not connect", 15, ^BOOL { return MCRemoteConnect.pending.count == 0; });
    MCGuiStep(@"the alert says why, on the connecting window, which closed", ^{
        NSArray<NSString *> *alerts = NewAlerts();
        MCGuiNote(@"alerts: %@ (%@)", alerts, MCGuiLastAlertInformative());
        GCHECK(alerts.count == 1);
        GCHECK_EQ_STR(alerts.lastObject, ([NSString stringWithFormat:@"Could not open ssh://standin%@", gFolder]));
        GCHECK([MCGuiLastAlertInformative() containsString:@"Connection refused"]);
        GCHECK(MCGuiLastSheetWindow() == gConnect.window);
        GCHECK(gConnect.progress.stage() == Stage::Failed);
        GCHECK(RemoteWindow() == nil);
        [NSFileManager.defaultManager removeItemAtPath:gBlock error:nil];
        gAlertsBefore = MCGuiAlertsShown().count;
    });
    MCGuiStep(@"slow again (2 s): the same address and another file while it connects wait for it", ^{
        MCGuiWriteFile(gSlow, @"2\n");
        [NSFileManager.defaultManager removeItemAtPath:gPidFile error:nil];
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gFile]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        gConnect = MCRemoteConnect.pending.firstObject;
        GCHECK(gConnect != nil && RemoteWindow() == nil);
        gTicksAtStart = gTicks;
        // The same address is this connect's; util.c's (the same machine)
        // waits, since the window it opens may hold it.
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        NSURL *util = [NSURL URLWithString:[NSString stringWithFormat:@"ssh://standin%@/src/util.c", gFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ util ]];
        GCHECK(MCRemoteConnect.pending.count == 1);
        GCHECK(gConnect.queued.count == 1 && [gConnect.queued.firstObject hasSuffix:@"/src/util.c"]);
        GCHECK(NewAlerts().count == 0);
    });
    MCGuiWait(@"mutible starting there, after the stand-in's sleep", 20, ^BOOL {
        return gConnect.progress.passed(Stage::Starting) || gConnect.progress.ended();
    });
    MCGuiStep(@"the main thread turned all along", ^{
        MCGuiNote(@"timer fires while connecting: %lu; lines: %@", (unsigned long)(gTicks - gTicksAtStart),
                  gConnect.linesShown);
        GCHECK(gTicks >= gTicksAtStart + 10);   // 2 s of sleep, a fire each 50 ms
        GCHECK([gConnect.linesShown containsObject:@"Connecting to standin…"]);
    });
    MCGuiWait(@"connected: the editor window, the connecting one gone", 20, ^BOOL {
        return RemoteWindow() != nil && MCRemoteConnect.pending.count == 0;
    });
    MCGuiStep(@"opened as before", ^{
        [gTicker invalidate];
        gTicker = nil;
        MCGuiNote(@"lines: %@", gConnect.linesShown);
        GCHECK(gConnect.progress.stage() == Stage::Open);
        GCHECK(gConnect.progress.passed(Stage::Starting) && gConnect.progress.passed(Stage::Opening));
        GCHECK([gConnect.linesShown
            containsObject:([NSString stringWithFormat:@"Opening %@ on standin…", gFolder])]);
        gConnect = nil;
        [NSFileManager.defaultManager removeItemAtPath:gSlow error:nil];
        gC = RemoteWindow();
        GCHECK(gC != nil);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@ (%@)", NewAlerts(), MCGuiLastAlertInformative());
        GCHECK(NewAlerts().count == 0);
        GCHECK(MCGuiControllers().count == gWindowsBefore + 1);   // one window for the three addresses
        if (!gC) return;
        gHostA = StandInPid();
        GCHECK(gHostA > 0);
        // util.c's address, which waited, opened in it through its host.
        NSString *util = [gFolder stringByAppendingPathComponent:@"src/util.c"];
        GCHECK_EQ_STR(gC.currentPath, util);
        GCHECK_EQ_STR(TabFor(gC, util).currentText, @"// a needle here\n");
        GCHECK([gC showFile:gFile]);
        GCHECK([gC.rootPath isEqualToString:gFolder]);
        NSString *label = [@"standin:" stringByAppendingString:gFolder];
        GCHECK_EQ_STR(gC.sessionRemoteLabel, label);
        MCTab *t = TabFor(gC, gFile);
        GCHECK(t != nil && t.session != nil);
        GCHECK_EQ_STR(t.currentText, @"first line\n");
        GCHECK(!t.dirty);
        GCHECK(t.session.hostSavesFile);
        GCHECK(t.fileModDate == nil);   // no stamp of this Mac's file at that path
        GCHECK_EQ_STR(gC.window.title, ([NSString stringWithFormat:@"notes.txt — %@ — mutible", label]));
        // How ssh was run.
        NSArray<NSString *> *args = [Read(gArgs) componentsSeparatedByString:@"\n"];
        GCHECK([args containsObject:@"-T"] && [args containsObject:@"BatchMode=yes"]);
        const NSUInteger dash = [args indexOfObject:@"--"];
        GCHECK(dash != NSNotFound && dash + 2 < args.count);
        if (dash != NSNotFound && dash + 2 < args.count) {
            GCHECK_EQ_STR(args[dash + 1], @"standin");
            GCHECK_EQ_STR(args[dash + 2], ([NSString stringWithFormat:@"mutible host --stdio '%@'", gFile]));
        }
    });
    MCGuiStep(@"typing and Cmd+S: the host writes the file", ^{
        if (!gC) return;
        TypeAtEnd(gC, @"typed over ssh\n");
        GCHECK(TabFor(gC, gFile).dirty);
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"on disk, written by the host", 10, ^BOOL {
        return [Read(gFile) isEqualToString:@"first line\ntyped over ssh\n"] && !TabFor(gC, gFile).dirty;
    });
    // ---- the tree through the host
    MCGuiStep(@"the tree lists the remote folder through its host", ^{
        if (!gC) return;
        MCRemoteWorkspace *ws = [gC remoteWorkspace];
        GCHECK(ws != nil && ws.listings > 0);
        NSArray<NSString *> *top = TopNames(gC);
        MCGuiNote(@"the tree's top: %@", top);
        GCHECK([top isEqualToArray:(@[ @"src", @"debug.log", @"notes.txt" ])]);   // folders first
        FileItem *log = TreeRow(gC, [gFolder stringByAppendingPathComponent:@"debug.log"]);
        GCHECK(log != nil && log.ignored && log.source == ws);   // .gitignore's, marked by the host
        GCHECK(!TreeRow(gC, gFile).ignored);
        // Never listed from this Mac's disk, though the stand-in's folder is on it.
        for (NSString *listed in MCTreeListedFolders()) GCHECK(![listed hasPrefix:gFolder]);
        FileItem *src = TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src"]);
        GCHECK(src != nil && src.isDir);
        if (src) [gC.outline expandItem:src];
        GCHECK(TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/main.c"]) != nil);
        GCHECK(TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/util.c"]) != nil);
    });
    // The step after: the folders the tree opened are told to the host on
    // the main queue's next turn.
    MCGuiStep(@"files made on disk there", ^{
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"later.txt"], @"made on disk\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/made-there.c"], @"int there;\n");
        // A file two folders down that the tree has not listed (Quick Open
        // into it, below).
        [NSFileManager.defaultManager createDirectoryAtPath:[gFolder stringByAppendingPathComponent:@"lib/inner"]
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"lib/inner/deep.c"], @"int deep;\n");
    });
    MCGuiWait(@"files made on disk appear in the tree", 15, ^BOOL {
        return TreeRow(gC, [gFolder stringByAppendingPathComponent:@"later.txt"]) != nil &&
               TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/made-there.c"]) != nil &&
               TreeRow(gC, [gFolder stringByAppendingPathComponent:@"lib"]) != nil;
    });
    MCGuiStep(@"Quick Open's open of a file in folders not listed yet: their listings in one round trip", ^{
        NSString *lib = [gFolder stringByAppendingPathComponent:@"lib"];
        NSString *deep = [gFolder stringByAppendingPathComponent:@"lib/inner/deep.c"];
        GCHECK(!TreeRow(gC, lib).loaded && TabFor(gC, deep) == nil);
        const NSUInteger listingsBefore = [gC remoteWorkspace].listings;
        const unsigned long long before = HostWaits(gC);
        const double t0 = MCGuiNowMs();
        [gC openFileFromQuickOpen:deep];   // the tree opens lib and lib/inner on the way
        MCGuiNote(@"lib/inner/deep.c opened in %.1f ms, %llu waits on the host, %lu folders listed",
                  MCGuiNowMs() - t0, HostWaits(gC) - before,
                  (unsigned long)([gC remoteWorkspace].listings - listingsBefore));
        // lib and lib/inner asked together, then the read: two, not three.
        GCHECK(HostWaits(gC) - before <= 2);
        GCHECK([gC remoteWorkspace].listings - listingsBefore >= 2);
        GCHECK([gC.currentPath isEqualToString:deep]);
        GCHECK_EQ_STR(TabFor(gC, deep).currentText, @"int deep;\n");
        GCHECK(TreeRow(gC, deep) != nil);   // revealed: its folders open in the tree
    });
    MCGuiStep(@"by the host's notices; a row opens its file through the host", ^{
        GCHECK([gC remoteWorkspace].notices > 0);
        for (NSString *listed in MCTreeListedFolders()) GCHECK(![listed hasPrefix:gFolder]);
        NSString *mainC = [gFolder stringByAppendingPathComponent:@"src/main.c"];
        const NSInteger row = [gC.outline rowForItem:TreeRow(gC, mainC)];
        GCHECK(row >= 0);
        GCHECK(TabFor(gC, mainC) == nil);   // so the open reads it
        // Opening a file costs one round trip: its read, whose own answer
        // is all that is waited for (no Ping behind it), and nothing else
        // asks the host on the way (1.6.19 sent a Ping behind every read).
        const unsigned long long before = HostWaits(gC);
        const double t0 = MCGuiNowMs();
        if (row >= 0) [gC activateRow:row];   // Return in the tree
        MCGuiNote(@"a tree row's file opened in %.1f ms, %llu waits on the host", MCGuiNowMs() - t0,
                  HostWaits(gC) - before);
        GCHECK(HostWaits(gC) - before <= 1);
        MCTab *t = TabFor(gC, mainC);
        GCHECK(t != nil && t.session != nil);
        GCHECK_EQ_STR(t.currentText, @"int needle = 1;\n");
        GCHECK(t.fileModDate == nil);
    });
    // ---- viewers through copies read from the host
    MCGuiStep(@"a PNG, a PDF and a file over 64 MB, made on disk there", ^{
        gPng = [gFolder stringByAppendingPathComponent:@"shot.png"];
        gPdf = [gFolder stringByAppendingPathComponent:@"paper.pdf"];
        gHuge = [gFolder stringByAppendingPathComponent:@"huge.mov"];
        GCHECK(WritePng(gPng, 37, 23));
        GCHECK(WritePdf(gPdf, 3));
        // Sparse: 65 MB that take no room.
        const int fd = open(gHuge.fileSystemRepresentation, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        GCHECK(fd >= 0 && ftruncate(fd, (off_t)65 << 20) == 0);
        if (fd >= 0) close(fd);
    });
    MCGuiWait(@"they come into the tree by the host's notices", 15, ^BOOL {
        return TreeRow(gC, gPng) && TreeRow(gC, gPdf) && TreeRow(gC, gHuge);
    });
    MCGuiStep(@"the PNG from the tree: the picture viewer, from a copy; the tab on the remote path", ^{
        GCHECK(SelectRow(gC, gPng));
        const NSInteger row = [gC.outline rowForItem:TreeRow(gC, gPng)];
        const unsigned long long before = HostWaits(gC);
        if (row >= 0) [gC activateRow:row];   // Return in the tree
        GCHECK(HostWaits(gC) - before <= 1);  // one read decides: text or a copy
        MCTab *t = TabFor(gC, gPng);
        GCHECK(t != nil && t.isImage);
        GCHECK_EQ_STR(gC.currentPath, gPng);
        GCHECK(NSEqualSizes(gC.imagePixels, NSMakeSize(37, 23)));
        if (!NSEqualSizes(gC.imagePixels, NSMakeSize(37, 23)))
            MCGuiNote(@"the picture: %@", NSStringFromSize(gC.imagePixels));
        MCRemoteFileCache *cache = [gC remoteFileCache];
        gCopies = cache.folder;
        NSString *copy = [cache localPathOf:gPng];
        MCGuiNote(@"the window's copies: %@", gCopies);
        GCHECK(gCopies.length > 0 && [gCopies hasPrefix:[MCRemoteFileCache root]]);
        GCHECK([copy hasPrefix:gCopies] && Exists(copy));
        GCHECK(![gPng hasPrefix:gCopies]);
        // Private: the folders 0700, the copy 0600.
        GCHECK(ModeOf(gCopies) == 0700);
        GCHECK(ModeOf([MCRemoteFileCache root]) == 0700);
        GCHECK(ModeOf(copy) == 0600);
        // The title and the tree name the remote file, never the copy.
        GCHECK([gC.window.title containsString:@"shot.png"]);
        GCHECK(![gC.window.title containsString:gCopies]);
        const NSInteger sel = gC.outline.selectedRow;
        id item = sel >= 0 ? [gC.outline itemAtRow:sel] : nil;
        GCHECK([item isKindOfClass:FileItem.class] && [((FileItem *)item).path isEqualToString:gPng]);
        GCHECK(t.fileModDate == nil);   // no stamp of this Mac's
    });
    MCGuiStep(@"the PNG changes there", ^{
        gRefreshes = [gC remoteFileCache].refreshes;
        GCHECK(WritePng(gPng, 50, 40));
    });
    MCGuiWait(@"the picture shown again at its new size, by the host's notice", 15, ^BOOL {
        return NSEqualSizes(gC.imagePixels, NSMakeSize(50, 40));
    });
    MCGuiStep(@"refreshed from a fresh copy, the tab as it was", ^{
        GCHECK([gC remoteFileCache].refreshes > gRefreshes);
        GCHECK_EQ_STR(gC.currentPath, gPng);
        GCHECK(TabFor(gC, gPng).isImage);
    });
    MCGuiStep(@"the PDF from the tree: three pages, from a copy", ^{
        GCHECK(SelectRow(gC, gPdf));
        const NSInteger row = [gC.outline rowForItem:TreeRow(gC, gPdf)];
        if (row >= 0) [gC activateRow:row];
        MCTab *t = TabFor(gC, gPdf);
        GCHECK(t != nil && t.isPDF);
        GCHECK_EQ_STR(gC.currentPath, gPdf);
        PDFView *v = [gC valueForKey:@"pdfView"];
        GCHECK(v.document.pageCount == 3);
        GCHECK([v.document.documentURL.path hasPrefix:gCopies]);
        GCHECK([gC.window.title containsString:@"paper.pdf"]);
    });
    MCGuiStep(@"back to the PNG's tab: its copy shows again", ^{
        GCHECK([gC showFile:gPng]);
        GCHECK(NSEqualSizes(gC.imagePixels, NSMakeSize(50, 40)));
    });
    MCGuiStep(@"a file over 64 MB: the placeholder, no alert and no copy", ^{
        gAlertsAt = MCGuiAlertsShown().count;
        GCHECK([gC showFile:gHuge]);
        MCTab *t = TabFor(gC, gHuge);
        GCHECK(t != nil && t.showingMessage && !t.isMedia);
        NSTextView *tv = [gC valueForKey:@"textView"];
        GCHECK([tv.string containsString:@"too large to open over SSH"]);
        if (![tv.string containsString:@"too large to open over SSH"]) MCGuiNote(@"shown: %@", tv.string);
        GCHECK(AlertsSinceStep().count == 0);
        GCHECK(!Exists([[gC remoteFileCache].folder stringByAppendingPathComponent:@"huge.mov"]));
        [NSFileManager.defaultManager removeItemAtPath:gHuge error:nil];   // nothing later reads 65 MB
    });
    // ---- the tree's file operations, done by the host
    MCGuiStep(@"files for the tree's operations, made on disk there", ^{
        GCHECK([gC remoteDoesFileOps]);
        gOps = [gFolder stringByAppendingPathComponent:@"ops"];
        [NSFileManager.defaultManager createDirectoryAtPath:[gOps stringByAppendingPathComponent:@"into"]
                                withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiWriteFile(Op(@"a.txt"), @"alpha\n");
        MCGuiWriteFile(Op(@"b.txt"), @"beta\n");
        MCGuiWriteFile(Op(@"gone.txt"), @"going\n");
    });
    MCGuiWait(@"ops in the tree", 15, ^BOOL { return TreeRow(gC, gOps) != nil; });
    MCGuiStep(@"Shift+Cmd+N in the tree: a folder made by the host", ^{
        FileItem *ops = TreeRow(gC, gOps);
        if (ops) [gC.outline expandItem:ops];
        GCHECK(SelectRow(gC, gOps));
        MCGuiSetKeyWindow(gC.window);
        [gC.window makeFirstResponder:gC.outline];
        const NSUInteger before = [gC remoteWorkspace].fileOps;
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiTypeInPrompts(@[ @"made" ]);
        MCGuiSendKeyToApp(gC.window, kVK_ANSI_N, NSEventModifierFlagCommand | NSEventModifierFlagShift, @"N", @"n");
        MCGuiNote(@"alerts: %@", AlertsSinceStep());
        GCHECK([AlertsSinceStep() isEqualToArray:@[ @"New folder name:" ]]);
        GCHECK([gC remoteWorkspace].fileOps == before + 1);
        GCHECK(IsDir(Op(@"made")));
        GCHECK(TreeRow(gC, Op(@"made")) != nil);
        // New File, in the folder selected: made by the host, then opened.
        GCHECK(SelectRow(gC, gOps));
        MCGuiTypeInPrompts(@[ @"new.txt" ]);
        [gC newFile:nil];
        GCHECK(Exists(Op(@"new.txt")) && !IsDir(Op(@"new.txt")));
        GCHECK_EQ_STR(Read(Op(@"new.txt")), @"");
        GCHECK_EQ_STR(gC.currentPath, Op(@"new.txt"));
        MCTab *t = TabFor(gC, Op(@"new.txt"));
        GCHECK(t != nil && !t.untitled && !t.dirty);
        GCHECK(TreeRow(gC, Op(@"new.txt")) != nil);
        MCGuiSetKeyWindow(nil);
    });
    MCGuiStep(@"Rename: the host renames a file whose tab has unsaved text, and the tab follows", ^{
        GCHECK([gC showFile:Op(@"a.txt")]);
        MCTab *t = TabFor(gC, Op(@"a.txt"));
        TypeAtEnd(gC, @"typed\n");
        GCHECK(t.dirty);
        // The tree's items are there (the host does file operations), Move
        // to Trash is Delete Permanently.
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Move to Trash" action:@selector(deleteSelected:)
                                               keyEquivalent:@""];
        GCHECK([gC validateMenuItem:item]);
        GCHECK_EQ_STR(item.title, @"Delete Permanently…");
        item = [[NSMenuItem alloc] initWithTitle:@"New Folder…" action:@selector(newFolder:) keyEquivalent:@""];
        GCHECK([gC validateMenuItem:item]);
        GCHECK(SelectRow(gC, Op(@"a.txt")));
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiTypeInPrompts(@[ @"renamed.txt" ]);
        [gC renameSelected:nil];
        GCHECK([AlertsSinceStep() isEqualToArray:@[ @"Rename to:" ]]);
        GCHECK(!Exists(Op(@"a.txt")));
        GCHECK_EQ_STR(Read(Op(@"renamed.txt")), @"alpha\n");   // renamed, not saved
        GCHECK(TabFor(gC, Op(@"a.txt")) == nil);
        GCHECK(TabFor(gC, Op(@"renamed.txt")) == t);
        GCHECK_EQ_STR(t.currentText, @"alpha\ntyped\n");
        GCHECK(t.dirty);
        GCHECK_EQ_STR(gC.currentPath, Op(@"renamed.txt"));
        GCHECK([gC.window.title containsString:@"renamed.txt — standin:"]);
        GCHECK(TreeRow(gC, Op(@"renamed.txt")) != nil && TreeRow(gC, Op(@"a.txt")) == nil);
        // Its document on the host has the new path: Cmd+S writes there.
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"saved to the new name, by the host", 10, ^BOOL {
        return [Read(Op(@"renamed.txt")) isEqualToString:@"alpha\ntyped\n"] && !TabFor(gC, Op(@"renamed.txt")).dirty;
    });
    MCGuiStep(@"renaming onto a name there already: the host refuses, and says why", ^{
        GCHECK(!Exists(Op(@"a.txt")));
        GCHECK(SelectRow(gC, Op(@"b.txt")));
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiTypeInPrompts(@[ @"renamed.txt" ]);
        [gC renameSelected:nil];
        MCGuiNote(@"alerts: %@ (%@)", AlertsSinceStep(), MCGuiLastAlertInformative());
        GCHECK([AlertsSinceStep() isEqualToArray:(@[ @"Rename to:", @"“b.txt” was not renamed." ])]);
        GCHECK_EQ_STR(MCGuiLastAlertInformative(), @"renamed.txt already exists.");
        GCHECK_EQ_STR(Read(Op(@"b.txt")), @"beta\n");
        GCHECK_EQ_STR(Read(Op(@"renamed.txt")), @"alpha\ntyped\n");
    });
    MCGuiStep(@"a row dragged onto a folder: moved by the host, its tab following", ^{
        GCHECK([gC showFile:Op(@"b.txt")]);
        MCTab *t = TabFor(gC, Op(@"b.txt"));
        GCHECK(t != nil && !t.dirty);
        FileItem *b = TreeRow(gC, Op(@"b.txt")), *into = TreeRow(gC, Op(@"into"));
        GCHECK(b != nil && into != nil);
        if (!b || !into) return;
        // What the drag carries: the tree's own row, no file URL of this Mac's.
        id<NSPasteboardWriting> writer = [(id<NSOutlineViewDataSource>)gC outlineView:gC.outline pasteboardWriterForItem:b];
        GCHECK([(id)writer isKindOfClass:NSPasteboardItem.class]);
        GCHECK([[(NSPasteboardItem *)writer types] isEqualToArray:@[ MCTreeRowPasteboardType ]]);
        [(id<NSOutlineViewDataSource>)gC outlineView:gC.outline draggingSession:NoSession()
                                  willBeginAtPoint:NSZeroPoint forItems:@[ b ]];
        MCRemoteTreeDrag *drag = [MCRemoteTreeDrag new];
        drag.draggingSource = gC.outline;
        id<NSDraggingInfo> info = (id<NSDraggingInfo>)drag;
        id<NSOutlineViewDataSource> ds = (id<NSOutlineViewDataSource>)gC;
        GCHECK([ds outlineView:gC.outline validateDrop:info proposedItem:into proposedChildIndex:-1] ==
               NSDragOperationMove);
        // From anywhere but the tree (Finder), nothing goes into the other machine's folder.
        MCRemoteTreeDrag *outside = [MCRemoteTreeDrag new];
        GCHECK([ds outlineView:gC.outline validateDrop:(id<NSDraggingInfo>)outside proposedItem:into
                proposedChildIndex:-1] == NSDragOperationNone);
        GCHECK([ds outlineView:gC.outline acceptDrop:info item:into childIndex:-1]);
        [(id<NSOutlineViewDataSource>)gC outlineView:gC.outline draggingSession:NoSession()
                                      endedAtPoint:NSZeroPoint operation:NSDragOperationMove];
        GCHECK(!Exists(Op(@"b.txt")));
        GCHECK_EQ_STR(Read(Op(@"into/b.txt")), @"beta\n");
        GCHECK(TabFor(gC, Op(@"into/b.txt")) == t && !t.dirty);
        GCHECK_EQ_STR(t.currentText, @"beta\n");
        GCHECK(TreeRow(gC, Op(@"into/b.txt")) != nil && TreeRow(gC, Op(@"b.txt")) == nil);
    });
    MCGuiStep(@"Delete asks first (no Trash there): Cancel keeps it", ^{
        GCHECK([gC showFile:Op(@"gone.txt")]);
        GCHECK(SelectRow(gC, Op(@"gone.txt")));
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[ @(NSAlertSecondButtonReturn) ]);
        [gC deleteSelected:nil];
        GCHECK([AlertsSinceStep() isEqualToArray:@[ @"Delete “gone.txt” permanently?" ]]);
        MCGuiNote(@"informative: %@", MCGuiLastAlertInformative());
        GCHECK([MCGuiLastAlertInformative() containsString:@"which has no Trash"]);
        GCHECK_EQ_STR(Read(Op(@"gone.txt")), @"going\n");
        GCHECK(!TabFor(gC, Op(@"gone.txt")).gone);
    });
    MCGuiStep(@"Delete Permanently: gone from the host's disk, its tab marked deleted", ^{
        GCHECK(SelectRow(gC, Op(@"gone.txt")));
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[ @(NSAlertFirstButtonReturn) ]);
        [gC deleteSelected:nil];
        GCHECK([AlertsSinceStep() isEqualToArray:@[ @"Delete “gone.txt” permanently?" ]]);
        GCHECK(!Exists(Op(@"gone.txt")));
        MCTab *t = TabFor(gC, Op(@"gone.txt"));
        GCHECK(t != nil && t.gone && t.dirty);
        GCHECK_EQ_STR(t.currentText, @"going\n");
        GCHECK(TreeRow(gC, Op(@"gone.txt")) == nil);
        MCGuiNote(@"title: %@", gC.window.title);
        GCHECK([gC.window.title containsString:@"gone.txt (deleted)"]);
        // Closed unsaved, so the host keeps nothing of it.
        if (t) GCHECK([gC closeTab:t prompt:NO]);
        // The questions of these steps are this suite's own: the later steps
        // count from here.
        gAlertsBefore = MCGuiAlertsShown().count;
    });
    // ---- Quick Open and Find in Folder on the host
    MCGuiStep(@"Cmd+P lists the host's files", ^{
        MCGuiSetKeyWindow(gC.window);
        GCHECK(MCGuiPerformMenuKey(gC.window, 35, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Palette(gC) != nil);
    });
    MCGuiWait(@"listed by the host", 15, ^BOOL { return Palette(gC) && MCCommandPaletteFilesListed(Palette(gC)); });
    MCGuiStep(@"the ignored file left out; \"util\" and Return opens src/util.c", ^{
        GCHECK([gC remoteWorkspace].fileLists > 0);
        NSArray<NSString *> *labels = MCCommandPaletteLabels(Palette(gC));
        MCGuiNote(@"Quick Open lists: %@", labels);
        BOOL log = NO, util = NO;
        for (NSString *l in labels) {
            log = log || [l containsString:@"debug.log"];
            util = util || [l containsString:@"util.c"];
        }
        GCHECK(util && !log);
        [PaletteEditor(gC) insertText:@"util" replacementRange:PaletteEditor(gC).selectedRange];
        gWaitsBefore = HostWaits(gC);
        [PaletteEditor(gC) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"src/util.c open, read through the host", 10, ^BOOL {
        return [gC.currentPath isEqualToString:[gFolder stringByAppendingPathComponent:@"src/util.c"]] &&
               [TabFor(gC, gC.currentPath).currentText isEqualToString:@"// a needle here\n"];
    });
    MCGuiStep(@"Quick Open's open waited on the host once at most", ^{
        // src is listed already (the tree opened it): the read at most
        // (none when util.c's tab, from its address above, is still open).
        MCGuiNote(@"Quick Open's open: %llu waits on the host", HostWaits(gC) - gWaitsBefore);
        GCHECK(HostWaits(gC) - gWaitsBefore <= 1);
    });
    MCGuiStep(@"Shift+Cmd+F searches on the host", ^{
        MCGuiSetKeyWindow(nil);
        [gC openSearch:nil];
        SearchPanel *p = gC.searchPanel;
        GCHECK(p != nil && p.hostSearch != nil);
        p.field.stringValue = @"needle";
        [p runSearch:nil];
    });
    MCGuiWait(@"its rows", 15, ^BOOL {
        SearchPanel *p = gC.searchPanel;
        return !p.busy && [p.status.stringValue containsString:@"match"];
    });
    MCGuiStep(@"two files, the ignored one left out; a row opens its file", ^{
        SearchPanel *p = gC.searchPanel;
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK_EQ_STR(p.status.stringValue, @"2 matches in 2 files");
        GCHECK([gC remoteWorkspace].searches > 0);
        NSOutlineView *t = (NSOutlineView *)p.table;
        NSInteger hit = -1;
        for (NSInteger r = 0; r < t.numberOfRows && hit < 0; r++) {
            id item = [t itemAtRow:r];
            if ([item respondsToSelector:@selector(line)] &&
                [[item valueForKey:@"path"] hasSuffix:@"/src/main.c"])
                hit = r;
        }
        GCHECK(hit >= 0);
        if (hit >= 0) {
            [t selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)hit] byExtendingSelection:NO];
            [NSApp sendAction:t.doubleAction to:t.target from:t];
        }
        GCHECK([gC.currentPath hasSuffix:@"/src/main.c"]);
    });
    // ---- Replace All on the host: r1 closed, r2 open and clean, r3 open
    // with unsaved text.
    MCGuiStep(@"files to replace in: one closed, one in a clean tab, one in a tab with unsaved text", ^{
        MCGuiWriteFile(Op(@"r1.txt"), @"swap one\n");
        MCGuiWriteFile(Op(@"r2.txt"), @"swap two\n");
        MCGuiWriteFile(Op(@"r3.txt"), @"swap three\n");
        GCHECK([gC showFile:Op(@"r2.txt")]);
        GCHECK(!TabFor(gC, Op(@"r2.txt")).dirty);
        GCHECK([gC showFile:Op(@"r3.txt")]);
        TypeAtEnd(gC, @"more\n");
        GCHECK(TabFor(gC, Op(@"r3.txt")).dirty);
        SearchPanel *p = gC.searchPanel;
        GCHECK(p.hostReplace != nil);
        p.field.stringValue = @"swap";
        [p runSearch:nil];
    });
    MCGuiWait(@"the rows to replace", 15, ^BOOL {
        SearchPanel *p = gC.searchPanel;
        return !p.busy && [p.status.stringValue containsString:@"match"];
    });
    MCGuiStep(@"Replace All asks as locally, and the host replaces", ^{
        SearchPanel *p = gC.searchPanel;
        GCHECK_EQ_STR(p.status.stringValue, @"3 matches in 3 files");
        p.replaceField.stringValue = @"traded";
        p.preserveCase = NO;   // the host ignores it: refused over SSH
        gAlertsAt = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[ @(NSAlertFirstButtonReturn) ]);
        [p replaceAll:nil];
        GCHECK([AlertsSinceStep() isEqualToArray:@[ @"Replace 2 matches in 2 files?" ]]);
        MCGuiNote(@"informative: %@", MCGuiLastAlertInformative());
        GCHECK([MCGuiLastAlertInformative() hasPrefix:@"Each becomes “traded”."]);
        GCHECK([MCGuiLastAlertInformative()
            containsString:@"Left alone, since their tabs have unsaved changes: r3.txt."]);
        GCHECK([gC remoteWorkspace].replaces > 0);
    });
    MCGuiWait(@"replaced, and searched again", 15, ^BOOL {
        SearchPanel *p = gC.searchPanel;
        return !p.busy && [p.status.stringValue hasPrefix:@"Replaced"];
    });
    MCGuiStep(@"the closed file rewritten, the clean tab edited unsaved, the other left alone", ^{
        SearchPanel *p = gC.searchPanel;
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK([p.status.stringValue hasPrefix:@"Replaced 2 matches in 2 files (1 open in tabs, unsaved). "
                                               @"Left alone, with unsaved changes: r3.txt"]);
        GCHECK_EQ_STR(Read(Op(@"r1.txt")), @"traded one\n");
        GCHECK_EQ_STR(Read(Op(@"r2.txt")), @"swap two\n");
        MCTab *r2 = TabFor(gC, Op(@"r2.txt"));
        GCHECK_EQ_STR(r2.currentText, @"traded two\n");
        GCHECK(r2.dirty);
        GCHECK_EQ_STR(Read(Op(@"r3.txt")), @"swap three\n");
        GCHECK_EQ_STR(TabFor(gC, Op(@"r3.txt")).currentText, @"swap three\nmore\n");
        GCHECK_EQ_STR(Read([gFolder stringByAppendingPathComponent:@"src/main.c"]), @"int needle = 1;\n");
        [p.window orderOut:nil];
        // r3 closed unsaved, r2 saved: nothing unsaved is left on the host.
        if (MCTab *r3 = TabFor(gC, Op(@"r3.txt"))) GCHECK([gC closeTab:r3 prompt:NO]);
        GCHECK([gC showFile:Op(@"r2.txt")]);
        [gC saveCurrentFile:nil];
        gAlertsBefore = MCGuiAlertsShown().count;
    });
    MCGuiWait(@"r2 saved by the host", 10, ^BOOL {
        return [Read(Op(@"r2.txt")) isEqualToString:@"traded two\n"] && !TabFor(gC, Op(@"r2.txt")).dirty;
    });
    // ---- addresses an open window's folder holds. As a local file goes
    // to the window whose folder holds it, a file's address on the same
    // machine opens in the window over SSH whose folder holds it, through
    // that window's host (no ssh run, no second window), by the host's own
    // resolving (a link to the folder, "..").
    MCGuiStep(@"ssh://standin/<folder>/src/util.c opens in the open window, through its host", ^{
        GCHECK([gC showFile:gFile]);
        NSString *util = [gFolder stringByAppendingPathComponent:@"src/util.c"];
        if (MCTab *open = TabFor(gC, util)) GCHECK([gC closeTab:open prompt:NO]);
        GCHECK(TabFor(gC, util) == nil);
        gWindowsBefore = MCGuiControllers().count;
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:util]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        GCHECK(NewAlerts().count == 0);
        GCHECK(MCGuiControllers().count == gWindowsBefore);
        GCHECK(RemoteWindow() == gC && RemoteWindowOn([gFolder stringByAppendingPathComponent:@"src"]) == nil);
        GCHECK(StandInPid() == gHostA);   // no ssh run for it
        GCHECK_EQ_STR(gC.currentPath, util);
        MCTab *t = TabFor(gC, util);
        GCHECK(t != nil && t.session != nil && t.fileModDate == nil);
        GCHECK_EQ_STR(t.currentText, @"// a needle here\n");
    });
    MCGuiStep(@"the same file through a link to the folder, and by \"..\": the same window and tab", ^{
        NSString *alias = [MCGuiScratch() stringByAppendingPathComponent:@"remote-alias"];
        [NSFileManager.defaultManager createSymbolicLinkAtPath:alias withDestinationPath:gFolder error:nil];
        GCHECK([gC showFile:gFile]);
        NSString *mainC = [gFolder stringByAppendingPathComponent:@"src/main.c"];
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:[alias stringByAppendingPathComponent:@"src/main.c"]]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        GCHECK(MCGuiControllers().count == gWindowsBefore);
        GCHECK(StandInPid() == gHostA);
        GCHECK_EQ_STR(gC.currentPath, mainC);   // spelled under the window's folder, as the host resolved it
        GCHECK(TabFor(gC, [alias stringByAppendingPathComponent:@"src/main.c"]) == nil);
        GCHECK([gC showFile:gFile]);
        url = [NSURL URLWithString:[NSString stringWithFormat:@"ssh://standin%@/src/../src/main.c", gFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        GCHECK(MCGuiControllers().count == gWindowsBefore);
        GCHECK_EQ_STR(gC.currentPath, mainC);
        GCHECK(NewAlerts().count == 0);
    });
    MCGuiStep(@"ssh://standin/<folder> again: the open window comes to front, no other opens", ^{
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        url = [NSURL URLWithString:[NSString stringWithFormat:@"ssh://standin%@/", [MCGuiScratch() stringByAppendingPathComponent:@"remote-alias"]]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        GCHECK(NewAlerts().count == 0);
        GCHECK(MCGuiControllers().count == gWindowsBefore);
        GCHECK(RemoteWindow() == gC);
        GCHECK(StandInPid() == gHostA);
    });
    // ---- a new file's address in the open window's folder: a tab of that
    // window, empty and untitled at its path, which Cmd+S makes through the
    // window's host.
    MCGuiStep(@"ssh://standin/<folder>/src/fresh.txt, not there yet, opens an empty untitled tab there", ^{
        NSString *fresh = [gFolder stringByAppendingPathComponent:@"src/fresh.txt"];
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:fresh]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@ (%@)", NewAlerts(), MCGuiLastAlertInformative());
        GCHECK(NewAlerts().count == 0);
        GCHECK(MCGuiControllers().count == gWindowsBefore);
        GCHECK(StandInPid() == gHostA);
        MCTab *t = TabFor(gC, fresh);
        if (!t) MCGuiNote(@"its tabs: %@", [[gC valueForKey:@"tabs"] valueForKey:@"currentPath"]);
        GCHECK(t != nil && t.untitled && !t.dirty);
        GCHECK_EQ_STR(t.currentText, @"");
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:fresh]);
        GCHECK_EQ_STR(gC.currentPath, fresh);
        if (!t) return;
        TypeAtEnd(gC, @"fresh\n");
        GCHECK(t.dirty);
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"made by the window's host's save", 10, ^BOOL {
        NSString *fresh = [gFolder stringByAppendingPathComponent:@"src/fresh.txt"];
        MCTab *t = TabFor(gC, fresh);
        return [Read(fresh) isEqualToString:@"fresh\n"] && t && !t.dirty && !t.untitled;
    });
    // ---- what no open window holds: another machine's address (the
    // stand-in serves the same disk, so only the machine differs), and a
    // file outside the folder. Each opens a window on the file's folder,
    // closed after with its host.
    MCGuiStep(@"ssh://other/<folder>/src/util.c: another machine", ^{
        NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"ssh://other%@/src/util.c", gFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        GCHECK(MCRemoteConnect.pending.count == 1);   // not the open window's machine: nothing waits
    });
    MCGuiWait(@"connected", 20, ^BOOL { return MCRemoteConnect.pending.count == 0; });
    MCGuiStep(@"a window of its own", ^{
        GCHECK(NewAlerts().count == 0);
        gFreshWindow = RemoteWindowOn([gFolder stringByAppendingPathComponent:@"src"]);
        GCHECK(gFreshWindow != nil && gFreshWindow != gC);
        if (!gFreshWindow) return;
        gHostFresh = StandInPid();
        GCHECK(gHostFresh > 0 && gHostFresh != gHostA);
        GCHECK([gFreshWindow.sessionRemoteLabel hasPrefix:@"other:"]);
        GCHECK_EQ_STR(gFreshWindow.currentPath, [gFolder stringByAppendingPathComponent:@"src/util.c"]);
        [gFreshWindow.window performClose:nil];
        gFreshWindow = nil;
    });
    MCGuiWait(@"that window gone, and its host with it", 15, ^BOOL {
        return RemoteWindow() == gC && MCGuiControllers().count == gWindowsBefore && (gHostFresh <= 0 || Gone(gHostFresh));
    });
    MCGuiStep(@"ssh://standin/<outside>/elsewhere.txt: outside the folder, a window of its own", ^{
        if (gHostFresh > 0 && !Gone(gHostFresh)) kill(-gHostFresh, SIGKILL);   // the suite's own, by its recorded pid
        gHostFresh = 0;
        NSString *outside = [RealPath(MCGuiScratch()) stringByAppendingPathComponent:@"remote-outside"];
        [NSFileManager.defaultManager createDirectoryAtPath:outside withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *file = [outside stringByAppendingPathComponent:@"elsewhere.txt"];
        MCGuiWriteFile(file, @"elsewhere\n");
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:file]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
    });
    MCGuiWait(@"connected", 20, ^BOOL { return MCRemoteConnect.pending.count == 0; });
    MCGuiStep(@"a window of its own on that folder", ^{
        NSString *outside = [RealPath(MCGuiScratch()) stringByAppendingPathComponent:@"remote-outside"];
        NSString *file = [outside stringByAppendingPathComponent:@"elsewhere.txt"];
        GCHECK(NewAlerts().count == 0);
        gFreshWindow = RemoteWindowOn(outside);
        GCHECK(gFreshWindow != nil && gFreshWindow != gC);
        if (!gFreshWindow) {
            NSMutableArray *roots = [NSMutableArray array];
            for (EditorController *c in MCGuiControllers()) [roots addObject:c.rootPath ?: @"?"];
            MCGuiNote(@"windows: %@", roots);
            return;
        }
        gHostFresh = StandInPid();
        GCHECK(gHostFresh > 0 && gHostFresh != gHostA);
        GCHECK_EQ_STR(TabFor(gFreshWindow, file).currentText, @"elsewhere\n");
        [gFreshWindow.window performClose:nil];
        gFreshWindow = nil;
    });
    MCGuiWait(@"that window gone too, and its host", 15, ^BOOL {
        return RemoteWindow() == gC && MCGuiControllers().count == gWindowsBefore && (gHostFresh <= 0 || Gone(gHostFresh));
    });
    MCGuiStep(@"the first window's notes.txt on show again", ^{
        if (gHostFresh > 0 && !Gone(gHostFresh)) kill(-gHostFresh, SIGKILL);   // the suite's own, by its recorded pid
        GCHECK([gC showFile:gFile]);
        GCHECK_EQ_STR(gC.currentPath, gFile);
    });
    MCGuiStep(@"still no stamp, and no question was asked", ^{
        if (!gC) return;
        GCHECK(TabFor(gC, gFile).fileModDate == nil);
        GCHECK(NewAlerts().count == 0);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@", NewAlerts());
        [gC newTerminal:nil];
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm != nil && gTerm.onSessionHost && gTerm.sessionTerminal != 0);
        GCHECK(gTerm.shellPid == 0);   // a remote shell has no pid of this Mac's
    });
    // The parent's name as ps gives it: the stand-in's `mutible`, the host
    // itself; the variable is the stand-in's, not the Mac's.
    RunUntil(@"the terminal runs in the host's shell and environment",
             @"echo \"remote-$SSH_STANDIN-$((6*7))\" \"parent-$(basename \"$(ps -o comm= -p $PPID)\")\"",
             @"remote-yes-42 parent-mutible");
    // The host is the folder's lasting one: the shell's parent, not the
    // stand-in (the relay).
    RunUntil(@"the terminal's shell and host", @"echo \"shell-$$ host-$PPID end-$((3*5))\"", @" end-15");
    MCGuiStep(@"the lasting host serves it, not the relay", ^{
        gShellPid = ScreenWord(@"shell-");
        gLasting = (pid_t)ScreenWord(@"host-").intValue;
        gTermId = gTerm.sessionTerminal;
        MCGuiNote(@"shell %@, host %d, relay %d", gShellPid, (int)gLasting, (int)gHostA);
        GCHECK(gShellPid.intValue > 0 && gLasting > 0 && gLasting != gHostA);
        GCHECK(gTermId != 0);
    });
    // ---- the remote's own shell integration (its host's scripts, Welcome
    // flag 128, TermSpec's integration): marks, a running program, exit
    // status, a nonce no local terminal shares.
    MCGuiStep(@"the remote host sets up its own integration", ^{
        MCSessionLinkRef *ref = [gC sessionRemoteLink];
        GCHECK(ref.link && ref.link->client().hostIntegrates());
    });
    MCGuiWait(@"the remote shell's marks believed: at its prompt", 20, ^BOOL {
        return gTerm.integrated && gTerm.atPrompt && !gTerm.busy;
    });
    MCGuiStep(@"its nonce is this view's, kept under the remote host's key; the user's ZDOTDIR", ^{
        gRemoteNonce = gTerm.integrationNonce;
        GCHECK(gRemoteNonce.length == 32);
        GCHECK_EQ_STR([TerminalView nonceForHost:LinkKey([gC sessionRemoteLink]) terminal:gTerm.sessionTerminal],
                      gRemoteNonce);
        gEndedBefore = Ended(gTerm);
        [gTerm runCommand:@"echo \"zdot-[$ZDOTDIR] nonce-[${MUTIBLE_NONCE-unset}]\""];
    });
    MCGuiWait(@"the user's ZDOTDIR handed back, the nonce out of the environment, the command marked", 15, ^BOOL {
        NSString *want = [NSString stringWithFormat:@"zdot-[%s] nonce-[unset]", getenv("ZDOTDIR") ?: ""];
        return [[gTerm recentText:30] containsString:want] && Ended(gTerm) > gEndedBefore && gTerm.atPrompt;
    });
    MCGuiStep(@"sleep 5", ^{
        gEndedBefore = Ended(gTerm);
        [gTerm runCommand:@"sleep 5"];
    });
    MCGuiWait(@"a running program, by the marks: busy, its command named", 10, ^BOOL {
        return gTerm.busy && !gTerm.atPrompt && [[gTerm valueForKey:@"commandStarted"] boolValue] &&
               [gTerm.runningCommand hasPrefix:@"sleep 5"];
    });
    MCGuiStep(@"while it runs, a command waits for the prompt", ^{
        GCHECK(gTerm.busy && Ended(gTerm) == gEndedBefore);
        [gTerm runCommand:@"false"];
        GCHECK([gTerm.pendingCommands containsObject:@"false"]);
    });
    MCGuiWait(@"sleep ends, then false runs: its exit status seen", 20, ^BOOL {
        return Ended(gTerm) == gEndedBefore + 2 && gTerm.atPrompt && !gTerm.busy && gTerm.pendingCommands.count == 0;
    });
    MCGuiStep(@"false's status is 1, sleep's 0", ^{
        TerminalGridView *g = [gTerm valueForKey:@"grid"];
        std::vector<TermCommand> ended;
        for (const TermCommand &c : g.screen->commands())
            if (c.ended) ended.push_back(c);
        GCHECK(ended.size() >= 2);
        if (ended.size() >= 2) {
            GCHECK(ended[ended.size() - 2].status == 0);
            GCHECK(ended.back().status == 1);
        }
        GCHECK(LastEnded(gTerm).status == 1);
    });
    MCGuiStep(@"a local terminal on its own host beside it", ^{
        MCGuiSetSettings(@"lsp.enabled = false\neditor.session-host = on\n");
        gLocalC = nil;
        for (EditorController *c in MCGuiControllers())
            if (c != gC && ![c sessionRemoteLink]) gLocalC = c;   // the harness's own window
        GCHECK(gLocalC != nil);
        if (!gLocalC) return;
        [gLocalC newTerminal:nil];
        gLocalTerm = [gLocalC valueForKey:@"terminal"];
        GCHECK(gLocalTerm != nil && gLocalTerm != gTerm && gLocalTerm.onSessionHost);
    });
    MCGuiWait(@"its prompt, by this Mac's integration", 20, ^BOOL {
        return gLocalTerm.integrated && gLocalTerm.atPrompt && !gLocalTerm.busy;
    });
    MCGuiStep(@"each nonce its own, each under its own host's key", ^{
        NSString *localKey = LinkKey(gLocalTerm.sessionLink), *remoteKey = LinkKey([gC sessionRemoteLink]);
        MCGuiNote(@"local [%@] term %llu, remote [%@] term %llu", localKey, gLocalTerm.sessionTerminal, remoteKey,
                  gTerm.sessionTerminal);
        // The process's own host has the key "" (Link::key: no folder, it
        // serves every window); a remote one's is its ssh:// and folder.
        GCHECK(gLocalTerm.sessionLink.link != nullptr && remoteKey.length && ![localKey isEqualToString:remoteKey]);
        GCHECK(gLocalTerm.integrationNonce.length == 32 && ![gLocalTerm.integrationNonce isEqualToString:gRemoteNonce]);
        GCHECK_EQ_STR([TerminalView nonceForHost:localKey terminal:gLocalTerm.sessionTerminal],
                      gLocalTerm.integrationNonce);
        GCHECK_EQ_STR([TerminalView nonceForHost:remoteKey terminal:gTerm.sessionTerminal], gRemoteNonce);
        // A terminal of the same id on the other host never finds this one's.
        NSString *cross = [TerminalView nonceForHost:localKey terminal:gTerm.sessionTerminal];
        GCHECK(cross == nil || ![cross isEqualToString:gRemoteNonce]);
        cross = [TerminalView nonceForHost:remoteKey terminal:gLocalTerm.sessionTerminal];
        GCHECK(cross == nil || ![cross isEqualToString:gLocalTerm.integrationNonce]);
        GCHECK([(MCTerminalPanel *)[gLocalC valueForKey:@"terminalPanel"] closeTerminal:gLocalTerm]);
        gLocalTerm = nil;
        MCGuiSetSettings(@"lsp.enabled = false\n");
        GCHECK(gTerm.integrated);
    });
    // Earlier output, and a program left running across the drop.
    RunUntil(@"output before the drop", @"echo \"before-drop-$((6*7))\"", @"before-drop-42");
    MCGuiStep(@"a program runs; typed text is unsaved; the connection drops", ^{
        if (!gC) return;
        [gTerm runCommand:@"sleep 600"];
        TypeAtEnd(gC, @"before drop\n");
        MCTab *t = TabFor(gC, gFile);
        GCHECK(t.dirty);
        GCHECK(!gC.sessionHostLost);
    });
    MCGuiWait(@"the host says sleep runs and the text is unsaved", 15, ^BOOL {
        return gTerm.programHasTerminal && [MCSessionCheckTabs(@[ TabFor(gC, gFile) ]) isEqualToString:@""];
    });
    MCGuiStep(@"the relay killed, the stand-in refusing for now", ^{
        MCGuiWriteFile(gBlock, @"");
        gTriesBefore = RefusedTries();
        // The stand-in is the leader of its own process group (the spawn's
        // session): the relay goes with it, the lasting host (a session of
        // its own) does not.
        if (gHostA > 0) kill(-gHostA, SIGKILL);
    });
    MCGuiWait(@"the window says it is reconnecting", 15, ^BOOL {
        return gC.sessionHostLost && gC.sessionReconnecting && [gC.window.title containsString:@"(reconnecting)"];
    });
    MCGuiStep(@"the text stays and takes more typing; no Reconnect button while it tries", ^{
        GCHECK(!Gone(gLasting));   // the host outlived the connection
        MCTab *t = TabFor(gC, gFile);
        GCHECK_EQ_STR(t.currentText, @"first line\ntyped over ssh\nbefore drop\n");
        GCHECK(!t.session.hostSavesFile);
        TypeAtEnd(gC, @"while away\n");
        GCHECK(t.dirty);
        GCHECK_EQ_STR(t.currentText, @"first line\ntyped over ssh\nbefore drop\nwhile away\n");
        BOOL button = NO;
        for (NSView *v in [[gC valueForKey:@"statusBar"] subviews])
            if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:@"Reconnect"] && !v.hidden)
                button = YES;
        GCHECK(!button);
    });
    MCGuiWait(@"its tries refused (the backoff's first waits)", 20, ^BOOL { return RefusedTries() >= gTriesBefore + 2; });
    MCGuiStep(@"the stand-in connects again", ^{
        GCHECK(gC.sessionHostLost && gC.sessionReconnecting);
        [NSFileManager.defaultManager removeItemAtPath:gBlock error:nil];
    });
    MCGuiWait(@"the window takes up the host by itself", 30, ^BOOL {
        return !gC.sessionHostLost && TabFor(gC, gFile).session.hostSavesFile;
    });
    MCGuiStep(@"a new relay to the same host: the same terminal, its output, its program", ^{
        gHostB = StandInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        GCHECK(Gone(gHostA));
        GCHECK(!Gone(gLasting));
        GCHECK(!gC.sessionReconnecting);
        GCHECK(![gC.window.title containsString:@"(reconnecting)"] && ![gC.window.title containsString:@"(disconnected)"]);
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm.sessionTerminal == gTermId);   // the host's own terminal, taken up again
        // The host's replay, scrollback included, is parsed before the
        // window takes up the host returns (reattachOn:place:).
        NSString *recent = [gTerm recentText:60];
        if (![recent containsString:@"before-drop-42"]) MCGuiNote(@"the terminal after the reconnect: %@", recent);
    });
    MCGuiWait(@"its output from before the drop, replayed by the host", 10, ^BOOL {
        return [[gTerm recentText:60] containsString:@"before-drop-42"];
    });
    MCGuiWait(@"sleep still runs there", 10, ^BOOL { return gTerm.programHasTerminal; });
    MCGuiStep(@"the marks still believed: the nonce found under the remote host's key", ^{
        GCHECK(gTerm.integrated && [gTerm.integrationNonce isEqualToString:gRemoteNonce]);
        GCHECK(gTerm.busy && !gTerm.atPrompt);
    });
    MCGuiStep(@"Ctrl+C ends it", ^{
        const char etx = 0x03;
        TerminalGridView *grid = [gTerm valueForKey:@"grid"];
        grid.onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"the shell has the terminal again, at its prompt", 10, ^BOOL {
        return !gTerm.programHasTerminal && gTerm.atPrompt;
    });
    RunUntil(@"the same shell types on", @"echo \"again-$$-$((7*8))\"", @"-56");
    MCGuiStep(@"the same shell's pid; the document as it was, with what was typed meanwhile", ^{
        GCHECK_EQ_STR(ScreenWord(@"again-"), ([NSString stringWithFormat:@"%@-56", gShellPid]));
        MCTab *t = TabFor(gC, gFile);
        GCHECK(t.dirty);
        GCHECK_EQ_STR(t.currentText, @"first line\ntyped over ssh\nbefore drop\nwhile away\n");
        GCHECK_EQ_STR(MCSessionCheckTabs(@[ t ]), @"");
        GCHECK([gC showFile:gFile]);
    });
    MCGuiStep(@"undo goes back past the drop, into what the host had before it", ^{
        NSTextView *tv = [gC valueForKey:@"textView"];
        [tv.undoManager undo];   // what was typed while away (its own step)
        GCHECK_EQ_STR(TabFor(gC, gFile).currentText, @"first line\ntyped over ssh\nbefore drop\n");
        [tv.undoManager undo];   // typed before the drop, the host's step
        // (which may have joined the typing before it: the caret stayed put)
        NSString *back = TabFor(gC, gFile).currentText;
        MCGuiNote(@"after two undos: %@", back);
        GCHECK([back hasPrefix:@"first line\n"] && ![back containsString:@"before drop"]);
        [tv.undoManager redo];
        [tv.undoManager redo];
        GCHECK_EQ_STR(TabFor(gC, gFile).currentText, @"first line\ntyped over ssh\nbefore drop\nwhile away\n");
        GCHECK_EQ_STR(MCSessionCheckTabs(@[ TabFor(gC, gFile) ]), @"");
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"saved by the host", 10, ^BOOL {
        return [Read(gFile) isEqualToString:@"first line\ntyped over ssh\nbefore drop\nwhile away\n"] &&
               !TabFor(gC, gFile).dirty;
    });
    MCGuiStep(@"a file made after the drop", ^{
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"after-reconnect.txt"], @"again\n");
    });
    MCGuiWait(@"comes into the tree by the host's notices", 15, ^BOOL {
        return TreeRow(gC, [gFolder stringByAppendingPathComponent:@"after-reconnect.txt"]) != nil;
    });
    MCGuiStep(@"the window closes", ^{
        GCHECK(NewAlerts().count == 0);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@", NewAlerts());
        gTerm = nil;
        [gC.window performClose:nil];
        gC = nil;
    });
    MCGuiWait(@"gone", 10, ^BOOL { return RemoteWindow() == nil; });
    MCGuiStep(@"its folder of copies gone with it", ^{
        GCHECK(gCopies.length > 0 && !Exists(gCopies));
    });
    MCGuiWait(@"its relay ends with it", 15, ^BOOL { return Gone(gHostB); });
    MCGuiWait(@"the lasting host ends after its idle time, nothing left in it", 30, ^BOOL { return Gone(gLasting); });
    // ---- a remote whose login shell is bash (Ubuntu's default): its host
    // sets up the bash script (build/'s copy, 4.4 or later), and the Mac
    // follows its marks: a running program, exit status, the typing queue,
    // a half-typed line set aside and given back (the pop key), and Run
    // File's way in (runCommand:). A window on a folder of its own, so its
    // lasting host starts with the stand-in's bash.
    MCGuiStep(@"a remote with a bash login shell", ^{
        gBash = FindBash();
        if (!gBash) {
            MCGuiNote(@"no bash 4.4 or later on this Mac: the bash remote's checks skipped");
            return;
        }
        MCGuiWriteFile(gShellFile, [gBash stringByAppendingString:@"\n"]);
        NSString *made = [MCGuiScratch() stringByAppendingPathComponent:@"remote-bash"];
        [NSFileManager.defaultManager createDirectoryAtPath:made withIntermediateDirectories:YES attributes:nil error:nil];
        gBashFolder = RealPath(made);
        MCGuiWriteFile([gBashFolder stringByAppendingPathComponent:@"run.sh"], @"echo ran-$((5*5))\n");
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gBashFolder]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
    });
    MCGuiWait(@"connected", 20, ^BOOL { return !gBash || MCRemoteConnect.pending.count == 0; });
    MCGuiStep(@"its window, a terminal", ^{
        if (!gBash) return;
        GCHECK(NewAlerts().count == 0);
        gBashC = RemoteWindowOn(gBashFolder);
        GCHECK(gBashC != nil);
        if (!gBashC) { gBash = nil; return; }
        GCHECK([gBashC sessionRemoteLink].link->client().hostIntegrates());
        [gBashC newTerminal:nil];
        gBashTerm = [gBashC valueForKey:@"terminal"];
        GCHECK(gBashTerm != nil && gBashTerm.onSessionHost);
    });
    MCGuiWait(@"the bash's marks believed: at its prompt", 20, ^BOOL {
        return !gBash || (gBashTerm.integrated && gBashTerm.integratedBash && gBashTerm.atPrompt && !gBashTerm.busy);
    });
    MCGuiStep(@"its nonce kept under its host's key; a login bash, the nonce out of its environment", ^{
        if (!gBash) return;
        GCHECK(gBashTerm.integrationNonce.length == 32 && ![gBashTerm.integrationNonce isEqualToString:gRemoteNonce]);
        GCHECK_EQ_STR([TerminalView nonceForHost:LinkKey([gBashC sessionRemoteLink]) terminal:gBashTerm.sessionTerminal],
                      gBashTerm.integrationNonce);
        gEndedBefore = Ended(gBashTerm);
        [gBashTerm runCommand:@"echo \"nonce-[${MUTIBLE_NONCE-unset}] login-[${MUTIBLE_BASH_LOGIN-unset}] host-$PPID\""];
    });
    MCGuiWait(@"run, marked, and back at the prompt", 15, ^BOOL {
        return !gBash || ([[gBashTerm recentText:30] containsString:@"nonce-[unset] login-[unset] host-"] &&
                          Ended(gBashTerm) > gEndedBefore && gBashTerm.atPrompt && !gBashTerm.busy);
    });
    MCGuiStep(@"sleep 5", ^{
        if (!gBash) return;
        gBashHost = (pid_t)ScreenWordIn(gBashTerm, @"host-").intValue;
        GCHECK(gBashHost > 0);
        gEndedBefore = Ended(gBashTerm);
        [gBashTerm runCommand:@"sleep 5"];
    });
    MCGuiWait(@"a running program, by the marks: busy, its command named", 10, ^BOOL {
        return !gBash || (gBashTerm.busy && !gBashTerm.atPrompt && [[gBashTerm valueForKey:@"commandStarted"] boolValue] &&
                          [gBashTerm.runningCommand hasPrefix:@"sleep 5"]);
    });
    MCGuiStep(@"while it runs, false waits for the prompt", ^{
        if (!gBash) return;
        GCHECK(gBashTerm.busy && Ended(gBashTerm) == gEndedBefore);
        [gBashTerm runCommand:@"false"];
        GCHECK([gBashTerm.pendingCommands containsObject:@"false"]);
    });
    MCGuiWait(@"sleep ends, then false runs", 20, ^BOOL {
        return !gBash || (Ended(gBashTerm) == gEndedBefore + 2 && gBashTerm.atPrompt && !gBashTerm.busy &&
                          gBashTerm.pendingCommands.count == 0);
    });
    MCGuiStep(@"false's status is 1; a half-typed line, then a command the app runs", ^{
        if (!gBash) return;
        GCHECK(LastEnded(gBashTerm).status == 1);
        gEndedBefore = Ended(gBashTerm);
        TerminalGridView *grid = [gBashTerm valueForKey:@"grid"];
        grid.onInput([@"echo half-$((4*5))" dataUsingEncoding:NSUTF8StringEncoding]);
        [gBashTerm runCommand:@"echo pushed-$((2*3))"];
    });
    MCGuiWait(@"the command ran alone, and the half-typed line is back at the prompt", 15, ^BOOL {
        return !gBash || (Ended(gBashTerm) == gEndedBefore + 1 && gBashTerm.atPrompt &&
                          [AfterPushed(gBashTerm) containsString:@"echo half-$((4*5))"]);
    });
    MCGuiStep(@"Return runs it", ^{
        if (!gBash) return;
        TerminalGridView *grid = [gBashTerm valueForKey:@"grid"];
        grid.onInput([@"\r" dataUsingEncoding:NSUTF8StringEncoding]);
    });
    MCGuiWait(@"half-20", 10, ^BOOL {
        return !gBash || ([AfterPushed(gBashTerm) containsString:@"half-20"] && gBashTerm.atPrompt && !gBashTerm.busy);
    });
    MCGuiStep(@"Run File's way in: the command typed at the prompt", ^{
        if (!gBash) return;
        gEndedBefore = Ended(gBashTerm);
        [gBashTerm runCommand:@"sh run.sh"];
    });
    MCGuiWait(@"ran, status 0", 10, ^BOOL {
        return !gBash || ([[gBashTerm recentText:10] containsString:@"ran-25"] && Ended(gBashTerm) == gEndedBefore + 1 &&
                          LastEnded(gBashTerm).status == 0 && gBashTerm.atPrompt);
    });
    MCGuiStep(@"its window closes", ^{
        if (!gBash) return;
        gBashTerm = nil;
        [gBashC.window performClose:nil];
        gBashC = nil;
    });
    MCGuiWait(@"its lasting host ends after its idle time", 30, ^BOOL { return !gBash || gBashHost <= 0 || Gone(gBashHost); });
    MCGuiStep(@"nothing left", ^{
        // The suite's own, by their recorded pids.
        if (gHostB > 0 && !Gone(gHostB)) kill(-gHostB, SIGKILL);
        if (gLasting > 0 && !Gone(gLasting)) kill(gLasting, SIGKILL);
        if (gBashHost > 0 && !Gone(gBashHost)) kill(gBashHost, SIGKILL);
        [NSFileManager.defaultManager removeItemAtPath:gShellFile error:nil];
        [NSFileManager.defaultManager removeItemAtPath:gBlock error:nil];
        MCSetEnv("MUTIBLE_SSH", nullptr);
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", nullptr);
    });
}
