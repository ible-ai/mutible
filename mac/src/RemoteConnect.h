// RemoteConnect.h — Objective-C++. A window over SSH connecting for the
// first time (stage 8, docs/notes/session-host.md, "Connecting off the main
// thread"): `mutible ssh://host/path` shows this small window at once while
// ssh connects and mutible starts there (MCSession::connectRemote, which
// waits on nothing), so the app and its other windows go on as usual for
// the 30 s a slow or unreachable machine may take.
//   - Its line says how far it got (Shared::ConnectProgress): "Connecting
//     to host…", "Starting mutible on host…" (once `mutible host --stdio`
//     said it started, on ssh's standard error), "Opening <folder> on
//     host…" (the Welcome came; the window on the folder is made on the next
//     turns, then this one closes).
//   - Cancel (the button, Esc, or the close button) kills ssh's process
//     group by its pid and closes the window; the addresses that waited for
//     it are dropped.
//   - When it cannot connect, the alert says why (Shared::explainSshFailure:
//     a password or host key to confirm, mutible missing or too old there,
//     ssh's own last line) as a sheet on this window, which closes with it;
//     the addresses that waited for it are opened again, each on its own.
//   - Another address on the same machine and account while it connects
//     waits for it (queueAddress:): the window it opens may hold that file,
//     as #327's routing says, and an address of the same machine and path is
//     this window's own (the caller brings it to front).
// Main thread.
#pragma once
#import <Cocoa/Cocoa.h>

#include "SessionServer.h"

@class MCSessionLinkRef;

@interface MCRemoteConnect : NSObject <NSWindowDelegate>
// Starts connecting to `address`, an ssh:// address that no open window
// holds, its window shown. `opened` once with the link when the host's
// Welcome came; `ended` once at the end, after `opened` or the failure's
// alert, with the addresses that waited for it (nil when cancelled).
+ (instancetype)connectTo:(NSString *)address
                   opened:(void (^)(MCSessionLinkRef *link))opened
                    ended:(void (^)(NSArray<NSString *> *queued))ended;
// Every connect under way, oldest first.
+ (NSArray<MCRemoteConnect *> *)pending;
// The one under way for the same machine, account and path as `address`
// (MCSession::remoteName), or for the same machine and account.
+ (MCRemoteConnect *)pendingForAddress:(NSString *)address;
+ (MCRemoteConnect *)pendingOnMachineOf:(NSString *)address;

@property(nonatomic, readonly, copy) NSString *address;
@property(nonatomic, readonly) NSWindow *window;
// What the window's line says now, every line it has said, and how far it
// got (a stage may pass between two looks: progress.passed says it did).
@property(nonatomic, readonly, copy) NSString *progressLine;
@property(nonatomic, readonly) NSArray<NSString *> *linesShown;
@property(nonatomic, readonly) Shared::ConnectProgress progress;
// ssh's pid while it connects, else 0.
@property(nonatomic, readonly) pid_t commandPid;
// Opened again once this one ends (not when it is cancelled).
- (void)queueAddress:(NSString *)address;
@property(nonatomic, readonly) NSArray<NSString *> *queued;
- (IBAction)cancel:(id)sender;
@end
