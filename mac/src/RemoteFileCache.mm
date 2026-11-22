// RemoteFileCache.mm — see RemoteFileCache.h.
#import "RemoteFileCache.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <vector>

#include "RemoteCache.h"

static NSString *Str(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

@implementation MCRemoteFileCache {
    NSString *_folder;
    std::map<std::string, std::uint64_t> _prints;   // remote path -> its copy's fingerprint
    NSMutableSet<NSString *> *_stale;
    NSMutableSet<NSString *> *_again;   // told of a change while being read
}

+ (NSString *)root {
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"mutible-ssh-copies"];
}

- (instancetype)initWithRemoteFolder:(NSString *)remoteFolder {
    if ((self = [super init])) {
        _remoteFolder = [remoteFolder copy];
        _stale = [NSMutableSet set];
        _fetching = [NSMutableSet set];
        _failed = [NSMutableSet set];
        _again = [NSMutableSet set];
    }
    return self;
}

- (NSString *)folder { return _folder; }

// This window's folder, made 0700 (and the folder of folders) the first time.
- (NSString *)madeFolder {
    if (_folder) return _folder;
    static std::atomic<std::uint64_t> serial{0};
    NSString *root = [MCRemoteFileCache root];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSDictionary *privateAttrs = @{NSFilePosixPermissions: @0700};
    if (![fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:privateAttrs error:nil])
        return nil;
    chmod(root.fileSystemRepresentation, 0700);   // made before, by an older umask
    for (int tries = 0; tries < 100; tries++) {
        NSString *name = Str(RemoteCache::windowFolderName((long)getpid(), ++serial));
        NSString *dir = [root stringByAppendingPathComponent:name];
        // mkdir, not "with intermediate directories": one there already
        // (another window's, a process's with this pid before) is never taken.
        if (mkdir(dir.fileSystemRepresentation, 0700) == 0) {
            _folder = dir;
            return _folder;
        }
        if (errno != EEXIST) return nil;
    }
    return nil;
}

- (NSString *)localPathOf:(NSString *)remotePath {
    if (!remotePath.length || !_remoteFolder.length) return nil;
    const std::string place =
        RemoteCache::placeOf(_remoteFolder.fileSystemRepresentation, remotePath.fileSystemRepresentation);
    if (place.empty() || !_folder) return nil;
    return [_folder stringByAppendingPathComponent:Str(place)];
}

- (BOOL)hasCopyOf:(NSString *)remotePath {
    return remotePath && _folder && _prints.count(remotePath.fileSystemRepresentation) &&
           [NSFileManager.defaultManager fileExistsAtPath:[self localPathOf:remotePath]];
}

- (BOOL)isStale:(NSString *)remotePath {
    return remotePath && [_stale containsObject:remotePath];
}

- (NSString *)store:(const std::string &)bytes of:(NSString *)remotePath {
    if (![self madeFolder]) return nil;
    NSString *path = [self localPathOf:remotePath];
    if (!path) return nil;
    NSString *dir = path.stringByDeletingLastPathComponent;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                  attributes:@{NSFilePosixPermissions: @0700}
                                                       error:nil])
        return nil;
    // A temporary file beside it, renamed over it: a viewer reading the old
    // copy keeps it whole, and the new one is never half written.
    std::string tmp = (dir.fileSystemRepresentation + std::string("/.copy-XXXXXX"));
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());   // 0600
    if (fd < 0) return nil;
    const char *p = bytes.data();
    size_t left = bytes.size();
    bool ok = true;
    while (left > 0) {
        const ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) != 0) ok = false;
    if (!ok || rename(name.data(), path.fileSystemRepresentation) != 0) {
        unlink(name.data());
        return nil;
    }
    _prints[remotePath.fileSystemRepresentation] = RemoteCache::fingerprint(bytes);
    [_stale removeObject:remotePath];
    return path;
}

- (BOOL)localCopyOf:(NSString *)remotePath holds:(const std::string &)bytes {
    auto it = remotePath ? _prints.find(remotePath.fileSystemRepresentation) : _prints.end();
    return it != _prints.end() && it->second == RemoteCache::fingerprint(bytes);
}

- (NSArray<NSString *> *)foldersChanged:(NSArray<NSString *> *)folders {
    std::vector<std::string> changed;
    for (NSString *f in folders) changed.push_back(f.fileSystemRepresentation);
    for (NSString *p in [_failed allObjects])
        if (RemoteCache::mayHaveChanged(changed, p.fileSystemRepresentation)) [_failed removeObject:p];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const auto &[path, print] : _prints) {
        (void)print;
        if (!RemoteCache::mayHaveChanged(changed, path)) continue;
        NSString *p = Str(path);
        [_stale addObject:p];
        if ([_fetching containsObject:p]) [_again addObject:p];
        [out addObject:p];
    }
    return out;
}

- (void)markFresh:(NSString *)remotePath {
    if (remotePath) [_stale removeObject:remotePath];
}

- (void)markStale:(NSString *)remotePath {
    if (remotePath) [_stale addObject:remotePath];
}

- (BOOL)takeAgain:(NSString *)remotePath {
    if (!remotePath || ![_again containsObject:remotePath]) return NO;
    [_again removeObject:remotePath];
    return YES;
}

- (NSArray<NSString *> *)remoteFolders {
    NSMutableOrderedSet<NSString *> *dirs = [NSMutableOrderedSet orderedSet];
    for (const auto &[path, print] : _prints) {
        (void)print;
        [dirs addObject:Str(path).stringByDeletingLastPathComponent];
    }
    return dirs.array;
}

- (void)removeAll {
    if (_folder) [NSFileManager.defaultManager removeItemAtPath:_folder error:nil];
    _folder = nil;
    _prints.clear();
    [_stale removeAllObjects];
    [_failed removeAllObjects];
    [_again removeAllObjects];
}

- (void)dealloc {
    [self removeAll];
}

+ (void)removeLeftovers {
    NSString *root = [self root];
    NSFileManager *fm = [NSFileManager new];
    for (NSString *name in [fm contentsOfDirectoryAtPath:root error:nil] ?: @[]) {
        long pid = 0;
        if (!RemoteCache::ownerOfFolder(name.UTF8String ?: "", &pid)) continue;
        if (pid == (long)getpid()) continue;
        // Removed only when no process has that id now.
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
        [fm removeItemAtPath:[root stringByAppendingPathComponent:name] error:nil];
    }
}

@end
