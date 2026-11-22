// RemoteCache.h — the rules for showing a file of a window over SSH in a
// viewer that reads a file of its own (a picture, a PDF, a player, a
// SQLite database, Quick Look, a table too large to edit): the window reads
// the file's bytes through its host (Client::readFile) into a private
// folder of copies on this machine, one folder per window, and points the
// viewer at the copy. The tab's path stays the remote one; only the viewer
// sees the copy. docs/notes/session-host.md, "Viewers over SSH".
//
// What lives here: the copies' folder names (so a launch can remove what a
// process that ended left), where a remote file's copy goes in its window's
// folder, the size limit and its sentence, how a file read through the
// host is shown, whether a notice from the host's disk watching may have
// changed a copy, the fingerprint that tells a fresh read from the copy,
// and which remote file a picture in a Markdown file names. Pure: no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RemoteCache {

// The most bytes of a file read for a viewer: the host's own limit for a
// file it reads whole (SessionFiles.h's kMaxFollowed, Link::readText's).
constexpr std::size_t kMaxBytes = std::size_t(64) << 20;

// A window's folder of copies, in the app's folder of them: "w<pid>-<n>",
// the process's id and a number of its own, so a launch can tell a
// folder left by a process that ended (a crash, a quit with the window
// open) from one a running copy of the app is using.
std::string windowFolderName(long pid, std::uint64_t serial);
// The process a window folder's name holds; false for any other name
// (nothing that is not one is ever removed).
bool ownerOfFolder(const std::string& name, long* pid);

// Where the copy of `path`, a file inside the remote folder `root`, goes in
// its window's folder: its path from `root`, so the copy keeps its name and
// extension (the viewers choose by them) and two files of the same name in
// two folders never share a copy. "" when `path` is not inside `root`, is
// `root`, or has a name that is none ("", ".", "..", a NUL).
std::string placeOf(const std::string& root, const std::string& path);

// How a file read through the host is shown.
enum class Show {
    Text,       // the editor, or a viewer over or beside its text (a CSV table, a JSON outline, a notebook)
    Image,      // a copy, in the picture viewer
    Pdf,        // a copy, in the PDF viewer
    Media,      // a copy, in the player
    Viewer,     // a copy, in a viewer without text (SQLite, Quick Look, a table too large to edit)
    TooLarge,   // over kMaxBytes: a placeholder saying so, nothing read
};
// By its name first, as a file on this machine is (pictures, PDFs, video
// and audio by extension), then by its head (FileSniff::classify: a
// database, a document only Quick Look shows, a CSV or TSV over
// `tableEditableBytes`, which opens read-only from the file), then by
// whether its bytes decode as text (`decodesAsText`): a file that does not
// goes to Quick Look, as on this machine. `tooLarge`: the host said it is
// over the limit.
Show showFor(const std::string& path, bool tooLarge, const std::string& bytes, bool decodesAsText,
             std::uint64_t tableEditableBytes);
const char* showName(Show s);   // "text", "image", ... for tests and logs

// The placeholder's text for a file over kMaxBytes: its name, the limit,
// and where the file is (`place`, the window's "host:path").
std::string tooLargeMessage(const std::string& name, const std::string& place);

// Whether a notice of the host's disk watching (FsChanged: the folders
// whose listings changed, absolute, spelled as the window spells its tabs'
// paths) may have changed the file at `path`: its folder is among them.
// The host's listing carries no sizes or dates, so the window reads the
// file again and compares (fingerprint).
bool mayHaveChanged(const std::vector<std::string>& changedFolders, const std::string& path);

// A copy's fingerprint (64-bit FNV-1a of its bytes): a fresh read with the
// same one is the same file, and the viewer is left as it is.
std::uint64_t fingerprint(const std::string& bytes);

// The remote file a picture in a Markdown file names, when it is inside
// `root`: `src` as written (a path from `docFolder`, the Markdown file's
// folder, or an absolute one), its query and fragment dropped and %XX
// decoded, "." and ".." taken out by name. "" for a web address, a data:
// URI, a file:// URL, a ~ path, an empty one, or anything outside `root`:
// in a window over SSH nothing names a file of this machine.
std::string markdownPicture(const std::string& src, const std::string& docFolder, const std::string& root);

}  // namespace RemoteCache
