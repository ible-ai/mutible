// SessionLink.h — the GTK app's links to the session's hosts (SESSION-DESIGN.md,
// stages 4a and 6; the core's SessionHost.h, SessionClient.h and
// SessionServer.h). A host owns every open document's text and undo
// history and every terminal's shell; the editor's documents are its
// mirrors (Document.h records each buffer edit into the client, applies
// what the host sends back, and asks the host for undo), the terminals feed
// VTE what the host forwards, and the language server sync reads the host's
// text through the client's copy of it (LspSessionSync.cpp). The windows'
// tabs are reported as they change.
//
// A host serves one folder, so there is one Link per folder the windows
// show (resolved), shared by every window on it: a window takes its
// folder's link when it is made and again when it opens another folder
// (Editor::setSessionLink), and each document and terminal keeps the link
// it was opened on for its life (so a terminal started before Open Folder
// stays on the old folder's host). The link goes when the last of them lets
// it go. Each link has its own client, in code points (what GtkTextIter
// counts), its messages dispatched on the main loop as they arrive, and its
// ids from that client (Client::newId), so ids of two hosts never meet in
// one client.
//
// Where the host is, read when a link is made (MUTIBLE_HOST wins over the
// setting, for the checks and for forcing one):
//   - MUTIBLE_HOST=inprocess, or unset with editor.session-host off or on:
//     a SessionHost on a thread of this process, one per folder; kept
//     while it holds what windows that closed left (keepWhileHolding). The
//     default until the default flips on both apps together.
//   - MUTIBLE_HOST=loopback: the same, every message through the
//     protocol's encoding and back, both ways (MUTIBLE_HOST_DELAY_US delays
//     each on the host's thread, as a slow link would).
//   - MUTIBLE_HOST=socket, or unset with editor.session-host = headless: the
//     folder's headless host (`mutible host --folder`, SessionServer.h),
//     found by its socket or started as this program, detached, and
//     attached to over the socket. MUTIBLE_HOST_IDLE_MINUTES is passed on
//     as --idle-minutes (checks). When it cannot be had, the in-process host
//     for that folder, with one line on stderr saying why.
//
// A headless host that goes (killed, or its socket closed) is reported once
// on stderr and to the link's lost listeners (the windows' status bars, the
// terminals); its client then sends nothing and waits for nothing
// (Client::hostLost). The next window on the folder, or Reconnect in the
// status bar, finds or starts the host again (forFolder passes a lost link
// over) and moves the windows to it, their buffers opened on it anew.
// Quitting detaches from every headless host (detachAll), which keeps
// running until its lifetime ends.
//
// Windows detach and reattach (stage 6, step 2): a window closing leaves its
// documents and terminals on the host and its record (windowDetached); a
// window opening takes the latest record a closed window left, with what it
// names, and what no record names (takeForWindow).
//
// Saving (stage 7, docs/notes/session-host.md, "GTK's tabs save through the
// host"): every client says in its Hello that the windows save through the
// host (flag 16), and on a host that saves for apps
// (Client::hostSavesForApps) a tab's save is the host's write
// (Document::save), after everything the window asks. The host's notices
// about a document go to the document by its id (listen): its count of it
// as unsaved flipping (DocDirty), and its writes for another renderer (a
// page's save: DocDirty flag 2, Client::hearWrites).
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FolderSearch.h"
#include "SessionClient.h"
#include "SessionServer.h"

namespace Model {
class Window;
}
namespace Shared {
class SessionHost;
}

namespace SessionLink {

class Link : public std::enable_shared_from_this<Link> {
public:
    // Remote: `mutible host --stdio` on another machine through the user's
    // ssh (stage 8, connectRemote).
    enum class Kind { InProcess, Loopback, Socket, Remote };
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Shared::Client& client() { return *client_; }
    Kind kind() const { return kind_; }
    // The folder, resolved (as given when it is not one).
    const std::string& folder() const { return folder_; }
    // A headless host's socket, and the pid of the host this link started
    // (0 when it found one running, or for an in-process host).
    const std::string& socketPath() const { return socketPath_; }
    pid_t startedHost() const { return startedHost_; }
    // The headless host went away.
    bool lost() const { return client_->lost(); }

