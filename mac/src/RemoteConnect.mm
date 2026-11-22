// RemoteConnect.mm — Objective-C++. See RemoteConnect.h.
#import "RemoteConnect.h"
#import "SessionLink.h"

#include <memory>
#include <string>

static NSMutableArray<MCRemoteConnect *> *Pending(void) {
    static NSMutableArray<MCRemoteConnect *> *list;
    if (!list) list = [NSMutableArray array];
    return list;
}

static NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

@implementation MCRemoteConnect {
    std::shared_ptr<MCSession::Link> _link;
    Shared::SshTarget _target;
    Shared::ConnectProgress _progress;
    std::string _name, _machine;
    NSWindow *_window;
    NSTextField *_line;
    NSProgressIndicator *_spinner;
    NSButton *_cancelButton;
    NSTimer *_poll;
    NSMutableArray<NSString *> *_lines, *_queued;
    void (^_opened)(MCSessionLinkRef *);
    void (^_ended)(NSArray<NSString *> *);
    BOOL _over;   // ended: opened, failed and its alert answered, or cancelled
}

+ (NSArray<MCRemoteConnect *> *)pending {
    return [Pending() copy];
}

+ (MCRemoteConnect *)pendingForAddress:(NSString *)address {
    const std::string name = MCSession::remoteName(address.UTF8String ?: "");
    if (name.empty()) return nil;
    for (MCRemoteConnect *c in Pending())
        if (c->_name == name) return c;
    return nil;
}

+ (MCRemoteConnect *)pendingOnMachineOf:(NSString *)address {
    const std::string machine = MCSession::remoteMachine(address.UTF8String ?: "");
    if (machine.empty()) return nil;
    for (MCRemoteConnect *c in Pending())
        if (c->_machine == machine) return c;
    return nil;
}

+ (instancetype)connectTo:(NSString *)address
                   opened:(void (^)(MCSessionLinkRef *))opened
                    ended:(void (^)(NSArray<NSString *> *))ended {
    MCRemoteConnect *c = [[MCRemoteConnect alloc] initWithAddress:address opened:opened ended:ended];
    [c start];
    return c;
}

- (instancetype)initWithAddress:(NSString *)address
                         opened:(void (^)(MCSessionLinkRef *))opened
                          ended:(void (^)(NSArray<NSString *> *))ended {
    if (!(self = [super init])) return nil;
    _address = [address copy];
    _opened = [opened copy];
    _ended = [ended copy];
    _lines = [NSMutableArray array];
    _queued = [NSMutableArray array];
    const std::string a = address.UTF8String ?: "";
    Shared::parseSshUrl(a, &_target, nullptr);   // connectRemote says why when it is not one
    _name = MCSession::remoteName(a);
    _machine = MCSession::remoteMachine(a);
    [self makeWindow];
    return self;
}

- (void)makeWindow {
    const CGFloat W = 460, H = 118;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.releasedWhenClosed = NO;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.delegate = self;
    _window.title = [NSString stringWithFormat:@"%@ — mutible",
                                               _target.host.empty() ? _address : NS(Shared::sshLabel(_target))];
    NSView *content = _window.contentView;

    _spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, H - 44, 16, 16)];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    [content addSubview:_spinner];
    [_spinner startAnimation:nil];

    _line = [NSTextField labelWithString:@""];
    _line.font = [NSFont systemFontOfSize:13];
    _line.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _line.frame = NSMakeRect(46, H - 46, W - 66, 20);
    [content addSubview:_line];

    NSTextField *addr = [NSTextField labelWithString:_address];
    addr.font = [NSFont systemFontOfSize:11];
    addr.textColor = NSColor.secondaryLabelColor;
    addr.lineBreakMode = NSLineBreakByTruncatingMiddle;
    addr.frame = NSMakeRect(46, H - 66, W - 66, 16);
    [content addSubview:addr];

    _cancelButton = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    _cancelButton.keyEquivalent = @"\033";   // Esc
    _cancelButton.toolTip = @"Stop connecting: ssh is ended";
    [_cancelButton sizeToFit];
    const CGFloat bw = MAX(88, NSWidth(_cancelButton.frame));
    _cancelButton.frame = NSMakeRect(W - 20 - bw, 12, bw, NSHeight(_cancelButton.frame));
    [content addSubview:_cancelButton];
    [self showProgress];
}

