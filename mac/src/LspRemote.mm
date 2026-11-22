// LspRemote.mm — see LspRemote.h.
#import "LspRemote.h"
#import "LspServer.h"
#import "SessionLink.h"

#include <map>
#include <memory>
#include <set>

#include "SessionLsp.h"

@interface MCLspRemoteHost ()
- (void)server:(uint64_t)sid startedWithPid:(int)pid info:(const Json &)info;
- (void)server:(uint64_t)sid wrote:(const std::string &)bytes;
- (void)server:(uint64_t)sid exited:(int)status why:(const std::string &)why info:(const Json &)info;
@end

namespace {
// The client's listener for this window's servers: each message to the
// MCLspRemoteHost, which finds the server by its id. Main thread (the
// link dispatches there).
struct Relay : Shared::Client::ServerListener {
    __weak MCLspRemoteHost *owner = nil;
    void serverStarted(std::uint64_t id, int pid, const Json &started) override {
        [owner server:id startedWithPid:pid info:started];
    }
    void serverOutput(std::uint64_t id, const std::string &bytes) override { [owner server:id wrote:bytes]; }
    void serverExited(std::uint64_t id, int status, const std::string &why, const Json &info) override {
        [owner server:id exited:status why:why info:info];
    }
};
}  // namespace

@implementation MCLspRemoteHost {
    MCSessionLinkRef *_link;
    uint64_t _window;
    int _filesToken;
    std::unique_ptr<Relay> _relay;
    // Each server by its id on the current link, held weakly: a server
    // gone is simply not told.
    NSMapTable<NSNumber *, MCLspServer *> *_servers;
    // Every id the client holds for this window (the map may have lost a
    // server already): each let go of there before the relay goes.
    std::set<uint64_t> _ids;
}

- (instancetype)initWithLink:(MCSessionLinkRef *)link window:(uint64_t)window {
    if ((self = [super init])) {
        _window = window;
        _relay = std::make_unique<Relay>();
        _relay->owner = self;
        _servers = [NSMapTable strongToWeakObjectsMapTable];
        [self attachLink:link];
    }
    return self;
}

- (void)dealloc {
    [self detach];
}

- (MCSession::Link *)live {
    MCSession::Link *l = _link.link.get();
    return l && !l->lost() ? l : nullptr;
}

- (void)attachLink:(MCSessionLinkRef *)link {
    if (_link.link && _filesToken) _link.link->removeFilesListener(_filesToken);
    _filesToken = 0;
    // Servers on the old link heard "lost" when it went; any it still names
    // are let go of there.
    [self forgetAll];
    _link = link;
    if (!_link.link) return;
    _label = [@(_link.link->label().c_str()) copy];
    _machine = [@(_link.link->target().host.c_str()) copy];
    if (!_machine.length) _machine = _label;
    _consentKey = [@(_link.link->key().c_str()) copy];
    __weak MCLspRemoteHost *weakSelf = self;
    const uint64_t window = _window;
    _filesToken = _link.link->addFilesListener([weakSelf, window](std::uint64_t w, const Json &files) {
        if (w && window && w != window) return;
        std::vector<Lsp::FileChange> changes = Shared::filesFrom(files);
        if (changes.empty()) return;
        // Told inside the link's dispatch, which may be inside a wait: the
        // servers hear it on the next turn.
        auto copy = std::make_shared<std::vector<Lsp::FileChange>>(std::move(changes));
        dispatch_async(dispatch_get_main_queue(), ^{
            MCLspRemoteHost *s = weakSelf;
            if (!s) return;
            s->_fileBatches++;
            if (s.onFiles) s.onFiles(*copy);
        });
    });
}

// Every server of this window let go of on the link it was started on
// (stopped there when it still runs), nothing more told.
- (void)forgetAll {
    if (MCSession::Link *l = _link.link.get())
        for (uint64_t sid : _ids) l->client().forgetServer(sid);
    _ids.clear();
    [_servers removeAllObjects];
}

- (void)detach {
    [self forgetAll];
    if (_link.link && _filesToken) _link.link->removeFilesListener(_filesToken);
    _filesToken = 0;
    _link = nil;
}

- (BOOL)connected { return [self live] != nullptr; }
- (BOOL)runsServers {
    MCSession::Link *l = [self live];
    return l && l->runsServers();
}

- (uint64_t)startServer:(MCLspServer *)server request:(Json)request {
    MCSession::Link *l = [self live];
    if (!l || !l->runsServers() || !server) return 0;
    const uint64_t sid = l->client().newId(2000);
    if (!sid) return 0;
    [_servers setObject:server forKey:@(sid)];
    if (!l->client().startServer(sid, _window, std::move(request), _relay.get())) {
        [_servers removeObjectForKey:@(sid)];
        return 0;
    }
    _ids.insert(sid);
    _started++;
    return sid;
}

- (void)send:(std::string)bytes toServer:(uint64_t)sid {
    if (MCSession::Link *l = [self live]) l->client().serverInput(sid, std::move(bytes));
}

- (void)stopServer:(uint64_t)sid {
    if (MCSession::Link *l = [self live]) l->client().stopServer(sid);
}

// ---- from the relay

- (void)server:(uint64_t)sid startedWithPid:(int)pid info:(const Json &)info {
    [[_servers objectForKey:@(sid)] remoteStartedWithPid:pid info:info];
}

- (void)server:(uint64_t)sid wrote:(const std::string &)bytes {
    [[_servers objectForKey:@(sid)] remoteWrote:bytes];
}

- (void)server:(uint64_t)sid exited:(int)status why:(const std::string &)why info:(const Json &)info {
    MCLspServer *s = [_servers objectForKey:@(sid)];
    [_servers removeObjectForKey:@(sid)];
    _ids.erase(sid);
    [s remoteExited:status why:why info:info];
}
@end
