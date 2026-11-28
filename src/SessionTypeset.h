// SessionTypeset.h — the LaTeX preview for a window on another machine (the
// preview over SSH's first slice, docs/notes/session-host.md, "LaTeX typeset
// by the host"). A window over SSH (`mutible ssh://host/path`) previews a
// .tex file that is on the other machine, so the host typesets it there with
// that machine's tectonic and the window gets the PDF and the SyncTeX file
// back; the files the document reads (\input, \includegraphics, .bib) are
// that machine's, so they resolve as they do for a local preview.
//
//   FsRead {req, window, json: {"op": "typeset", "doc", "view", "gen",
//           "timeout"}}
//     -> FsData {req, json: {"op": "typeset", "view", "gen", "ok", ...},
//                bytes: the PDF, then the SyncTeX file, then TeX's
//                transcript, their sizes in the json}
//      or FsData {req, json: {"op": "typeset", "error", "reason"}} (not
//                run: "readonly", "nodoc", "unsaved", "args", "busy")
//   FsRead {json: {"op": "typesetStop", "view"}}   answered by nothing
//
// `doc` is the document's id (the host holds its text, so the unsaved text
// is what is typeset), `view` the window's key for one preview (the local
// preview's `<view>`: two previews of one file have two keys), `gen` a number
// the window counts up and the answer echoes (a window drops an answer that
// is no longer its newest). A newer request for the same view stops the run
// in flight (SIGTERM, then SIGKILL, of its process group) and replaces it:
// the older one is answered {"superseded": true}. One run at a time per view.
//
// The text goes to the hidden sibling `.<name>.<pid>-<view>.mutible.tex`
// beside the document's file (LatexScratch: its record, kept in the work
// folder, written before it), and tectonic runs on it with the apps' own
// arguments (--untrusted --synctex --keep-logs --chatter minimal --color
// never --outdir <work folder>), in the file's folder, with the host's
// environment. It is removed when the run ends, so a repository never shows
// it for longer than the run: also by a newer run, TypesetStop, the
// document's close (DocClose by that window), the window's connection going,
// the host's end, and, after a crash, by the next host's start, which
// removes the work folders of owners that are gone and, through their
// records, a sibling that is still exactly the bytes written. The work
// folder (under the host's $TMPDIR, `mutible-host-latex-<uid>`) is removed
// with each run, the outputs having been read into the answer.
//
// A host offers it in its Welcome (flag 8192, kHostTypesets). Run on a
// worker of the host's (as the formatters are, SessionFormat.h), never on
// its thread. Pure C++17 and POSIX.
//
// What the answer's json says: "ok" (a PDF came), "missing" (no tectonic
// there: "error" names the host and what was looked for), "superseded" and
// "stopped" (the run was stopped: "error" says by what), "error" (the
// words to show), "log" (what tectonic printed, valid UTF-8), "status"
// (its exit status, -1 none), "scratch" (the sibling's path on the host,
// which SyncTeX's input lines name), "stem" (the outputs' name, without an
// extension), "pdf", "synctex", "transcript" (the bytes' sizes).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Json.h"
#include "SessionProtocol.h"

namespace Shared {

constexpr double kTypesetSeconds = 300;                         // a run's longest: 5 minutes, as the apps'
constexpr std::size_t kMaxTypesetPdf = std::size_t(64) << 20;   // as the remote viewers' limit
constexpr std::size_t kMaxTypesetSynctex = std::size_t(64) << 20;
constexpr std::size_t kMaxTypesetTranscript = std::size_t(4) << 20;
constexpr std::size_t kMaxTypesetLog = std::size_t(1) << 20;

// One preview's place on the host (shared by the thread that takes the
// requests and the worker that runs them).
struct TypesetSlot {
    std::mutex run;                        // one run at a time
    std::atomic<std::uint64_t> latest{0};  // the newest run's number; a run that is not it stops
    unsigned long long view = 0;           // LatexScratch::Owner's view
    DocId doc = 0;
    std::atomic<bool> closed{false};       // the preview was closed, not replaced
};

// The previews of a host, by renderer and key. The host thread's.
class TypesetPreviews {
public:
    // A new run for (c, view): its slot and number. Whatever ran for the key
    // is no longer the newest and stops.
    std::shared_ptr<TypesetSlot> begin(ClientId c, const std::string& view, DocId doc, std::uint64_t* seq);
    // The preview is closed: its run stops and the key is forgotten.
    void stop(ClientId c, const std::string& view);
    void stopDoc(ClientId c, DocId doc);
    void stopAll(ClientId c);
    std::size_t size() const { return slots_.size(); }

private:
    std::map<std::pair<ClientId, std::string>, std::shared_ptr<TypesetSlot>> slots_;
    std::uint64_t seq_ = 0;
    unsigned long long views_ = 0;
};

// What a run needs.
struct TypesetJob {
    std::string path;      // the document's file, absolute, on the host
    std::string text;      // its text now
    std::string view;
    std::uint64_t gen = 0;
    double timeout = kTypesetSeconds;
};

// The answer's json and bytes. Blocks (on the slot's run lock, then the
// run) for the worker's thread; `cancel` is the worker's (the renderer
// went, the host ends), `seq` the run's number in `slot`.
struct TypesetAnswer {
    Json json;
    std::string bytes;
};
TypesetAnswer runTypeset(const TypesetJob& job, TypesetSlot& slot, std::uint64_t seq,
                         const std::vector<std::string>& env, const std::atomic<bool>& cancel);
TypesetAnswer typesetRefused(const std::string& why, const std::string& reason);

// Where tectonic is on this machine: `env`'s MUTIBLE_TECTONIC (else this
// process's), mutible's managed folder, then PATH and the usual folders;
// "" when none, `tried` then naming what was looked for.
std::string findTectonic(const std::vector<std::string>& env, std::string* tried);

// The folder the work folders are made under: $TMPDIR/mutible-host-latex-<uid>
// (mode 0700, owned by this user), "" when there is none that is private.
std::string typesetBaseFolder();
// A host's start: the work folders of owners that are gone are removed, and
// with them a sibling their record names that is still exactly its bytes.
void sweepTypesetFolders();

// The SHA-256 of `bytes`, lowercase hex (LatexScratch's records hold it).
std::string typesetSha256Hex(const std::string& bytes);

// ---- the window's side

struct TypesetReply {
    bool ok = false, missing = false, superseded = false, stopped = false;
    std::string error, log, scratch, stem;
    int status = -1;
    std::uint64_t gen = 0;
    std::string view;
    std::string pdf, synctex, transcript;   // the bytes
};
TypesetReply typesetReplyFrom(const Json& answer, std::string&& bytes);

// Writes the reply's PDF and SyncTeX file into `folder` (made when it is
// not there) as tectonic would have left them beside a local typeset:
// <stem>.pdf and <stem>.synctex.gz (no file when the host sent none), and
// TeX's transcript as <stem>.log when `withTranscript`. False, with `error`,
// when one could not be written.
struct TypesetFiles {
    std::string pdf, synctex, transcript;
};
bool writeTypesetFiles(const TypesetReply& r, const std::string& folder, bool withTranscript,
                       TypesetFiles* out, std::string* error);

// Words for a window whose host cannot typeset or is gone.
std::string typesetTooOldText(const std::string& host);
std::string typesetLostText(const std::string& host);

}  // namespace Shared