    // ---- a remote link (Kind::Remote, stage 8)
    // Which host this is, for moving windows to its replacement: the folder
    // for a link on this machine, the other machine's ssh:// and the folder
    // there for a remote one.
    std::string key() const { return kind_ == Kind::Remote ? machine_ + "\n" + folder_ : folder_; }
    // Its address as given (ssh://...), its title's "host:path" (the folder
    // as the address names it, ~ kept), the file the address named (spelled
    // under folder(), as the host resolved it; "" for a folder), and why the
    // connection went (ssh's status and last words, in the window's words)
    // once lost.
    const std::string& address() const { return address_; }
    const std::string& label() const { return label_; }
    const std::string& askedFile() const { return askedFile_; }
    const std::string& failure() const { return failure_; }
    // Which machine and account (Shared::sshMachineKey), the same for every
    // folder there.
    const std::string& machine() const { return machine_; }
    // While it connects (connectRemote, reconnectRemote): how far it got
    // (ssh's standard error heard for `--stdio`'s word that it started, the
    // Welcome), the address as parsed, and the command carrying it (ssh):
    // its pid, 0 when there is none. `onProgress` is called on the main
    // loop whenever the stage changes (one listener; the connecting window).
    bool connecting() const { return connecting_; }
    const Shared::ConnectProgress& progress() const { return progress_; }
    const Shared::SshTarget& target() const { return target_; }
    pid_t commandPid() const { return socket_ ? socket_->pid() : 0; }
    void onProgress(std::function<void()> f) { progressListener_ = std::move(f); }
    // Stops connecting: ssh's process group killed, the transport let go of,
    // the try's `done` never called. Nothing once it has connected.
    void cancelConnecting();
    // Where an ssh:// address's path (`asked`, "~", "~/x" or "/x", as the
    // target names it) is on this link's host, resolved there (FsRead
    // "locate", SessionFiles.h): inside this folder or not, and when inside
    // its path spelled under folder(), whether it is a folder, and whether
    // it is a file not there yet. False when the host did not say (gone, too
    // old for the read, no answer within `timeoutMs`). Waits on the host.
    struct Located {
        bool inside = false, dir = false, isNew = false;
        std::string path;
    };
    bool locate(const std::string& asked, Located* out, int timeoutMs = 5000);
    // The token of the window over SSH this link belongs to, the same on
    // every connection it makes again (Client::setResume), "" for no
    // remote link.
    const std::string& resume() const { return resume_; }
    // After a reconnect (stage 8, the lasting host): `window`'s record on
    // this link's host, when the host still holds the one `old` (the lost
    // link) had made for it; false when it ended meanwhile.
    bool resumeWindowFrom(const Link& old, std::uint64_t window);
    // A file's text read through the host (FsRead "read", piece by piece, 64
    // MB at most), decoded as a local file is (FolderSearch::decodeFileText).
    // `ok` false when it cannot be read or is not text; `tooLarge` past the
    // limit. Waits on the host (at most 20 s a piece). Never a stamp: the
    // file is on the other machine.
    struct RemoteText {
        bool ok = false, tooLarge = false;
        std::string text;
        FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
        bool bom = false;
    };
    RemoteText readText(const std::string& path);
    // The bytes of such a read, before decoding.
    struct RemoteBytes {
        bool ok = false, tooLarge = false;
        std::string bytes;
    };
    // ---- the host's reads without waiting (stage 8, second slice). The
    // host's answers are dispatched on the main loop (dispatchOnMain), so
    // these never block and their callbacks run there, from dispatch(),
    // never inside the call. A lost link answers each one still waiting
    // with failure at once.
    // One FsRead (Shared::Client::requestRead): `done(ok, answer)`. False,
    // calling nothing, when the link is lost.
    bool readAsync(Json request, std::function<void(bool ok, const Json& answer)> done);
    // The same for a "read": the piece's bytes come with the answer.
    using BytesFn = std::function<void(bool ok, const Json& answer, std::string&& bytes)>;
    bool readBytesAsync(Json request, BytesFn done);
    // One git command run by the host (Welcome flag 256, SessionGit.h:
    // Client::requestGit; `request` is Shared::gitRequest's, `input` the
    // command's standard input): `done(ok, answer, out)` as readBytesAsync's,
    // Shared::gitReplyFrom reading it. False, calling nothing, when the link
    // is lost or the host does not run git (runsGit).
    bool runsGit() const;
    bool gitAsync(Json request, std::string input, BytesFn done);
    // A tree's file operation on the host (Client::requestFileOp: rename,
    // move, delete, newFile, newFolder): `done(ok, answer)` as readAsync's,
    // the answer {"ok": true, ...} or {"error", "reason"}. False, calling
    // nothing, when the link is lost or the host does not do them
    // (Client::hostDoesFileOps: an older mutible there).
    bool fileOpAsync(Json request, std::function<void(bool ok, const Json& answer)> done);
    // A remote file's text without waiting (stage 8, third slice): the
    // pieces are asked one after another from the main loop, then
    // `done(text)` runs from dispatch() (never inside the call), with
    // ok false for a read that failed, or tooLarge. `*cancel` set stops it:
    // nothing more is asked and `done` is not called. False, calling
    // nothing, when the link is lost or the path is not under the folder.
    bool readTextAsync(const std::string& path, std::shared_ptr<std::atomic<bool>> cancel,
                       std::function<void(RemoteBytes&&)> done);
    // One of the heavier reads ("files", "search"; Client::requestStream):
    // `answers(batch, done)` for each batch as it comes, the last with done
    // true (also when the link is lost: an empty batch). 0, calling nothing,
    // when the host does not take them.
    using StreamFn = std::function<void(std::vector<Json>&& answers, bool done)>;
    std::uint64_t stream(Json request, StreamFn answers);
    // Replace in Folder (FsRead's "replace"), answered as a stream: 0,
    // calling nothing, when the host does not write for pages. Sent behind
    // the edits this window sent.
    std::uint64_t replace(Json request, StreamFn answers);
    // Stops one: the host is told, nothing more is called for it.
    void cancelStream(std::uint64_t req);
    // What the host saw change on disk (Hello flag 32; `mutible host
    // --stdio` watches, a remote link's client hears it): `folders` as
    // Client::hearDisk's, each on a turn of the main loop of its own, never
    // inside a dispatch. Only a remote link hears the disk.
    using DiskFn = std::function<void(std::uint64_t window, const Json& changed)>;
    int addDiskListener(DiskFn f);
    void removeDiskListener(int token);
    // Files the remote host saw change, for its language servers'
    // didChangeWatchedFiles (LspFiles, SessionLsp.h: sent while a server of
    // this link's runs there), with the host's id of the window its servers
    // named. Called inside a dispatch, so a listener defers what it does.
    using FilesFn = std::function<void(std::uint64_t window, const Json& files)>;
    int addFilesListener(FilesFn f);
    void removeFilesListener(int token);
    // Whether the host runs language servers for a window (Welcome flag 512,
    // SessionLsp.h): never for a link that is lost.
    bool runsServers() const { return !lost() && client_->hostRunsServers(); }
    // Reads and streams still waiting (checks).
    std::size_t pending() const { return reads_.size() + byteReads_.size() + streams_.size(); }

