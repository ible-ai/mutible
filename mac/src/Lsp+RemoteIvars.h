// Lsp+RemoteIvars.h — the instance variables only Lsp+Remote.mm uses,
// which imports it, as does Lsp.mm, which lays them out. See the rule at
// the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@class MCLspRemoteHost;

@interface LspSession () {
    // A window over SSH's host, which runs its servers (LspRemote.h); nil
    // in a window on this Mac.
    MCLspRemoteHost *_remoteHost;
    // What the status bar has said once in this window (a server not
    // installed there, the host too old), so it is not said at every look.
    NSMutableSet<NSString *> *_remoteNoted;
    // A server not installed there, by key: when the host last said so and
    // its words, so a tab switch (which attaches the file again) asks the
    // host again only now and then.
    NSMutableDictionary<NSString *, NSArray *> *_remoteMissing;
}
@end
