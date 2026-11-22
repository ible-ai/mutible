// LspServer.h — one language server process (MCLspServer), and turning
// a command line into the program to run. For the Lsp files only.
#import <Cocoa/Cocoa.h>
#include "LspClient.h"
#include "SessionLsp.h"
#include <string>
#include <vector>

@class MCLspRemoteHost;

// ------------------------------------------------------------- MCLspServer
// One language server process. The client state machine runs on the main
// thread; bytes are read on a background queue and handed over, and writes
// go through a serial queue so a server that is slow to read never blocks
// typing.
//
// Or, in a window over SSH (-initWithKey:remote:request:), one its host
// runs on the other machine (LspRemote.h, SessionLsp.h): the client's
// bytes go to the host as LspInput, the host's LspOutput is framed and
// parsed as a pipe's would be, and its pid is the other machine's (never
// signalled here). It is "running" from the request on; the host's answer
// says what started (onStarted, with the command found there and the
// project-code check's verdict) or why nothing did (refusal).
@interface MCLspServer : NSObject
@property(nonatomic, readonly, copy) NSString *key;   // "python", or "python.also"
// Unique among every server this process starts ("python.also#3"): what its
// diagnostics are kept under.
@property(nonatomic, readonly, copy) NSString *serverId;
@property(nonatomic, readonly, copy) NSString *name;   // for the status bar: "pyright"
@property(nonatomic, readonly, copy) NSString *program;
@property(nonatomic, readonly, copy) NSArray<NSString *> *argv;
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) BOOL stopping;
@property(nonatomic, readonly) pid_t pid;
@property(nonatomic, readonly) NSDate *started;
// Once it has ended: "exited with status 1", "killed by signal 9", with the
// last line it wrote to stderr.
@property(nonatomic, readonly, copy) NSString *exitDescription;
@property(nonatomic, readonly) BOOL processRunning;   // NSTask's view, any thread
// It stopped reading its input until too much waited (64 MB): nothing more
// is sent, and it was killed.
@property(nonatomic, readonly) BOOL unresponsive;
@property(nonatomic, readonly) size_t queuedBytes;   // sent, not yet read
@property(nonatomic, copy) void (^onExit)(MCLspServer *server);
- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root;
// A server the window's host runs (nil when the host cannot be asked):
// `request`'s commands are tried there in order. `name` is what the status
// bar calls it until the host says which command it found.
- (instancetype)initWithKey:(NSString *)key remote:(MCLspRemoteHost *)host
                    request:(const Shared::ServerRequest &)request name:(NSString *)name;
@property(nonatomic, readonly) BOOL remote;
// The host started it: the command found there and what the folder's code
// allows (Shared::ServerStarted). Called once, before any output.
@property(nonatomic, copy) void (^onStarted)(MCLspServer *server, const Shared::ServerStarted &started);
@property(nonatomic, readonly) BOOL remoteStarted;
// Why the host never started it ("missing", "waits", "failed", "busy"), or
// "lost" when the connection went (started or not); nil otherwise. With
// what the host said (the programs looked for, the file that decided).
@property(nonatomic, readonly, copy) NSString *refusal;
@property(nonatomic, readonly) Json refusalInfo;
// From MCLspRemoteHost.
- (void)remoteStartedWithPid:(int)pid info:(const Json &)info;
- (void)remoteWrote:(const std::string &)bytes;
- (void)remoteExited:(int)status why:(const std::string &)why info:(const Json &)info;
- (Lsp::Client &)client;
- (void)stop;       // shutdown, exit, then signals if it lingers
- (void)stopNow;    // for app quit: no waiting on the event loop
@end

// A command line from the settings (or the defaults) -> program path plus
// arguments, or nil when the program is not installed.
NSArray<NSString *> *MCResolveCommand(const std::string &command,
                                      NSArray<NSString *> *first);
std::vector<std::string> ArgvWords(NSArray<NSString *> *argv);
// How much may wait for a server to read before it is taken as hung (tests;
// 0 puts back the default, 64 MB).
void MCLspSetMaxQueuedBytes(size_t bytes);
// How a server is named in the status bar: pyright, not pyright-langserver.
NSString *ServerDisplayName(NSString *key, NSArray<NSString *> *argv);