    // For checks: the in-process host, null on a headless one
    // (SessionHost::diskWatchSettled).
    Shared::SessionHost* inProcessHost() const { return host_.get(); }

    // The host's notices about one document of this link's (stage 7): its
    // count of it as unsaved flipped (DocDirty), and it wrote the file for
    // another renderer (DocDirty flag 2, with the host version written and
    // {path, encodingName, stamp}). Called on the main loop inside a
    // dispatch, which a wait (settle) can run in the middle of anything.
    class DocNotices {
    public:
        virtual void hostDirty(bool dirty) = 0;
        virtual void hostWrote(std::uint64_t version, const Json& json) = 0;

    protected:
        ~DocNotices() = default;
    };
    // `n` hears the notices about `doc` (its id on this link's host) until
    // unlisten; unlisten takes it off only while it is `n`'s.
    void listen(Shared::DocId doc, DocNotices* n) { notices_[doc] = n; }
    void unlisten(Shared::DocId doc, DocNotices* n);

    // Told once, on the main loop, when the host goes, with a sentence
    // saying so; the number takes it off again.
    int addLostListener(std::function<void(const std::string& message)> f);
    void removeLostListener(int token);

    // A window's tabs, sent when they differ from what was sent last, with
    // the host terminals its dock shows ("terms", as the Mac's window has)
    // and how they sit in its tabs and panes ("termLayout", the core's
    // Shared::termLayoutJson, TermDockRecord.h; null leaves it out).
    void windowTabs(std::uint64_t window, const Model::Window& w);
    void windowTerminals(std::uint64_t window, const Model::Window& w, std::vector<std::uint64_t> terms,
                         Json layout = Json());
    void windowClosed(std::uint64_t window);
    // What the window's browser pane holds ("browser", the core's
    // Shared::Web::browserReportJson) and which panes it shows ("panes",
    // panesReportJson), sent with its tabs from now on (when they differ)
    // for the session browser's page (SessionBrowser.h); null leaves either
    // out; and its commit and worktree pages ("reviews", reviewsReportJson).
    void windowPanes(std::uint64_t window, const Model::Window& w, Json browser, Json panes, Json reviews = Json());
    // The window's id on the host, the one its tabs are reported under,
    // made now when it has none (the session browser serves only its tabs).
    std::uint64_t hostWindowId(std::uint64_t window) { return hostWindow(window); }
    // A browser's view (the session browser's server, SessionWeb.h's
    // Options::connect) connected to this link's in-process host as one
    // more renderer, the host's messages for it delivered to `client` from
    // then on; returns what lets it go again. Any thread. Null for a
    // headless host, which the view reaches over its socket instead.
    std::function<void()> connectViewer(Shared::Client& client);
    // What the host sent, taken now (dispatched on the main loop otherwise):
    // a page's save waits for every edit it made to be in the tab.
    void dispatch();
    // The window closes and detaches (stage 6, step 2): its record stays on
    // the host, sent once more so it is the latest a closing window left,
    // for the folder's next window to take; none is kept for a window with
    // no tabs and no terminals.
    void windowDetached(std::uint64_t window, const Model::Window& w);

