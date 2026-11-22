// Lsp+Remote.mm — Objective-C++. LspSession (Remote): a window over SSH's
// language servers, run by its host on the other machine (stage 9,
// docs/notes/lsp.md, "Over SSH"; LspRemote.h, the core's SessionLsp.h).
// Everything above the transport is the session's as for a server on this
// Mac (the same Lsp::Client, the same callbacks, wireServer:); what differs
// is starting one and what its end means:
//   - the settings' commands (lsp.<language>, else the defaults) are sent
//     unresolved, and the host finds the first there (its PATH and the
//     places package managers put servers), so nothing is looked for on
//     this Mac; the client is initialized once the host says what started,
//     with the folder as the host names it and no parent process;
//   - the folder's answer about running its own code (server_consent) is
//     kept per host and folder (consentRoot), and the host checks what
//     the folder holds on its own disk: it starts rust-analyzer restricted
//     or not at all, and says which file decided;
//   - one not installed there says so, naming the host and the programs
//     looked for, once in the status bar and on its card; no install bar
//     (this Mac's commands would not put it there);
//   - a dropped connection ends the window's servers there (the host ends
//     them with the connection), counted as no crash: each starts again
//     once the window is connected again (remoteHostReconnected), which
//     opens its documents anew;
//   - files changed there come from the host's disk watching (LspFiles) in
//     place of FSEvents (remoteFilesChanged:).
#import "Lsp+Private.h"
#import "Lsp+RemoteIvars.h"
#import "LspRemote.h"

#include "SessionLsp.h"

@implementation LspSession (Remote)

- (BOOL)isRemote { return _remoteHost != nil; }
- (MCLspRemoteHost *)remoteHost { return _remoteHost; }

- (NSString *)consentRoot {
    return _remoteHost.consentKey.length ? _remoteHost.consentKey : _root;
}

- (void)useRemoteHost:(MCLspRemoteHost *)host {
    if (!host || _remoteHost == host) return;
    // Anything started before it belongs to this Mac (the window takes its
    // link before its first file, so there should be none).
    NSString *path = _path;
    if (_entries.count) {
        [self closeDocument];
        [self stopAllServers];
    }
    _remoteHost = host;
    _remoteNoted = [NSMutableSet set];
    __weak LspSession *weakSelf = self;
    host.onFiles = ^(const std::vector<Lsp::FileChange> &changes) { [weakSelf remoteFilesChanged:changes]; };
    if (path) [self documentOpened:path];
}

// The host's machine, for what the window says.
- (std::string)remoteMachine {
    NSString *m = _remoteHost.machine.length ? _remoteHost.machine : @"the other machine";
    return Std(m);
}

// Said in the status bar once in this window (the card says it for as long
// as it holds).
- (void)noteRemote:(NSString *)message once:(NSString *)key {
    if (!message.length || [_remoteNoted containsObject:key]) return;
    [_remoteNoted addObject:key];
    [self note:message];
}

// How often a server not installed there is looked for again unasked.
static const NSTimeInterval kMissingAskedAgain = 30;

- (MCLspServer *)startRemoteEntry:(MCLspEntry *)e {
    const std::string key = Std(e.key);
    // Not installed there a moment ago: said again without asking (Start
    // and Restart, which make it idle first, ask at once).
    NSArray *missing = _remoteMissing[e.key];
    if (e.state == MCLspStateNotInstalled && missing &&
        -[(NSDate *)missing[0] timeIntervalSinceNow] < kMissingAskedAgain) {
        e.message = missing[1];
        return nil;
    }
    const std::vector<std::string> commands = [self commandsForKey:key];
    Shared::ServerRequest r;
    r.key = key;
    for (const std::string &c : commands) {
        std::vector<std::string> words = Lsp::splitCommand(c);
        if (!words.empty()) r.commands.push_back(std::move(words));
    }
    if (r.commands.empty()) {
        e.state = MCLspStateNotInstalled;
        return nil;
    }
    NSMutableArray *first = [NSMutableArray array];
    for (const std::string &w : r.commands.front()) [first addObject:NS(w)];
    if (!e.serverName) e.serverName = ServerDisplayName(e.key, first);
    if (!_remoteHost.connected) {
        // Started once the window is connected again.
        e.state = MCLspStateRestarting;
        e.waitsForHost = YES;
        e.message = NS(Shared::serversLostText([self remoteMachine]));
        return nil;
    }
    if (!_remoteHost.runsServers) {
        e.state = MCLspStateFailed;
        e.message = NS(Shared::serversTooOldText(Std(_remoteHost.label ?: _remoteHost.machine)));
        [self noteRemote:e.message once:@"too old"];
        return nil;
    }
    // The folder's answer, and the file's folder (cargo reads its
    // configuration from where the crate is, up); the host checks the rest.
    r.allowed = [self folderEnvironmentAllowed];
    Lsp::Language lang;
    if (_path && [self language:lang forPath:_path installed:NO] && lang.server == key)
        r.fileDir = Std(_path.stringByDeletingLastPathComponent);
    MCLspServer *server = [[MCLspServer alloc] initWithKey:e.key remote:_remoteHost request:r name:nil];
    if (!server) {
        e.state = MCLspStateFailed;
        e.message = [NSString stringWithFormat:@"%@ could not be asked to start it", _remoteHost.machine];
        return nil;
    }
    e.server = server;
    e.serverName = server.name;
    e.state = MCLspStateStarting;
    e.command = NS(commands.front());
    e.projectCode = Lsp::ProjectCode::Free;
    e.projectCodeFile = nil;
    e.waitsForHost = NO;
    [self wireServer:server];
    __weak LspSession *weakSelf = self;
    server.onStarted = ^(MCLspServer *s, const Shared::ServerStarted &started) {
        [weakSelf remoteServer:s started:started];
    };
    return server;
}