- (void)start {
    [Pending() addObject:self];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    __weak MCRemoteConnect *weakSelf = self;
    _link = MCSession::connectRemote(_address.UTF8String ?: "",
                                     [weakSelf](std::shared_ptr<MCSession::Link> link, const std::string &why) {
                                         [weakSelf connected:link why:why];
                                     });
    // ssh says nothing to the main queue until the Welcome or its end, so
    // its standard error is looked at for `--stdio`'s start.
    _poll = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        MCRemoteConnect *s = weakSelf;
        if (!s || s->_over) {
            [t invalidate];
            return;
        }
        [s showProgress];
    }];
    [NSRunLoop.mainRunLoop addTimer:_poll forMode:NSRunLoopCommonModes];
}

- (void)showProgress {
    // The link's progress until this one ended (a link given straight away
    // stays at opening; this window says when it opened).
    if (_link && !_progress.ended()) _progress = _link->progress();
    NSString *line = NS(_progress.line(_target));
    if (![line isEqualToString:_line.stringValue]) {
        _line.stringValue = line;
        [_lines addObject:line];
    }
}

- (NSString *)progressLine { return _line.stringValue; }
- (NSArray<NSString *> *)linesShown { return [_lines copy]; }
- (Shared::ConnectProgress)progress { return _progress; }
- (pid_t)commandPid { return _link && !_progress.ended() ? _link->commandPid() : 0; }
- (NSArray<NSString *> *)queued { return [_queued copy]; }
- (NSWindow *)window { return _window; }

- (void)queueAddress:(NSString *)address {
    if (address) [_queued addObject:address];
}

- (void)connected:(std::shared_ptr<MCSession::Link>)link why:(const std::string &)why {
    if (_over || _progress.stage() == Shared::ConnectProgress::Stage::Cancelled) return;
    if (!link) {
        if (_link && !_progress.ended()) _progress = _link->progress();
        _progress.failed();
        _link.reset();
        [self showProgress];
        [_spinner stopAnimation:nil];
        _cancelButton.enabled = NO;
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"Could not open %@", _address];
        a.informativeText = NS(why);
        __weak MCRemoteConnect *weakSelf = self;
        [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse) { [weakSelf endWith:YES]; }];
        return;
    }
    // A live link given straight away (another window made it meanwhile)
    // was never this one's to watch.
    _link = link;
    _progress = link->progress();
    if (_progress.stage() == Shared::ConnectProgress::Stage::Connecting ||
        _progress.stage() == Shared::ConnectProgress::Stage::Starting)
        _progress.welcomed(link->folder());
    [self showProgress];
    // "Opening …" drawn before the window on the folder is made, which reads
    // through the host on the main thread.
    __weak MCRemoteConnect *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        MCRemoteConnect *s = weakSelf;
        if (!s || s->_over || s->_progress.stage() != Shared::ConnectProgress::Stage::Opening) return;
        if (s->_opened) s->_opened([[MCSessionLinkRef alloc] initWithLink:s->_link]);
        s->_progress.opened();
        [s endWith:YES];
    });
}

// The end: the window closed, taken off the list, and `ended` told (with
// the addresses that waited, unless cancelled).
- (void)endWith:(BOOL)reopenQueued {
    if (_over) return;
    _over = YES;
    [_poll invalidate];
    _poll = nil;
    [_spinner stopAnimation:nil];
    _window.delegate = nil;
    [_window orderOut:nil];
    [_window close];
    _link.reset();
    MCRemoteConnect *keep = self;   // the list may hold the last reference
    [Pending() removeObject:self];
    void (^ended)(NSArray<NSString *> *) = _ended;
    _ended = nil;
    _opened = nil;
    if (ended) ended(reopenQueued ? [_queued copy] : nil);
    (void)keep;
}

- (IBAction)cancel:(id)sender {
    if (_over) return;
    if (!_progress.cancel()) return;   // opened already
    if (_link) _link->cancelConnecting();   // ssh's group killed by its pid
    [self showProgress];
    [self endWith:NO];
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    [self cancel:sender];
    return NO;   // cancel: closed it already
}

@end