    // What the host holds that a window opening on the folder takes
    // (takeForWindow): the core's rules (Shared::reattachFrom, SessionClient.h:
    // the latest record a closed window left with what it names, then what
    // no record names; never two documents of one path; an untitled buffer
    // too, which the window names, Backups::untitledPath).
    using HeldDoc = Shared::Reattach::Doc;
    using HeldTerm = Shared::Reattach::Term;
    using Held = Shared::Reattach;
    // `window` takes the record over: its tabs are reported under the
    // record's id from now on.
    Held takeForWindow(std::uint64_t window);
    // Everything free (every free record, and what no window's record
    // names), for endDetached.
    Held allFree();

    // For the GUI checks: whether the host's text of each of `docs` (a
    // document's id and its buffer's text) is the buffer's, and the host
    // reported nothing wrong; and whether the window's tabs as the host has
    // them are the window's own. Empty when so, else what differs.
    std::string checkDocuments(const std::vector<std::pair<std::uint64_t, std::string>>& docs);
    std::string checkWindow(std::uint64_t window, const Model::Window& w);

    // Lets go of a headless host: the host forgets this renderer and keeps
    // the rest (quitting). Nothing is sent after it.
    void detach();

private:
    friend std::shared_ptr<Link> forFolder(const std::string& root, bool headless);
    friend std::shared_ptr<Link> connectRemote(const std::string& address,
                                               std::function<void(std::shared_ptr<Link>, std::string)> done);
    friend std::shared_ptr<Link> connectRemoteFor(const std::string& address, const std::shared_ptr<Link>& lost,
                                                  std::function<void(std::shared_ptr<Link>, std::string)> done);
    friend std::shared_ptr<Link> reconnectRemote(const std::shared_ptr<Link>& lost,
                                                 std::function<void(std::shared_ptr<Link>, std::string)> done);
    Held free(bool all);
    Json tabsJson(std::uint64_t window, const Model::Window& w) const;
    Link() = default;
    struct Wake;
    void startInProcess(bool loopback, unsigned delayUs);
    bool startSocket(std::string* why, bool onlyExisting = false);
    bool startRemote(std::string* why);
    void makeClient();
    static int dispatchOnMain(void* wake);   // a GSourceFunc
    std::uint64_t hostWindow(std::uint64_t window);

