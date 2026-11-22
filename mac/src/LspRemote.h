// LspRemote.h — Objective-C++. A window over SSH's language servers run by
// its host (stage 9, docs/notes/lsp.md, "Over SSH"; the core's
// SessionLsp.h). MCLspRemoteHost is the window's host as the place its
// servers run: an MCLspServer made with -initWithKey:remote:request: sends
// its client's bytes as LspInput through it and hears LspStarted,
// LspOutput and LspExited from it, so the LspClient above (completion,
// hover, diagnostics, rename, inlay hints, semantic tokens, formatting
// through the server) is the same as for a server on this Mac. The host
// also says which files changed on its disk (LspFiles), which the session
// passes to its servers' watchers in place of FSEvents. Its link is the
// window's, moved by Reconnect (attachLink:). Main thread.
#pragma once
#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Json.h"
#include "LspClient.h"

@class MCSessionLinkRef, MCLspServer;

@interface MCLspRemoteHost : NSObject
- (instancetype)initWithLink:(MCSessionLinkRef *)link window:(uint64_t)window;
// The window's link again after Reconnect (servers on the old one heard
// "lost" when it went).
- (void)attachLink:(MCSessionLinkRef *)link;
// The window closes: its servers are stopped there, nothing more is heard.
- (void)detach;
// The machine as the window's title names it ("thinkpad:~/proj"), and its
// machine alone ("thinkpad"), for what the window says.
@property(nonatomic, readonly, copy) NSString *label;
@property(nonatomic, readonly, copy) NSString *machine;
// The folder's key for the answers kept per folder (server_consent):
// the link's ssh:// machine and folder, never a path of this Mac's.
@property(nonatomic, readonly, copy) NSString *consentKey;
// Connected now, and whether that host runs language servers (Welcome
// flag 512).
@property(nonatomic, readonly) BOOL connected;
@property(nonatomic, readonly) BOOL runsServers;
// LspStart for `server` (`request`: SessionLsp.h's serverRequestJson):
// its id, or 0, sending nothing, when the host does not or is gone.
- (uint64_t)startServer:(MCLspServer *)server request:(Json)request;
- (void)send:(std::string)bytes toServer:(uint64_t)sid;
- (void)stopServer:(uint64_t)sid;
// Files the host saw change (absolute paths there), on the main queue.
@property(nonatomic, copy) void (^onFiles)(const std::vector<Lsp::FileChange> &changes);
// For checks: servers started through it, and LspFiles batches heard.
@property(nonatomic, readonly) NSUInteger started, fileBatches;
@end