// The host started it: what it found, and the client's handshake.
- (void)remoteServer:(MCLspServer *)server started:(const Shared::ServerStarted &)started {
    MCLspEntry *e = [self entryOf:server];
    if (!e || server.stopping) return;
    e.serverName = server.name;
    const std::vector<std::string> commands = [self commandsForKey:Std(e.key)];
    if (started.command >= 0 && (size_t)started.command < commands.size())
        e.command = NS(commands[(size_t)started.command]);
    e.projectCode = started.code.verdict;
    e.projectCodeFile = started.code.file.empty() ? nil : NS(started.code.file);
    Lsp::Client &c = server.client;
    if ([e.key isEqualToString:@"python"]) {
        // No interpreter of this Mac's: the server finds its own there
        // (basedpyright reads a root .venv by itself).
        Lsp::PythonServer kind = Lsp::pythonServerKind(ArgvWords(server.argv));
        e.pythonKind = kind;
        e.interpreter = nil;
        e.interpreterSource = @"the server's own, on the host";
        c.setInitializationOptions(Lsp::pythonInitializationOptions(kind, ""));
        c.setSettings(Lsp::pythonSettings(kind, ""));
    }
    if (started.code.verdict == Lsp::ProjectCode::Restricted) {
        c.setInitializationOptions(started.code.initializationOptions);
        if (!started.code.settings.isNull()) c.setSettings(started.code.settings);
    }
    // The folder as the host names it (its real path: the window's folder),
    // and no parent process: a pid of this Mac's names nothing there.
    c.initialize(Std(_root), 0);
    [self changed];
}

- (BOOL)remoteServerEnded:(MCLspServer *)server {
    NSString *why = server.refusal;
    if (!why.length) return NO;   // it ran and ended by itself: a crash, as here
    MCLspEntry *e = [self entryOf:server];
    [self forgetDocumentsOf:server];
    [self finishWorkspaceSymbolsOf:server];
    if (e) {
        e.server = nil;
        e.generation++;
        const Json &info = server.refusalInfo;
        if ([why isEqualToString:@"lost"]) {
            e.state = MCLspStateRestarting;
            e.waitsForHost = YES;
            e.message = NS(Shared::serversLostText([self remoteMachine]));
        } else if ([why isEqualToString:@"missing"]) {
            std::vector<std::string> tried;
            for (const Json &t : info["tried"].elements())
                if (t.isString()) tried.push_back(t.asString());
            e.state = MCLspStateNotInstalled;
            e.message = NS(Shared::serverMissingText(Std(e.serverName ?: server.name), [self remoteMachine], tried));
            if (!_remoteMissing) _remoteMissing = [NSMutableDictionary dictionary];
            _remoteMissing[e.key] = @[ [NSDate date], e.message ];
            [self noteRemote:e.message once:[@"missing " stringByAppendingString:e.key]];
        } else if ([why isEqualToString:@"waits"]) {
            e.state = MCLspStateWaiting;
            e.projectCode = Lsp::ProjectCode::Waits;
            e.projectCodeFile = info["file"].asString().empty() ? nil : NS(info["file"].asString());
            e.message = [self projectCodeQuestionForKey:e.key];
        } else {
            e.state = MCLspStateFailed;
            e.message = server.exitDescription;
        }
    }
    [self dropServer:server];
    [self changed];
    return YES;
}

- (void)remoteHostReconnected {
    if (!_remoteHost) return;
    BOOL any = NO;
    for (MCLspEntry *e in _entries.allValues) {
        // Ended by the drop, or never started for the host's being away;
        // and one the old host was too old for (the new one may not be).
        const BOOL tooOld = e.state == MCLspStateFailed && !e.server &&
                            [e.message isEqualToString:NS(Shared::serversTooOldText(
                                                           Std(_remoteHost.label ?: _remoteHost.machine)))];
        if (!e.waitsForHost && !tooOld) continue;
        e.waitsForHost = NO;
        e.crashes = 0;
        e.message = nil;
        e.state = MCLspStateIdle;
        any = YES;
    }
    if (!any) return;
    [self attachDocument];   // starts the file's servers (ensureServer) and opens its tabs there
    [self changed];
}

// Files the host saw change on its disk: to each server's watchers, as a
// batch from FSEvents would go (matched here: a remote batch is small).
- (void)remoteFilesChanged:(const std::vector<Lsp::FileChange> &)changes {
    if (changes.empty()) return;
    [self filesChangedOnDisk:changes batches:std::vector<Lsp::WatchedBatch>()];
}

@end