    Kind kind_ = Kind::InProcess;
    std::string folder_, socketPath_;
    std::string address_, label_, machine_, askedFile_, failure_, resume_;
    Shared::SshTarget target_;
    bool connecting_ = false, cancelled_ = false;
    Shared::ConnectProgress progress_;
    std::function<void()> progressListener_;
    pid_t startedHost_ = 0;
    std::shared_ptr<Wake> wake_;
    // Destroyed in this order (~Link): the transport first, whose thread
    // delivers to the client.
    std::unique_ptr<Shared::Client> client_;
    std::unique_ptr<Shared::SessionHost> host_;   // in process
    std::unique_ptr<Shared::HostLink> socket_;    // headless
    Shared::ClientId hostId_ = 0;
    bool detached_ = false, lostTold_ = false;
    std::map<Shared::DocId, DocNotices*> notices_;   // listen
    std::map<int, std::function<void(const std::string&)>> lostListeners_;
    // readAsync and stream: by the client's request id, polled at the end of
    // each dispatch (pumpStreams). The disk's notices wait for a turn of
    // the loop (flushDisk).
    void pumpStreams();
    void failPending();
    void flushDisk();
    std::map<std::uint64_t, std::function<void(bool, const Json&)>> reads_;
    std::map<std::uint64_t, BytesFn> byteReads_;
    std::map<std::uint64_t, StreamFn> streams_;
    std::map<int, DiskFn> diskListeners_;
    std::map<int, FilesFn> filesListeners_;
    std::vector<std::pair<std::uint64_t, Json>> diskQueue_;
    bool diskQueued_ = false;
    bool pumping_ = false;
    int nextListener_ = 0;
    // What each window's tabs were last sent as, and each window's id on
    // the host by its id in the app's Model::Session (which numbers windows
    // from 1 for itself): the client's (newId), so a second renderer's
    // windows never take it.
    std::map<std::uint64_t, std::string> sentTabs_;
    std::map<std::uint64_t, std::uint64_t> hostWindow_;
    std::map<std::uint64_t, std::vector<std::uint64_t>> windowTerms_;
    std::map<std::uint64_t, Json> windowLayout_;
    struct Panes {
        Json browser, panes, reviews;
    };
    std::map<std::uint64_t, Panes> windowPanes_;
    // The browsers' views on the in-process host, by their id there: each
    // message the host sends one of them goes to its client, under the
    // lock, so letting one go stops its deliveries at once.
    struct Viewers;
    std::shared_ptr<Viewers> viewers_;
};

// The link to the host of `root`'s folder: the one already made for it,
// else a new one, its host found or started as the environment and
// `headless` (editor.session-host = headless) say. A link whose host went
// is passed over: the folder gets a new one (stage 6, step 2: the windows
// on the old one move to it, Editor::rehost), found or started again.
std::shared_ptr<Link> forFolder(const std::string& root, bool headless);
// A window over SSH (stage 8): `address` is ssh://[user@]host[:port]/path.
// The user's ssh ($MUTIBLE_SSH names another program, for the checks)
// running `mutible host --stdio <path>` there is started, and `done` is
// called on the main loop, never inside this call, with the live link once
// the host's Welcome has come (at most 30 s: ssh's connection), or with a
// null link and why in the window's words (ssh needed a password or a host
// key confirmed, mutible missing or too old there, ssh's own last line).
// The main loop runs meanwhile (the start is a spawn, the wait polls), so
// nothing waits on a connection. A live link for that machine and path is
// given again. Returns the link connecting, for its progress and
// Link::cancelConnecting (a cancelled try never calls `done`); null when
// ssh could not be run or the address is no ssh:// one, and the live link
// given again (not connecting() then).
std::shared_ptr<Link> connectRemote(const std::string& address,
                                    std::function<void(std::shared_ptr<Link>, std::string)> done);

std::shared_ptr<Link> connectRemoteFor(const std::string& address, const std::shared_ptr<Link>& lost,
                      std::function<void(std::shared_ptr<Link>, std::string)> done);

// A window over SSH's connection made again after it dropped (stage 8): the
// lost link's address and token, so the lasting host there lets go of the
// stale connection and the window's documents and terminals can be taken up
// again from it. `done` as connectRemote's; the lost link stays registered
// until a try succeeds.
std::shared_ptr<Link> reconnectRemote(const std::shared_ptr<Link>& lost,
                                      std::function<void(std::shared_ptr<Link>, std::string)> done);

// The folder a lasting host's socket is in, as `mutible host --stdio` makes
// it (tools/mutible-cli.cpp, lastingSocketDir).
std::string lastingSocketFolder();

// Whether the folder's link now has lost its host (a new window, or
// Reconnect, finds or starts it again).
bool folderLost(const std::string& root);

// A window let its link go, detached: an in-process host holding anything
// (documents, terminals, a window's record) is kept, with its thread, while
// the app runs, for the folder's next window; one holding nothing goes. A
// headless host needs nothing kept (it runs on by itself).
void keepWhileHolding(const std::shared_ptr<Link>& link);
// For the GUI checks between suites: everything the links hold that no
// open window shows (free records, their documents and terminals) is
// closed on its host, and kept links holding nothing go, so one suite's
// detached terminals never come back in the next suite's window.
void endDetached();

// Every link there is now (checks).
std::vector<std::shared_ptr<Link>> all();

// Quitting: every headless host let go of; each keeps running until its
// own lifetime ends. In-process hosts kept for the next window go (their
// shells hung up).
void detachAll();

}  // namespace SessionLink
