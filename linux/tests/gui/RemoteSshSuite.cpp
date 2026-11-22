// RemoteSshSuite.cpp — `./build/mutible-guitest remote_ssh`: a window over
// SSH (stage 8, docs/notes/session-host.md, "The GTK port over SSH"; the
// Mac's RemoteSshSuite.mm), with a stand-in for ssh. MUTIBLE_SSH names a
// script that records its arguments, skips ssh's options to the "--", takes
// the host, and runs the remote command as ssh's remote shell would, with
// this build's `mutible` as `mutible` and a variable of its own exported (so
// what the shell sees is the "remote" environment, not this one's). The
// folder it serves is a scratch folder of this machine's, but the window
// reads, writes and stamps nothing of it itself: every read and write goes
// through the host. No real ssh is ever run.
//   - `ssh://standin/<folder>/notes.txt` through the command line's path
//     (the launcher's): one window, on a remote link, its title
//     "notes.txt — standin:<folder> — mutible", the file's tab with its
//     text, clean, with no stamp; ssh run with BatchMode, no pty, `--`
//     before the host, and `mutible host --stdio '<file>'` as the remote
//     command;
//   - typing and Ctrl+S: the host writes the file (the tab clean, the text
//     on disk);
//   - a terminal: its shell is the host's child (`ps` of its parent says
//     mutible) in the host's environment (the stand-in's variable);
//   - the connection drops (the stand-in's process group killed): the
//     window says it is disconnected (its title, Reconnect), the text stays
//     and takes more typing, and a save says why it cannot be made;
//     Reconnect runs ssh again: the file is open on the new host with
//     everything typed, saved there with Ctrl+S, and the terminal has a new
//     shell there that runs a command;
//   - the tree, Quick Open and Find in Folder through the host (second
//     slice, the Mac's #322): the tree's top level is the host's listing
//     (folders first, the ignored log marked, every listing counted by the
//     window's workspace); the folder src opened; two files made on disk
//     appear through the host's notices; Enter on a row opens it through the
//     host; the tree's file operations go through the host (flag 64): New
//     Folder, Rename of an open file and of a folder holding one (the tabs'
//     paths follow, a save then writes the new name), a refused rename (the
//     host's sentence in the status bar, nothing changed), a drag onto a
//     folder, Delete after its question, New File; Ctrl+P lists the host's
//     files without the ignored log and opens src/util.c; Find in Folder
//     finds "2 matches in 2 files" with the log left out, a row opens
//     src/main.c, and Replace All goes through the host's replace (a host
//     without it refuses with a note; the closed file is rewritten, the clean
//     tab shows the new text unsaved, a tab with changes is left alone and
//     named, the question and the count in the local words); after
//     Reconnect a file made on disk appears from the new host;
//   - the connecting window (the Mac's #342): while ssh connects a small
//     window says "Connecting to standin…" (the stand-in sleeps while a flag
//     file exists), then "Starting mutible on standin…" once `host --stdio`
//     said it started on ssh's standard error, then "Opening <folder> on
//     standin…", and closes when the window opens; Cancel kills ssh's
//     process group (its pid is the stand-in's), leaving no window, no
//     alert and no process; Reconnect by the user shows none (the Mac's
//     way): the status bar says "Reconnecting to standin:..." with a Stop
//     button, Stop or Esc ends ssh and leaves the window disconnected with
//     Reconnect shown, and a third press connects; the tries by themselves
//     show no window either;
//   - the window closes, and the host it ran ends with it;
//   - a stand-in that fails as ssh does for a password, and one whose
//     remote has no mutible: an alert each, in the core's words, and no
//     window.
#include "Harness.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <vte/vte.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ConnectWindow.h"
#include "ControlPaths.h"
#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "FileTree.h"
#include "PalettePanel.h"
#include "Markdown.h"
#include "MediaView.h"
#include "RemoteCopies.h"
#include "RemoteWorkspace.h"
#include "Search.h"
#include "SessionLink.h"
#include "Terminal.h"

namespace {

std::string gFolder, gFile, gSsh, gArgs, gPidFile, gBin, gBlock;
std::string gBig, gCopies;   // gCopies: the window's folder of copies
size_t gBigSize = 0;
double gBlockingMs = 0, gMaxGapMs = 0;
gint64 gLastTick = 0;
int gTicks = 0, gTabsBefore = 0;
guint gTickId = 0;
int gShell = 0, gLasting = 0;   // the terminal's shell, and the lasting host that holds it
uint64_t gDocId = 0;
std::string gSocketOfLasting, gLastingFolderPath;
int gHostPidB = 0;
bool gSawNote = false;
bool gHadHost = false;
std::string gHostMode;
std::string gErrors;   // what the command line said, as its caller reads it
size_t gWindowsBefore = 0;
int gHostA = 0, gHostB = 0;
int gTried = 0;
double gSent = 0;
size_t gNoticesBefore = 0;
std::string gSlow;                       // while this file exists the stand-in waits before it starts the host
std::shared_ptr<ConnectWindow> gCw;      // the connecting window a step looked at
unsigned gShownBefore = 0;
int gCancelledPid = 0;

std::string rp(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int standInPid() { return std::atoi(readFile(gPidFile).c_str()); }

bool gone(int pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }

MCGuiAppWindow remoteWindow() {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.editor && w.editor->sessionLink() &&
            w.editor->sessionLink()->kind() == SessionLink::Link::Kind::Remote)
            found = w;
    return found;
}

// ---- the tree's rows, read from its model
GListModel* treeRows() {
    GtkWidget* w = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(remoteWindow().tree->widget()));
    return GTK_IS_LIST_VIEW(w) ? G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(w))) : nullptr;
}

struct TreeRow {
    std::string name;
    guint depth = 0;
    bool dir = false, ignored = false;
};

std::vector<TreeRow> allTreeRows() {
    std::vector<TreeRow> out;
    GListModel* m = treeRows();
    for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
        GtkTreeListRow* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(r));
        TreeRow t;
        t.name = g_file_info_get_name(info);
        t.depth = gtk_tree_list_row_get_depth(r);
        t.dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        t.ignored = g_object_get_data(G_OBJECT(info), "mutible-ignored") != nullptr;
        out.push_back(t);
        g_object_unref(info);
        g_object_unref(r);
    }
    return out;
}

std::string topNames() {
    std::string out;
    for (const TreeRow& r : allTreeRows())
        if (r.depth == 0) out += (out.empty() ? "" : ",") + r.name + (r.dir ? "/" : "");
    return out;
}

bool hasRow(const std::string& name, guint depth) {
    for (const TreeRow& r : allTreeRows())
        if (r.name == name && r.depth == depth) return true;
    return false;
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(remoteWindow().statusLabel)); }

Document* tabOf(const std::string& path) {
    const MCGuiAppWindow w = remoteWindow();
    return w.editor ? w.editor->documentFor(path) : nullptr;
}

std::string textOf(Document* d) {
    if (!d) return "";
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buffer(), &s, &e);
    char* t = gtk_text_buffer_get_text(d->buffer(), &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

std::string titleOf(const MCGuiAppWindow& w) {
    const char* t = w.window ? gtk_window_get_title(w.window) : nullptr;
    return t ? t : "";
}

// Every label's text under `w`, joined.
std::string allText(GtkWidget* w) {
    std::string out;
    if (GTK_IS_LABEL(w)) out += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        out += allText(c);
    return out;
}

// The text of an alert that is up (a toplevel that is no editor window), or "".
std::string alertText() {
    GtkWindow* top = MCGuiTopWindow();
    if (!top) return "";
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (top == a.window) return "";
    return allText(GTK_WIDGET(top));
}

void dismissAlert() {
    GtkWindow* top = MCGuiTopWindow();
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (top == a.window) return;
    if (top) gtk_window_close(top);
}

// The first widget of `type` under `w`.
GtkWidget* firstOfType(GtkWidget* w, GType type) {
    if (!w) return nullptr;
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = firstOfType(c, type)) return hit;
    return nullptr;
}

GtkWidget* nameEntry() { return firstOfType(remoteWindow().tree->namePopover(), GTK_TYPE_ENTRY); }

// The tree's name box (New File, New Folder, Rename) filled in and answered.
void typeName(const std::string& name) {
    MCGuiWait("the name box, with the keyboard", 10, [] {
        GtkWidget* e = nameEntry();
        return e && gtk_widget_is_visible(e) && gtk_widget_has_focus(firstOfType(e, GTK_TYPE_TEXT)) &&
               gtk_window_is_active(remoteWindow().window);
    });
    MCGuiStep("the name typed over what was there", [name] {
        MCGuiKey("ctrl+a");
        MCGuiType(name);
    });
    MCGuiWait("in the box", 10, [name] {
        GtkWidget* e = nameEntry();
        return e && std::string(gtk_editable_get_text(GTK_EDITABLE(e))) == name;
    });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
}

// The row at `rel` (from the folder) revealed and selected, the tree with the keyboard.
void selectRow(const std::string& rel) {
    MCGuiStep("the row " + rel + " selected", [rel] {
        remoteWindow().tree->revealPath(gFolder + rel);
        remoteWindow().tree->focus();
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("selected", 10, [rel] { return remoteWindow().tree->selectedPath() == gFolder + rel; });
}


// A PNG of w by h pixels at `path`.
void writePng(const std::string& path, int w, int h) {
    GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_fill(pb, 0x336699ff);
    gdk_pixbuf_save(pb, path.c_str(), "png", nullptr, nullptr);
    g_object_unref(pb);
}

// A PDF of `pages` empty pages, its cross-reference table made as it goes.
std::string pdfOf(int pages) {
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> at;
    auto object = [&](const std::string& body) {
        at.push_back(out.size());
        out += std::to_string(at.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    object("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (int i = 0; i < pages; i++) kids += std::to_string(3 + i) + " 0 R ";
    object("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
    for (int i = 0; i < pages; i++) object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] >>");
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(at.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : at) {
        char line[32];
        snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(at.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

bool modeIs(const std::string& path, mode_t mode) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == mode;
}

void writeScript(const std::string& path, const std::string& body) {
    MCGuiWriteFile(path, body);
    chmod(path.c_str(), 0755);
}

// `command` typed into the terminal until `expect` shows on its screen,
// again every 3 s: a remote shell has no integration's marks to say its
// line editor is ready, and one starting may drop what came before it.
bool runUntil(const std::string& command, const std::string& expect) {
    Terminal* t = remoteWindow().terminal;
    if (!t || !t->vteWidget()) return false;
    if (t->recentText(40).find(expect) != std::string::npos) return true;
    const double now = (double)g_get_monotonic_time() / 1000.0;
    if (now - gSent > 3000) {
        gSent = now;
        const std::string line = command + "\n";
        vte_terminal_feed_child(VTE_TERMINAL(t->vteWidget()), line.c_str(), (gssize)line.size());
    }
    return false;
}

// A fresh word for an alert's steps.
void fails(const std::string& what, const std::string& body, const std::string& address,
           const std::string& expect) {
    MCGuiStep(what, [body, address] {
        gWindowsBefore = MCGuiAppWindows().size();
        const std::string script = rp(MCGuiScratch()) + "/ssh-fails";
        writeScript(script, body);
        EnvLock::envSet("MUTIBLE_SSH", script.c_str());
        gErrors.clear();
        MCGuiCommandLine({address}, gFolder, &gErrors);
    });
    MCGuiWait(what + ": an alert says why", 20, [expect] {
        return alertText().find(expect) != std::string::npos;
    });
    MCGuiStep(what + ": its words, and no window", [expect, address] {
        const std::string text = alertText();
        MCGuiNote("alert: %s", text.c_str());
        GCHECK(text.find("Could not open " + address) != std::string::npos);
        GCHECK(text.find(expect) != std::string::npos);
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(gErrors.find(address) != std::string::npos);
        dismissAlert();
    });
    MCGuiWait(what + ": the alert gone", 10, [] { return alertText().empty(); });
}

}  // namespace

MC_GUI_SUITE(remote_ssh) {
    MCGuiStep("a folder for the remote, and a stand-in for ssh", [] {
        const std::string scratch = rp(MCGuiScratch());
        gFolder = scratch + "/remote-proj";
        g_mkdir_with_parents(gFolder.c_str(), 0700);
        gFolder = rp(gFolder);   // as the host names it
        gFile = gFolder + "/notes.txt";
        MCGuiWriteFile(gFile, "first line\n");
        // What the tree, Quick Open and search show: an ignored log, two
        // sources that hold "needle" and the log too.
        g_mkdir_with_parents((gFolder + "/src").c_str(), 0700);
        MCGuiWriteFile(gFolder + "/.gitignore", "*.log\n");
        MCGuiWriteFile(gFolder + "/app.log", "needle in the log\n");
        MCGuiWriteFile(gFolder + "/src/main.c", "int main() { return 0; }  /* needle one */\n");
        MCGuiWriteFile(gFolder + "/src/util.c", "int util() { return 1; }  /* needle two */\n");
        gBin = scratch + "/ssh-bin";
        g_mkdir_with_parents(gBin.c_str(), 0700);
        const std::string app = MCGuiBuildDir() + "/mutible";
        GCHECK(g_file_test(app.c_str(), G_FILE_TEST_IS_EXECUTABLE));
        unlink((gBin + "/mutible").c_str());
        GCHECK(symlink(app.c_str(), (gBin + "/mutible").c_str()) == 0);
        gBlock = scratch + "/ssh-block";
        unlink(gBlock.c_str());
        // The lasting host ends soon after the window's work is done.
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "0.15");
        gArgs = scratch + "/ssh-args.txt";
        gPidFile = scratch + "/ssh-pid.txt";
        gSlow = scratch + "/ssh-slow";
        unlink(gSlow.c_str());
        gSsh = scratch + "/ssh-stand-in";
        writeScript(gSsh,
                    "#!/bin/sh\n"
                    "# A stand-in for ssh (RemoteSshSuite.cpp): its arguments recorded,\n"
                    "# its options skipped to the --, then the host, then the remote\n"
                    "# command run as ssh's remote shell runs it.\n"
                    "[ -e '" + gBlock + "' ] && { echo 'standin: Connection refused' >&2; exit 255; }\n"
                    "printf '%s\\n' \"$@\" > '" + gArgs + "'\n"
                    "while [ $# -gt 0 ] && [ \"$1\" != -- ]; do shift; done\n"
                    "shift\n"
                    "shift\n"
                    "echo $$ > '" + gPidFile + "'\n"
                    "while [ -e '" + gSlow + "' ]; do sleep 0.1; done\n"
                    "SSH_STANDIN=yes\n"
                    "export SSH_STANDIN\n"
                    "PATH='" + gBin + "':$PATH\n"
                    "export PATH\n"
                    "exec /bin/sh -c \"$*\"\n");
        EnvLock::envSet("MUTIBLE_SSH", gSsh.c_str());
        gWindowsBefore = MCGuiAppWindows().size();
    });

    // ---- ssh failing as it does: an alert, no window
    fails("a password asked for", "#!/bin/sh\necho 'standin: Permission denied (publickey,password).' >&2\nexit 255\n",
          "ssh://standin/tmp/x", "needs a password or a key's passphrase");
    fails("mutible not on the remote", "#!/bin/sh\necho 'sh: 1: mutible: not found' >&2\nexit 127\n",
          "ssh://standin/tmp/x", "mutible is not installed on standin");
    fails("a mutible without --stdio", "#!/bin/sh\necho 'mutible: unknown option --stdio' >&2\nexit 2\n",
          "ssh://standin/tmp/x", "is too old for a window over SSH");
    // ---- the connecting window: a slow connect cancelled, then one let through
    MCGuiStep("the stand-in back, slow: a first connect, its window", [] {
        EnvLock::envSet("MUTIBLE_SSH", gSsh.c_str());
        gErrors.clear();
        gWindowsBefore = MCGuiAppWindows().size();
        gShownBefore = ConnectWindow::shownCount();
        MCGuiWriteFile(gSlow, "x");
        unlink(gPidFile.c_str());
        MCGuiCommandLine({"ssh://standin" + gFile}, gFolder, &gErrors);
    });
    MCGuiWait("the connecting window is up, and ssh started", 20, [] {
        return ConnectWindow::pending().size() == 1 && standInPid() > 0;
    });
    MCGuiStep("it says it is connecting, no editor window yet; Cancel", [] {
        GCHECK(ConnectWindow::pending().size() == 1);
        if (ConnectWindow::pending().empty()) return;
        gCw = ConnectWindow::pending()[0];
        GCHECK_EQ_STR(gCw->line(), "Connecting to standin\u2026");
        GCHECK_EQ_STR(gCw->title(), "standin:" + gFile + " \u2014 mutible");
        GCHECK(gCw->window() && gtk_window_get_title(GTK_WINDOW(gCw->window())) == gCw->title());
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        gCancelledPid = standInPid();
        GCHECK(gCw->commandPid() == gCancelledPid && gCancelledPid > 0);
        GCHECK(!gone(gCancelledPid));
        gCw->cancel();
    });
    MCGuiWait("it closes, and ssh with its sleep is gone", 20, [] {
        return ConnectWindow::pending().empty() && gone(gCancelledPid) && kill(-gCancelledPid, 0) != 0;
    });
    MCGuiStep("no window, no alert, no link, nothing said; the lines it showed", [] {
        GCHECK(gCw->over());
        GCHECK(gCw->progress().passed(Shared::ConnectProgress::Stage::Cancelled));
        GCHECK(!gCw->progress().passed(Shared::ConnectProgress::Stage::Opening));
        GCHECK_EQ_STR(gCw->linesShown().size() == 2 ? gCw->linesShown()[1] : "", "Stopped connecting to standin.");
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(alertText().empty());
        GCHECK(gErrors.empty());
        for (const std::shared_ptr<SessionLink::Link>& l : SessionLink::all()) GCHECK(l->kind() != SessionLink::Link::Kind::Remote);
        gCw.reset();
    });
    // A negative check: a cancelled try must never answer later; time is the condition.
    MCGuiPause(0.3);
    MCGuiStep("nothing opened or said after the cancel; the same address again, slow", [] {
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(alertText().empty());
        GCHECK(gErrors.empty());
        GCHECK(ConnectWindow::pending().empty());
        unlink(gPidFile.c_str());
        MCGuiCommandLine({"ssh://standin" + gFile}, gFolder, &gErrors);
    });
    MCGuiWait("the connecting window again", 20, [] { return ConnectWindow::pending().size() == 1 && standInPid() > 0; });
    MCGuiStep("still connecting; ssh let through", [] {
        GCHECK(ConnectWindow::pending().size() == 1);
        if (ConnectWindow::pending().empty()) return;
        gCw = ConnectWindow::pending()[0];
        GCHECK_EQ_STR(gCw->line(), "Connecting to standin\u2026");
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        unlink(gSlow.c_str());
    });

    // ---- the window
    MCGuiWait("a window on the remote host", 30, [] { return remoteWindow().editor != nullptr; });
    MCGuiWait("notes.txt read through the host", 20, [] {
        return textOf(tabOf(gFile)) == "first line\n" && remoteWindow().editor->currentPath() == gFile;
    });
    MCGuiStep("its title, its tab, how ssh was run; the connecting window said its stages and closed", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(ConnectWindow::pending().empty());
        if (gCw) {
            GCHECK(gCw->over());
            const std::vector<std::string>& lines = gCw->linesShown();
            GCHECK(lines.size() == 3);
            if (lines.size() == 3) {
                GCHECK_EQ_STR(lines[0], "Connecting to standin\u2026");
                GCHECK_EQ_STR(lines[1], "Starting mutible on standin\u2026");
                GCHECK_EQ_STR(lines[2], "Opening " + gFolder + " on standin\u2026");
            }
            GCHECK(gCw->progress().passed(Shared::ConnectProgress::Stage::Starting));
            gCw.reset();
        }
        const MCGuiAppWindow w = remoteWindow();
        GCHECK(gErrors.empty());
        if (!w.editor) return;
        gHostA = standInPid();
        GCHECK(gHostA > 0);
        GCHECK_EQ_STR(w.rootDir, gFolder);
        SessionLink::Link* l = w.editor->sessionLink();
        GCHECK_EQ_STR(l->label(), "standin:" + gFolder);
        GCHECK_EQ_STR(l->folder(), gFolder);
        GCHECK_EQ_STR(titleOf(w), "notes.txt — standin:" + gFolder + " — mutible");
        Document* d = tabOf(gFile);
        GCHECK(d != nullptr && d->remote() && !d->dirty());
        if (d) GCHECK(!d->diskStamp().exists);   // no stamp of this machine's file at that path
        const std::string args = readFile(gArgs);
        MCGuiNote("ssh's arguments:\n%s", args.c_str());
        GCHECK(args.find("\n-T\n") != std::string::npos || args.rfind("-T\n", 0) == 0);
        GCHECK(args.find("BatchMode=yes") != std::string::npos);
        GCHECK(args.find("\n--\nstandin\nmutible host --stdio '" + gFile + "'\n") != std::string::npos);
        gWindowsBefore = MCGuiAppWindows().size();
    });

    // ---- typing and Ctrl+S: the host writes the file
    MCGuiStep("typed at the end", [] {
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiStep("typing", [] { MCGuiType("typed over ssh\n"); });
    MCGuiWait("unsaved", 10, [] {
        Document* d = tabOf(gFile);
        return d && d->dirty() && textOf(d) == "first line\ntyped over ssh\n";
    });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("on disk, written by the host, the tab clean", 10, [] {
        Document* d = tabOf(gFile);
        return readFile(gFile) == "first line\ntyped over ssh\n" && d && !d->dirty();
    });
    MCGuiStep("still no stamp, and the title has no dot", [] {
        Document* d = tabOf(gFile);
        GCHECK(d && !d->diskStamp().exists);
        GCHECK_EQ_STR(titleOf(remoteWindow()), "notes.txt — standin:" + gFolder + " — mutible");
    });


    // ---- the tree, Quick Open and Find in Folder through the host
    MCGuiWait("the tree's top level comes from the host", 20, [] {
        return hasRow("notes.txt", 0) && hasRow("app.log", 0);
    });
    MCGuiStep("folders first, the ignored log marked, listed by the host and not by this machine", [] {
        const MCGuiAppWindow w = remoteWindow();
        MCGuiNote("top level: %s", topNames().c_str());
        GCHECK(w.tree && w.tree->remote());
        GCHECK(w.remoteWs != nullptr);
        GCHECK_EQ_STR(topNames(), "src/,app.log,notes.txt");   // .gitignore is a dotfile: filtered
        for (const TreeRow& r : allTreeRows())
            if (r.depth == 0) GCHECK(r.ignored == (r.name == "app.log"));
        GCHECK(w.remoteWs && w.remoteWs->listings >= 1);   // a listing is a read of the host's
    });
    MCGuiStep("src opened", [] { remoteWindow().tree->revealPath(gFolder + "/src/main.c"); });
    MCGuiWait("src's files, listed by the host", 20, [] { return hasRow("main.c", 1) && hasRow("util.c", 1); });
    MCGuiStep("two files made on disk", [] {
        gNoticesBefore = remoteWindow().remoteWs->notices;
        MCGuiWriteFile(gFolder + "/src/fresh.c", "int fresh;\n");
        MCGuiWriteFile(gFolder + "/zeta.txt", "z\n");
    });
    MCGuiWait("both appear through the host's notices", 30, [] {
        return hasRow("fresh.c", 1) && hasRow("zeta.txt", 0) && remoteWindow().remoteWs->notices > gNoticesBefore;
    });
    MCGuiStep("util.c selected, Enter", [] {
        remoteWindow().tree->selectIfListed(gFolder + "/src/util.c");
        remoteWindow().tree->focus();
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("util.c selected", 10, [] { return remoteWindow().tree->selectedPath() == gFolder + "/src/util.c"; });
    MCGuiStep("Enter on the row", [] { MCGuiKey("enter"); });
    MCGuiWait("util.c read through the host", 20, [] {
        return textOf(tabOf(gFolder + "/src/util.c")) == "int util() { return 1; }  /* needle two */\n";
    });
    // ---- the tree's file operations through the host (Welcome flag 64):
    // New Folder, Rename, Move by a drag, Delete after its question, a
    // refusal, New File; each changes the disk (the stand-in's folder), the
    // tree's rows come from the host's notices, and the tabs follow.
    MCGuiStep("New Folder, from the tree's src folder", [] {
        remoteWindow().tree->selectIfListed(gFolder + "/src/util.c");
        remoteWindow().tree->focus();
        MCGuiSetKeyWindow(nullptr);
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "newfolder", nullptr);
    });
    typeName("madeby");
    MCGuiWait("made on the host's disk, its row there", 20, [] {
        return g_file_test((gFolder + "/src/madeby").c_str(), G_FILE_TEST_IS_DIR) && hasRow("madeby", 1);
    });
    MCGuiStep("no refusal note, the new folder selected", [] {
        GCHECK(statusText().find("does not work") == std::string::npos);
        GCHECK(remoteWindow().remoteWs->hostDoesFileOps());
    });
    MCGuiWait("the new folder selected", 10, [] { return remoteWindow().tree->selectedPath() == gFolder + "/src/madeby"; });

    MCGuiStep("ren.txt on disk and in a tab, its row selected", [] {
        MCGuiWriteFile(gFolder + "/src/ren.txt", "ren\n");
        MCGuiWriteFile(gFolder + "/src/pack/inner.txt", "inner\n");
        MCGuiWriteFile(gFolder + "/src/dup.txt", "dup\n");
        MCGuiWriteFile(gFolder + "/src/dup2.txt", "dup2\n");
    });
    MCGuiWait("their rows", 20, [] { return hasRow("ren.txt", 1) && hasRow("pack", 1) && hasRow("dup2.txt", 1); });
    MCGuiStep("ren.txt and pack/inner.txt opened", [] {
        remoteWindow().editor->openFile(gFolder + "/src/ren.txt");
        remoteWindow().editor->openFile(gFolder + "/src/pack/inner.txt");
    });
    MCGuiWait("both read through the host", 20, [] {
        return textOf(tabOf(gFolder + "/src/ren.txt")) == "ren\n" && textOf(tabOf(gFolder + "/src/pack/inner.txt")) == "inner\n";
    });
    selectRow("/src/ren.txt");
    MCGuiStep("Rename (F2's action)", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "rename", nullptr);
    });
    typeName("ren2.txt");
    MCGuiWait("renamed on disk, its row renamed, its tab follows", 20, [] {
        Document* d = tabOf(gFolder + "/src/ren2.txt");
        return !g_file_test((gFolder + "/src/ren.txt").c_str(), G_FILE_TEST_EXISTS) &&
               readFile(gFolder + "/src/ren2.txt") == "ren\n" && hasRow("ren2.txt", 1) && !hasRow("ren.txt", 1) &&
               d && d->path() == gFolder + "/src/ren2.txt" && tabOf(gFolder + "/src/ren.txt") == nullptr;
    });
    MCGuiStep("the tab's text kept, clean, still the host's document", [] {
        Document* d = tabOf(gFolder + "/src/ren2.txt");
        GCHECK(d != nullptr);
        if (!d) return;
        GCHECK_EQ_STR(textOf(d), "ren\n");
        GCHECK(!d->dirty());
        GCHECK(d->remote());
    });
    MCGuiStep("typed into the renamed tab, Ctrl+S: the host saves it at the new name", [] {
        remoteWindow().editor->openFile(gFolder + "/src/ren2.txt");
        MCGuiSetKeyWindow(nullptr);
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("more\n");
    });
    MCGuiWait("unsaved", 10, [] { Document* d = tabOf(gFolder + "/src/ren2.txt"); return d && d->dirty(); });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved at the new name, no old file back", 10, [] {
        Document* d = tabOf(gFolder + "/src/ren2.txt");
        return readFile(gFolder + "/src/ren2.txt") == "ren\nmore\n" && d && !d->dirty() &&
               !g_file_test((gFolder + "/src/ren.txt").c_str(), G_FILE_TEST_EXISTS);
    });

    selectRow("/src/pack");
    MCGuiStep("Rename the folder holding an open file", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "rename", nullptr);
    });
    typeName("pack2");
    MCGuiWait("the folder renamed on disk and in the tree, the tab's path follows", 20, [] {
        Document* d = tabOf(gFolder + "/src/pack2/inner.txt");
        return !g_file_test((gFolder + "/src/pack").c_str(), G_FILE_TEST_EXISTS) &&
               readFile(gFolder + "/src/pack2/inner.txt") == "inner\n" && hasRow("pack2", 1) && !hasRow("pack", 1) &&
               d && d->path() == gFolder + "/src/pack2/inner.txt" && tabOf(gFolder + "/src/pack/inner.txt") == nullptr;
    });

    // ---- a refusal: nothing changes, the host's sentence is in the status bar
    selectRow("/src/dup2.txt");
    MCGuiStep("Rename dup2.txt to the name dup.txt has", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "rename", nullptr);
    });
    typeName("dup.txt");
    MCGuiWait("the host's refusal in the status bar", 20, [] { return statusText().find("dup.txt") != std::string::npos &&
                                                                      statusText().find("exist") != std::string::npos; });
    MCGuiStep("nothing changed", [] {
        MCGuiNote("status: %s", statusText().c_str());
        GCHECK_EQ_STR(readFile(gFolder + "/src/dup.txt"), "dup\n");
        GCHECK_EQ_STR(readFile(gFolder + "/src/dup2.txt"), "dup2\n");
        GCHECK(hasRow("dup2.txt", 1) && hasRow("dup.txt", 1));
    });

    // ---- Move: ren2.txt dragged onto the folder madeby
    MCGuiStep("the row revealed", [] { remoteWindow().tree->revealPath(gFolder + "/src/ren2.txt"); });
    MCGuiWait("both rows on screen", 10, [] {
        GtkWidget* row = MCGuiTreeRow(remoteWindow().tree, gFolder + "/src/ren2.txt");
        GtkWidget* to = MCGuiTreeRow(remoteWindow().tree, gFolder + "/src/madeby");
        return row && to && MCGuiOnScreen(row) && MCGuiOnScreen(to);
    });
    MCGuiStep("the row dragged onto madeby", [] {
        GtkWidget* row = MCGuiTreeRow(remoteWindow().tree, gFolder + "/src/ren2.txt");
        GtkWidget* to = MCGuiTreeRow(remoteWindow().tree, gFolder + "/src/madeby");
        if (!row || !to) return;
        graphene_point_t mid = GRAPHENE_POINT_INIT((float)gtk_widget_get_width(to) / 2,
                                                   (float)gtk_widget_get_height(to) / 2);
        graphene_point_t at;
        if (!gtk_widget_compute_point(to, row, &mid, &at)) {
            GCHECK(!"the target's middle in the row's coordinates");
            return;
        }
        const double x0 = 12, y0 = gtk_widget_get_height(row) / 2.0;
        MCGuiDragThrough(row, {{x0, y0}, {x0 + 12, y0 + 2}, {(x0 + at.x) / 2, (y0 + at.y) / 2},
                               {at.x, at.y}, {at.x + 3, at.y + 2}, {at.x, at.y}});
    });
    MCGuiWait("moved on disk, the tab follows", 20, [] {
        Document* d = tabOf(gFolder + "/src/madeby/ren2.txt");
        return !g_file_test((gFolder + "/src/ren2.txt").c_str(), G_FILE_TEST_EXISTS) &&
               readFile(gFolder + "/src/madeby/ren2.txt") == "ren\nmore\n" &&
               d && d->path() == gFolder + "/src/madeby/ren2.txt" && tabOf(gFolder + "/src/ren2.txt") == nullptr;
    });
    MCGuiWait("its row under madeby", 20, [] { return hasRow("ren2.txt", 2); });
    // The drop came from the app's own drag: GDK's user time may be left
    // ahead of the server's (Input.cpp's trap), so put it back.
    MCGuiStep("GDK's user time put back", [] { MCGuiPutBackUserTime(); });
    // ---- Delete: the question, then the host deletes
    selectRow("/src/madeby/ren2.txt");
    MCGuiStep("Delete", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "trash", nullptr);
    });
    MCGuiWait("the question", 10, [] { return alertText().find("permanently") != std::string::npos; });
    MCGuiStep("its words; Cancel changes nothing", [] {
        const std::string t = alertText();
        MCGuiNote("question: %s", t.c_str());
        GCHECK(t.find("Delete “ren2.txt” permanently?") != std::string::npos);
        GCHECK(t.find("no Trash") != std::string::npos);
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Cancel"));
    });
    MCGuiWait("closed", 10, [] { return alertText().empty(); });
    MCGuiStep("still there", [] {
        GCHECK(g_file_test((gFolder + "/src/madeby/ren2.txt").c_str(), G_FILE_TEST_EXISTS));
        GCHECK(hasRow("ren2.txt", 2));
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "trash", nullptr);
    });
    MCGuiWait("the question again", 10, [] { return alertText().find("permanently") != std::string::npos; });
    MCGuiStep("Delete Permanently", [] {
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Delete Permanently"));
    });
    MCGuiWait("gone from the disk and the tree; its clean tab closed", 20, [] {
        return !g_file_test((gFolder + "/src/madeby/ren2.txt").c_str(), G_FILE_TEST_EXISTS) && !hasRow("ren2.txt", 2) &&
               tabOf(gFolder + "/src/madeby/ren2.txt") == nullptr;
    });

    // ---- New File
    selectRow("/src/madeby");
    MCGuiStep("New File in madeby", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "newfile", nullptr);
    });
    typeName("made.txt");
    MCGuiWait("made by the host, its row there, opened", 20, [] {
        Document* d = tabOf(gFolder + "/src/madeby/made.txt");
        return g_file_test((gFolder + "/src/madeby/made.txt").c_str(), G_FILE_TEST_IS_REGULAR) &&
               hasRow("made.txt", 2) && d && remoteWindow().editor->currentPath() == gFolder + "/src/madeby/made.txt";
    });
    MCGuiStep("the new file's tab is the host's: empty, clean; the statuses carry no refusal", [] {
        Document* d = tabOf(gFolder + "/src/madeby/made.txt");
        GCHECK(d != nullptr);
        if (d) GCHECK_EQ_STR(textOf(d), "");
        GCHECK_EQ_STR(readFile(gFolder + "/src/madeby/made.txt"), "");
        // Put the rest of the checks back as they were.
        remoteWindow().tree->selectIfListed(gFolder + "/src/util.c");
    });

    MCGuiStep("Quick Open", [] {
        MCGuiSetKeyWindow(nullptr);
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+p");
    });
    MCGuiWait("it lists the host's files, the ignored log left out", 20, [] {
        PalettePanel* pal = remoteWindow().palette;
        if (!pal || !pal->isOpen()) return false;
        bool util = false;
        for (const std::string& l : pal->labels()) util = util || l.find("util.c") != std::string::npos;
        return util;
    });
    MCGuiStep("no log among them; src/util.c typed, Enter", [] {
        PalettePanel* pal = remoteWindow().palette;
        for (const std::string& l : pal->labels()) {
            MCGuiNote("quick open: %s", l.c_str());
            GCHECK(l.find("app.log") == std::string::npos);
        }
        GCHECK(remoteWindow().remoteWs->fileLists >= 1);
        MCGuiType("src/main");
    });
    MCGuiWait("ranked", 10, [] {
        const std::vector<std::string> l = remoteWindow().palette->labels();
        return !l.empty() && l[0].find("main.c") != std::string::npos;
    });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("src/main.c opened through the host", 20, [] {
        return textOf(tabOf(gFolder + "/src/main.c")) == "int main() { return 0; }  /* needle one */\n" &&
               remoteWindow().editor->currentPath() == gFolder + "/src/main.c";
    });

    MCGuiStep("Find in Folder, \"needle\"", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("ctrl+shift+f");
    });
    MCGuiWait("the panel", 10, [] { return remoteWindow().search != nullptr; });
    // The panel is a window of its own: its entry is on screen (mapped and
    // laid out) a frame or so after the panel exists, and a click before
    // that has nothing to point at.
    MCGuiWait("the panel's entry on screen", 10, [] {
        SearchPanel* p = remoteWindow().search;
        return p && MCGuiOnScreen(p->queryEntry());
    });
    MCGuiStep("typed", [] {
        SearchPanel* p = remoteWindow().search;
        MCGuiSetKeyWindow(GTK_WINDOW(p->window()));
        MCGuiClick(p->queryEntry());
        MCGuiKey("ctrl+a");
        MCGuiType("needle");
    });
    MCGuiWait("two matches in two files, from the host", 30, [] {
        SearchPanel* p = remoteWindow().search;
        return p && !p->searching() && p->matches().size() == 2 && p->statusText() == "2 matches in 2 files";
    });
    MCGuiStep("the ignored log left out; a row opens its file", [] {
        SearchPanel* p = remoteWindow().search;
        MCGuiNote("status: %s", p->statusText().c_str());
        for (const FolderSearchMatch& m : p->matches()) GCHECK(m.relativePath.find("app.log") == std::string::npos);
        GCHECK(remoteWindow().remoteWs->searches >= 1);
        GCHECK_EQ_STR(p->matches()[0].relativePath, "src/main.c");
        GCHECK_EQ_STR(p->matches()[0].path, gFolder + "/src/main.c");
        // Away from main.c first, so the open is seen.
        remoteWindow().editor->openFile(gFile);
        MCGuiKey("down");
        MCGuiKey("enter");
    });
    MCGuiWait("src/main.c shown", 20, [] { return remoteWindow().editor->currentPath() == gFolder + "/src/main.c"; });
    // ---- Replace All through the host (its "replace", as the browser page uses
    // it): a host that does not take it refuses with a note; otherwise the
    // closed file is rewritten on the host's disk, the clean tab shows the
    // new text (the host's edit, unsaved), and the tab with unsaved changes is
    // left alone and named.
    MCGuiStep("a host without the replace refuses, with its note", [] {
        SearchPanel* p = remoteWindow().search;
        p->setHostReplace(nullptr);
        p->showReplace();
        MCGuiSetKeyWindow(GTK_WINDOW(p->window()));
        MCGuiKey("ctrl+alt+enter");   // Replace All
    });
    MCGuiWait("its note", 10, [] {
        return remoteWindow().search->statusText().find("Replace in Folder does not work in a window over SSH here") == 0;
    });
    MCGuiStep("the files untouched; the replace back; util.c closed, fresh.c open with a change of its own", [] {
        GCHECK_EQ_STR(readFile(gFolder + "/src/main.c"), "int main() { return 0; }  /* needle one */\n");
        GCHECK_EQ_STR(readFile(gFolder + "/src/util.c"), "int util() { return 1; }  /* needle two */\n");
        remoteWindow().search->setHostReplace([ws = remoteWindow().remoteWs](Json request, SearchPanel::HostBatch batch) {
            return ws->replace(std::move(request), std::move(batch));
        });
        if (Document* u = tabOf(gFolder + "/src/util.c")) remoteWindow().editor->closeTab(u);
        MCGuiWriteFile(gFolder + "/src/fresh.c", "needle held\n");
        MCGuiSetKeyWindow(nullptr);
        remoteWindow().editor->openFile(gFolder + "/src/fresh.c");
    });
    MCGuiWait("fresh.c read through the host", 20, [] {
        return textOf(tabOf(gFolder + "/src/fresh.c")) == "needle held\n" &&
               remoteWindow().editor->currentPath() == gFolder + "/src/fresh.c";
    });
    MCGuiStep("typed into fresh.c", [] {
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+home");
        MCGuiType("edited ");
    });
    MCGuiWait("fresh.c unsaved", 10, [] {
        Document* d = tabOf(gFolder + "/src/fresh.c");
        return d && d->dirty() && textOf(d) == "edited needle held\n";
    });
    MCGuiStep("searched again: three files", [] {
        SearchPanel* p = remoteWindow().search;
        MCGuiSetKeyWindow(GTK_WINDOW(p->window()));
        MCGuiClick(p->queryEntry());
        MCGuiKey("ctrl+a");
        MCGuiType("needle");
    });
    MCGuiWait("three matches in three files", 30, [] {
        SearchPanel* p = remoteWindow().search;
        return p && !p->searching() && p->matches().size() == 3 && p->statusText() == "3 matches in 3 files";
    });
    MCGuiStep("\"pin\" for the replacement; Replace All", [] {
        SearchPanel* p = remoteWindow().search;
        MCGuiClick(p->replaceEntry());
        MCGuiKey("ctrl+a");
        MCGuiType("pin");
        MCGuiKey("ctrl+alt+enter");
    });
    MCGuiWait("it asks first: fresh.c, held by its tab, is out of the count", 10, [] {
        return alertText().find("Replace 2 matches in 2 files?") != std::string::npos;
    });
    MCGuiWait("its Replace button on screen", 10, [] {
        GtkWindow* top = MCGuiTopWindow();
        GtkWidget* b = top ? MCGuiFindButton(GTK_WIDGET(top), "Replace") : nullptr;
        return b && MCGuiOnScreen(b);
    });
    MCGuiStep("the question's words, as the local one's; Replace", [] {
        const std::string t = alertText();
        MCGuiNote("question: %s", t.c_str());
        GCHECK(t.find("Each becomes “pin”. Files open in a tab are changed there, unsaved, and can be "
                      "undone there; the others are written to disk. Left alone, since their tabs have unsaved "
                      "changes: fresh.c.") != std::string::npos);
        MCGuiSetKeyWindow(MCGuiTopWindow());
    });
    // The question's buttons are laid out a frame after its text shows.
    MCGuiClickWhenReady("the question's Replace button", [] {
        return MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Replace");
    });
    MCGuiWait("replaced on the host", 30, [] {
        SearchPanel* p = remoteWindow().search;
        return !p->replacing() && readFile(gFolder + "/src/util.c") == "int util() { return 1; }  /* pin two */\n" &&
               textOf(tabOf(gFolder + "/src/main.c")) == "int main() { return 0; }  /* pin one */\n";
    });
    MCGuiWait("the count said, with the held file named", 30, [] {
        SearchPanel* p = remoteWindow().search;
        return !p->searching() && p->statusText().rfind("Replaced 2 matches in 2 files (1 open in tabs, unsaved). "
                                                        "Left alone, with unsaved changes: fresh.c. Now ", 0) == 0;
    });
    MCGuiStep("main.c: the tab changed, unsaved, the disk as it was; fresh.c left to its window", [] {
        MCGuiNote("status: %s", remoteWindow().search->statusText().c_str());
        Document* m = tabOf(gFolder + "/src/main.c");
        GCHECK(m && m->dirty());
        GCHECK_EQ_STR(readFile(gFolder + "/src/main.c"), "int main() { return 0; }  /* needle one */\n");
        Document* f = tabOf(gFolder + "/src/fresh.c");
        GCHECK_EQ_STR(textOf(f), "edited needle held\n");
        GCHECK(f && f->dirty());
        GCHECK_EQ_STR(readFile(gFolder + "/src/fresh.c"), "needle held\n");
        // As it was for what follows: both tabs saved, then the files as they were.
        MCGuiSetKeyWindow(nullptr);
        remoteWindow().editor->openFile(gFolder + "/src/main.c");
    });
    MCGuiWait("main.c shown", 10, [] { return remoteWindow().editor->currentPath() == gFolder + "/src/main.c"; });
    MCGuiStep("main.c saved", [] {
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("main.c on disk as the host wrote it", 10, [] {
        Document* m = tabOf(gFolder + "/src/main.c");
        return m && !m->dirty() && readFile(gFolder + "/src/main.c") == "int main() { return 0; }  /* pin one */\n";
    });
    MCGuiStep("fresh.c saved", [] { remoteWindow().editor->openFile(gFolder + "/src/fresh.c"); });
    MCGuiWait("fresh.c shown", 10, [] { return remoteWindow().editor->currentPath() == gFolder + "/src/fresh.c"; });
    MCGuiStep("fresh.c Ctrl+S", [] {
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("fresh.c saved", 10, [] {
        Document* f = tabOf(gFolder + "/src/fresh.c");
        return f && !f->dirty() && readFile(gFolder + "/src/fresh.c") == "edited needle held\n";
    });
    MCGuiStep("the files as they were; the search closed", [] {
        if (Document* f = tabOf(gFolder + "/src/fresh.c")) remoteWindow().editor->closeTab(f);
        MCGuiWriteFile(gFolder + "/src/main.c", "int main() { return 0; }  /* needle one */\n");
        MCGuiWriteFile(gFolder + "/src/util.c", "int util() { return 1; }  /* needle two */\n");
        MCGuiWriteFile(gFolder + "/src/fresh.c", "int fresh;\n");
        gtk_widget_set_visible(remoteWindow().search->window(), FALSE);
        MCGuiSetKeyWindow(nullptr);
        remoteWindow().editor->openFile(gFile);   // notes.txt for what follows
    });
    MCGuiWait("notes.txt shown again", 10, [] { return remoteWindow().editor->currentPath() == gFile; });

    // ---- addresses an open window's folder holds (slice 3, the Mac's #327).
    // As a local file goes to the window whose folder holds it, an ssh://
    // address on the same machine opens in the window over SSH whose folder
    // holds it, through that window's host: no second window, no ssh run, the
    // path as the host resolved it (a link, "..").
    MCGuiStep("ssh://standin/<folder>/src/util.c opens in the open window, through its host", [] {
        Document* open = tabOf(gFolder + "/src/util.c");
        if (open) remoteWindow().editor->closeTab(open);
        GCHECK(tabOf(gFolder + "/src/util.c") == nullptr);
        gWindowsBefore = MCGuiAppWindows().size();
        gErrors.clear();
        MCGuiCommandLine({"ssh://standin" + gFolder + "/src/util.c"}, gFolder, &gErrors);
        GCHECK(gErrors.empty());
        GCHECK(alertText().empty());
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(standInPid() == gHostA);   // no ssh run for it
    });
    MCGuiWait("util.c a tab of that window, read through the host", 20, [] {
        return remoteWindow().editor->currentPath() == gFolder + "/src/util.c" &&
               textOf(tabOf(gFolder + "/src/util.c")) == "int util() { return 1; }  /* needle two */\n";
    });
    MCGuiStep("the same file through a link to the folder, and by \"..\": the same window and tab", [] {
        const std::string alias = rp(MCGuiScratch()) + "/remote-alias";
        unlink(alias.c_str());
        GCHECK(symlink(gFolder.c_str(), alias.c_str()) == 0);
        remoteWindow().editor->openFile(gFile);
        gErrors.clear();
        MCGuiCommandLine({"ssh://standin" + alias + "/src/main.c"}, gFolder, &gErrors);
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(standInPid() == gHostA);
        GCHECK(tabOf(alias + "/src/main.c") == nullptr);
    });
    MCGuiWait("main.c shown, spelled under the window's folder as the host resolved it", 20, [] {
        return remoteWindow().editor->currentPath() == gFolder + "/src/main.c";
    });
    MCGuiStep("and by ..", [] {
        remoteWindow().editor->openFile(gFile);
        MCGuiCommandLine({"ssh://standin" + gFolder + "/src/../src/main.c"}, gFolder, &gErrors);
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(gErrors.empty());
    });
    MCGuiWait("main.c shown again", 20, [] {
        return remoteWindow().editor->currentPath() == gFolder + "/src/main.c";
    });
    MCGuiStep("the folder's address, and the link's: no other window opens", [] {
        remoteWindow().editor->openFile(gFile);
        const std::string alias = rp(MCGuiScratch()) + "/remote-alias";
        MCGuiCommandLine({"ssh://standin" + gFolder}, gFolder, &gErrors);
        MCGuiCommandLine({"ssh://standin" + alias + "/"}, gFolder, &gErrors);
        GCHECK(gErrors.empty());
        GCHECK(alertText().empty());
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(standInPid() == gHostA);
        GCHECK_EQ_STR(remoteWindow().editor->currentPath(), gFile);   // brought forward, nothing else opened
    });
    MCGuiStep("ssh://standin/<folder>/src/fresh.txt, not there yet: an empty untitled tab of that window", [] {
        gErrors.clear();
        MCGuiCommandLine({"ssh://standin" + gFolder + "/src/fresh.txt"}, gFolder, &gErrors);
        GCHECK(gErrors.empty());
        GCHECK(alertText().empty());
        GCHECK(MCGuiAppWindows().size() == gWindowsBefore);
        GCHECK(standInPid() == gHostA);
        Document* d = tabOf(gFolder + "/src/fresh.txt");
        GCHECK(d != nullptr);
        if (d) MCGuiNote("fresh.txt dirty %d", (int)d->dirty());
        GCHECK_EQ_STR(textOf(d), "");
        GCHECK(access((gFolder + "/src/fresh.txt").c_str(), F_OK) != 0);
        GCHECK_EQ_STR(remoteWindow().editor->currentPath(), gFolder + "/src/fresh.txt");
        gtk_widget_grab_focus(remoteWindow().editor->textView());
    });
    MCGuiStep("typed, Ctrl+S", [] {
        MCGuiType("fresh\n");
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("made by the host's save", 10, [] {
        Document* d = tabOf(gFolder + "/src/fresh.txt");
        return readFile(gFolder + "/src/fresh.txt") == "fresh\n" && d && !d->dirty();
    });
    // What no open window holds: another machine's address (the stand-in
    // serves the same disk, so only the machine differs), and a file outside
    // the folder. Each opens a window of its own, closed after with its host.
    MCGuiStep("ssh://other/<folder>/src/util.c: another machine, a window of its own", [] {
        gErrors.clear();
        MCGuiCommandLine({"ssh://other" + gFolder + "/src/util.c"}, gFolder, &gErrors);
    });
    MCGuiWait("that window, on src, with util.c", 30, [] {
        return MCGuiAppWindows().size() == gWindowsBefore + 1 && remoteWindow().rootDir == gFolder + "/src" &&
               remoteWindow().editor->currentPath() == gFolder + "/src/util.c";
    });
    MCGuiStep("its label names the other machine; it is closed", [] {
        GCHECK(gErrors.empty());
        GCHECK_EQ_STR(remoteWindow().editor->sessionLink()->label(), "other:" + gFolder + "/src");
        gHostB = standInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        MCGuiSetKeyWindow(nullptr);
        gtk_window_close(remoteWindow().window);
    });
    MCGuiWait("that window gone, its host with it", 30, [] {
        return MCGuiAppWindows().size() == gWindowsBefore && remoteWindow().rootDir == gFolder && gone(gHostB);
    });
    MCGuiStep("ssh://standin/<outside>/elsewhere.txt: outside the folder, a window of its own", [] {
        const std::string outside = rp(MCGuiScratch()) + "/remote-outside";
        g_mkdir_with_parents(outside.c_str(), 0700);
        MCGuiWriteFile(outside + "/elsewhere.txt", "elsewhere\n");
        gErrors.clear();
        MCGuiCommandLine({"ssh://standin" + rp(outside) + "/elsewhere.txt"}, gFolder, &gErrors);
    });
    MCGuiWait("that window, on its folder, with the file", 30, [] {
        return MCGuiAppWindows().size() == gWindowsBefore + 1 &&
               remoteWindow().rootDir == rp(rp(MCGuiScratch()) + "/remote-outside") &&
               remoteWindow().editor->currentPath() == rp(MCGuiScratch()) + "/remote-outside/elsewhere.txt";
    });
    MCGuiStep("it is closed", [] {
        GCHECK(gErrors.empty());
        gHostB = standInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        MCGuiSetKeyWindow(nullptr);
        gtk_window_close(remoteWindow().window);
    });
    MCGuiWait("gone, its host with it; the first window shows again", 30, [] {
        return MCGuiAppWindows().size() == gWindowsBefore && remoteWindow().rootDir == gFolder && gone(gHostB);
    });
    MCGuiStep("the first window's notes.txt once more", [] {
        gHostB = 0;
        remoteWindow().editor->openFile(gFile);
    });
    MCGuiWait("notes.txt shown", 10, [] { return remoteWindow().editor->currentPath() == gFile; });

    // ---- opening a file does not block the window (third slice): the tab
    // shows "Opening..." at once, the pieces are asked on the main loop, the
    // worker decodes them, and a tick on the main loop never waits long.
    MCGuiStep("a big file on the host; what the old, blocking read cost the main thread", [] {
        gBig = gFolder + "/big.txt";
        std::string chunk;
        for (int i = 0; i < 100000; i++) chunk += "line " + std::to_string(i) + " of the big remote file, with some words\n";
        std::string all;
        for (int i = 0; i < 2; i++) all += chunk;   // about 10 MB
        std::ofstream(gBig, std::ios::binary) << all;
        gBigSize = all.size();
        SessionLink::Link* l = remoteWindow().editor->sessionLink();
        const gint64 t0 = g_get_monotonic_time();
        const SessionLink::Link::RemoteText r = l->readText(gBig);   // what loadInto did before
        gBlockingMs = (g_get_monotonic_time() - t0) / 1000.0;
        GCHECK(r.ok && r.text.size() == gBigSize);
        MCGuiNote("the old blocking read of %zu bytes held the main thread %.1f ms", gBigSize, gBlockingMs);
    });
    MCGuiStep("opened: the tab is there at once, loading, and the call returned quickly", [] {
        gLastTick = g_get_monotonic_time();
        gMaxGapMs = 0;
        gTicks = 0;
        gTickId = g_timeout_add(2, +[](gpointer) -> gboolean {
            const gint64 now = g_get_monotonic_time();
            gMaxGapMs = std::max(gMaxGapMs, (now - gLastTick) / 1000.0);
            gLastTick = now;
            gTicks++;
            return G_SOURCE_CONTINUE;
        }, nullptr);
        const gint64 t0 = g_get_monotonic_time();
        remoteWindow().editor->openFile(gBig);
        const double ms = (g_get_monotonic_time() - t0) / 1000.0;
        MCGuiNote("openFile returned after %.1f ms", ms);
        GCHECK(ms < 100);
        Document* d = tabOf(gBig);
        GCHECK(d != nullptr && d->loading());
        if (d) GCHECK(!d->canSave());   // nothing to type into or save while it is read
        GCHECK_EQ_STR(remoteWindow().editor->currentPath(), gBig);
        if (d) GCHECK(textOf(d).empty());   // the text is not there yet
    });
    MCGuiWait("the text is in", 60, [] {
        Document* d = tabOf(gBig);
        return d && !d->loading() && d->kind() == Document::Kind::Text &&
               (size_t)gtk_text_buffer_get_char_count(d->buffer()) == gBigSize;
    });
    MCGuiStep("the main loop kept running through the read; the text is the file's", [] {
        g_source_remove(gTickId);
        MCGuiNote("main loop: %d ticks, the longest gap %.1f ms (the blocking read: %.1f ms)", gTicks, gMaxGapMs,
                  gBlockingMs);
        GCHECK(gTicks > 20);
        GCHECK(gMaxGapMs < gBlockingMs);
        GCHECK(gMaxGapMs < 400);
        Document* d = tabOf(gBig);
        GCHECK(d && !d->dirty() && d->remote());
        if (d) {
            GCHECK(d->canSave());
            GCHECK(!d->diskStamp().exists);
            GCHECK_EQ_STR(textOf(d).substr(0, 40), "line 0 of the big remote file, with some");
        }
    });
    MCGuiStep("a tab closed while its file is read", [] {
        Editor* e = remoteWindow().editor;
        const int tabs = e->tabCount();
        gTabsBefore = tabs;
        e->closeTab(tabOf(gBig));
        GCHECK(tabOf(gBig) == nullptr);
        GCHECK(e->tabCount() == tabs - 1);
        e->openFile(gBig);
        Document* d = tabOf(gBig);
        GCHECK(d && d->loading());
        e->closeTab(d);   // the read is still going: its answer is ignored
        GCHECK(tabOf(gBig) == nullptr);
        GCHECK(e->tabCount() == tabs - 1);
    });
    MCGuiPause(0.5);   // time is the condition: the abandoned read's answers must arrive and be ignored
    MCGuiStep("the window still works after the abandoned read; a file that cannot be read says so", [] {
        Editor* e = remoteWindow().editor;
        GCHECK(e->tabCount() == gTabsBefore - 1);
        e->openFile(gFile);
        GCHECK(tabOf(gFile) != nullptr);
        std::ofstream(gFolder + "/binary.dat", std::ios::binary) << std::string("\x00\x01\x02\xff\xfe", 5);
        e->openFile(gFolder + "/binary.dat");
    });
    MCGuiWait("a message in the tab, as before", 20, [] {
        Document* d = tabOf(gFolder + "/binary.dat");
        return d && !d->loading() && d->kind() == Document::Kind::Message;
    });
    MCGuiStep("the message's words; the big file and the binary one closed", [] {
        Editor* e = remoteWindow().editor;
        Document* d = tabOf(gFolder + "/binary.dat");
        GCHECK(d && textOf(d).find("Could not open") != std::string::npos);
        if (d) e->closeTab(d);
        e->openFile(gFile);
    });
    MCGuiWait("notes.txt shown again after the big file", 10, [] { return remoteWindow().editor->currentPath() == gFile; });

    // ---- viewers over SSH: copies read through the host (the Mac's #348)
    MCGuiStep("a picture in a folder, a PDF, a CSV, a JSON, a page with pictures, a database, a sound, a huge file", [] {
        g_mkdir_with_parents((gFolder + "/pics").c_str(), 0755);
        writePng(gFolder + "/pics/pic.png", 37, 23);
        std::ofstream(gFolder + "/doc.pdf", std::ios::binary) << pdfOf(3);
        std::ofstream(gFolder + "/table.csv") << "name,qty\napple,3\npear,5\n";
        std::ofstream(gFolder + "/data.json") << "{\"a\": [1, 2, 3], \"b\": {\"c\": true}}\n";
        writePng(gFolder + "/pics/shown.png", 20, 10);
        writePng(rp(gFolder) + "-outside.png", 8, 8);
        std::ofstream(gFolder + "/page.md") << "# A page\n\n![inside](pics/shown.png)\n\n![outside](" << rp(gFolder)
                                           << "-outside.png)\n\n![away](../nothing.png)\n";
        std::ofstream(gFolder + "/x.db", std::ios::binary) << std::string("SQLite format 3\0", 16) << std::string(4080, '\0');
        std::ofstream(gFolder + "/a.mp3", std::ios::binary) << std::string(2048, 'x');
        if (FILE* f = fopen((gFolder + "/huge.dat").c_str(), "wb")) {
            fclose(f);
            GCHECK(truncate((gFolder + "/huge.dat").c_str(), (off_t)65 * 1024 * 1024 + 10) == 0);
        }
        remoteWindow().editor->openFile(gFolder + "/pics/pic.png");
        GCHECK(tabOf(gFolder + "/pics/pic.png") != nullptr);
    });
    MCGuiWait("the picture shown from a copy: 37 by 23, the tab on the remote path", 30, [] {
        const MCGuiAppWindow w = remoteWindow();
        Document* d = tabOf(gFolder + "/pics/pic.png");
        return d && !d->loading() && d->kind() == Document::Kind::Media && w.editor->media() &&
               w.editor->media()->kind() == MediaView::Kind::Image && w.editor->media()->imageWidth() == 37;
    });
    MCGuiStep("the copy: 0600 in a 0700 folder of the window's; the tab, title and no stamp stay remote", [] {
        const MCGuiAppWindow w = remoteWindow();
        Document* d = tabOf(gFolder + "/pics/pic.png");
        RemoteCopies* c = w.editor->remoteCopies();
        GCHECK(c != nullptr);
        if (!c || !d) return;
        gCopies = c->folder();
        GCHECK(!gCopies.empty());
        GCHECK(gCopies.rfind(RemoteCopies::root() + "/w", 0) == 0);
        GCHECK(modeIs(RemoteCopies::root(), 0700));
        GCHECK(modeIs(gCopies, 0700));
        GCHECK(w.editor->media()->imageHeight() == 23);
        GCHECK(d->showsCopy());
        GCHECK(d->viewerPath().rfind(gCopies + "/", 0) == 0);
        GCHECK(d->viewerPath() != d->path());
        GCHECK(g_str_has_suffix(d->viewerPath().c_str(), "/pic.png"));
        GCHECK(modeIs(d->viewerPath(), 0600));
        GCHECK(modeIs(gCopies + "/pics", 0700));
        GCHECK_EQ_STR(d->path(), gFolder + "/pics/pic.png");
        GCHECK(!d->diskStamp().exists);
        GCHECK(titleOf(w).find("pic.png") != std::string::npos);
        GCHECK(alertText().empty());
        GCHECK(c->refreshes == 0);
    });
    MCGuiStep("the picture rewritten on the host at 50 by 40", [] {
        writePng(gFolder + "/pics/pic.png", 50, 40);
    });
    MCGuiWait("shown again from the host's notice", 30, [] {
        const MCGuiAppWindow w = remoteWindow();
        return w.editor->remoteCopies() && w.editor->remoteCopies()->refreshes >= 1 && w.editor->media() &&
               w.editor->media()->imageWidth() == 50 && w.editor->media()->imageHeight() == 40;
    });
    MCGuiStep("the PDF opened", [] { remoteWindow().editor->openFile(gFolder + "/doc.pdf"); });
    MCGuiWait("three pages from a copy", 30, [] {
        const MCGuiAppWindow w = remoteWindow();
        Document* d = tabOf(gFolder + "/doc.pdf");
        return d && !d->loading() && d->kind() == Document::Kind::Media && w.editor->media() &&
               w.editor->media()->kind() == MediaView::Kind::Pdf && w.editor->media()->pageCount() == 3;
    });
    MCGuiStep("the CSV opened", [] { remoteWindow().editor->openFile(gFolder + "/table.csv"); });
    MCGuiWait("its table", 30, [] {
        Document* d = tabOf(gFolder + "/table.csv");
        return d && !d->loading() && d->kind() == Document::Kind::Text && remoteWindow().editor->showsTable();
    });
    MCGuiStep("the JSON opened", [] { remoteWindow().editor->openFile(gFolder + "/data.json"); });
    MCGuiWait("its outline", 30, [] {
        Document* d = tabOf(gFolder + "/data.json");
        return d && !d->loading() && d->kind() == Document::Kind::Text && remoteWindow().editor->showsOutline();
    });
    MCGuiStep("the Markdown page opened", [] { remoteWindow().editor->openFile(gFolder + "/page.md"); });
    MCGuiWait("its picture inside the folder shown, from a copy, once it was fetched", 30, [] {
        Document* d = tabOf(gFolder + "/page.md");
        if (!d || d->loading() || !d->showsPreview() || !remoteWindow().editor->inPreview()) return false;
        for (const Markdown::Embed& e : d->pageInfo().embeds)
            if (e.widget && e.width == 20 && e.height == 10) return true;
        return false;
    });
    MCGuiStep("only that picture: the one outside the folder and the one by .. stay their alt text", [] {
        Document* d = tabOf(gFolder + "/page.md");
        int pictures = 0;
        if (d)
            for (const Markdown::Embed& e : d->pageInfo().embeds)
                if (e.widget) pictures++;
        GCHECK(pictures == 1);
        RemoteCopies* c = remoteWindow().editor->remoteCopies();
        GCHECK(c && c->hasCopyOf(gFolder + "/pics/shown.png"));
        if (c) GCHECK(c->localPathOf(gFolder + "/pics/shown.png").rfind(gCopies + "/", 0) == 0);
        GCHECK(c && !c->hasCopyOf(rp(gFolder) + "-outside.png"));
    });
    MCGuiStep("the database, the sound and the huge file opened", [] {
        remoteWindow().editor->openFile(gFolder + "/x.db");
        remoteWindow().editor->openFile(gFolder + "/a.mp3");
        remoteWindow().editor->openFile(gFolder + "/huge.dat");
    });
    MCGuiWait("the three read", 60, [] {
        for (const char* n : {"/x.db", "/a.mp3", "/huge.dat"}) {
            Document* d = tabOf(gFolder + n);
            if (!d || d->loading()) return false;
        }
        return true;
    });
    MCGuiStep("the database in its viewer from a copy, the sound opened without an alert, the huge file's note and no copy", [] {
        Document* db = tabOf(gFolder + "/x.db");
        GCHECK(db && db->kind() == Document::Kind::Database && db->showsCopy());
        Document* snd = tabOf(gFolder + "/a.mp3");
        GCHECK(snd && (snd->kind() == Document::Kind::Media || snd->kind() == Document::Kind::Message));
        Document* big = tabOf(gFolder + "/huge.dat");
        GCHECK(big && big->kind() == Document::Kind::Message);
        if (big) {
            GCHECK(textOf(big).find("too large to open over SSH") != std::string::npos);
            GCHECK(!big->showsCopy());
        }
        RemoteCopies* c = remoteWindow().editor->remoteCopies();
        GCHECK(c && !c->hasCopyOf(gFolder + "/huge.dat"));
        GCHECK(alertText().empty());
    });
    MCGuiStep("the viewers' tabs closed", [] {
        Editor* e = remoteWindow().editor;
        for (const char* n : {"/pics/pic.png", "/doc.pdf", "/table.csv", "/data.json", "/page.md", "/x.db", "/a.mp3", "/huge.dat"})
            if (Document* d = tabOf(gFolder + n)) e->closeTab(d);
        e->openFile(gFile);
    });
    MCGuiWait("notes.txt shown again after the viewers", 10, [] { return remoteWindow().editor->currentPath() == gFile; });

    // ---- a terminal in the host's shell
    MCGuiStep("a terminal", [] {
        Terminal* t = remoteWindow().terminal;
        GCHECK(t != nullptr);
        if (t) t->focus();
        gSent = 0;
    });
    // The parent's name as ps gives it: the host itself; the variable is the
    // stand-in's, not this process's.
    MCGuiWait("the terminal runs in the host's shell and environment", 40, [] {
        return runUntil("echo \"remote-$SSH_STANDIN-$((6*7))\" \"parent-$(basename \"$(ps -o comm= -p $PPID)\")\"",
                        "remote-yes-42 parent-mutible");
    });

    // ---- the remote shell's own integration, set up by its host: the marks
    // are believed like a local shell's. The login shell here is bash (zsh,
    // where installed, would be the same through the same code); the stand-in
    // runs this machine's binary, so the host finds the scripts beside it.
    MCGuiWait("the remote shell's prompt is marked", 30, [] {
        Terminal* t = remoteWindow().terminal;
        return t && t->shellState().known && t->shellState().atPrompt && !t->shellState().busy;
    });
    MCGuiStep("marked: a prompt counted, no status yet needed; sleep 2 typed", [] {
        Terminal* t = remoteWindow().terminal;
        GCHECK(t && t->shellState().prompts >= 1 && t->shellState().started == t->shellState().done);
        if (!t) return;
        const std::string line = "sleep 2\n";
        vte_terminal_feed_child(VTE_TERMINAL(t->vteWidget()), line.c_str(), (gssize)line.size());
    });
    MCGuiWait("busy with the command's text", 10, [] {
        Terminal* t = remoteWindow().terminal;
        return t && t->shellState().busy && t->shellState().command == "sleep 2" && t->busy();
    });
    MCGuiWait("idle again at the prompt, status 0", 15, [] {
        Terminal* t = remoteWindow().terminal;
        return t && t->shellState().atPrompt && !t->shellState().busy && t->shellState().status == 0 &&
               t->shellState().command.empty();
    });
    MCGuiStep("false typed", [] {
        Terminal* t = remoteWindow().terminal;
        GCHECK(t != nullptr);
        if (!t) return;
        const std::string line = "false\n";
        vte_terminal_feed_child(VTE_TERMINAL(t->vteWidget()), line.c_str(), (gssize)line.size());
    });
    MCGuiWait("the exit status 1 is seen", 10, [] {
        Terminal* t = remoteWindow().terminal;
        return t && t->shellState().atPrompt && t->shellState().status == 1;
    });

    // ---- the lasting host: which process holds the shell
    MCGuiStep("the shell's ids", [] { gSent = 0; });
    MCGuiWait("the shell's pid and its parent's, the lasting host", 40, [] {
        Terminal* t = remoteWindow().terminal;
        if (!t) return false;
        if (!runUntil("MARK=kept; echo \"ids-$$-$PPID-end\"", "-end")) {
            // The echoed line holds the variables unexpanded; wait for digits.
        }
        const std::string screen = t->recentText(40);
        const std::size_t at = screen.rfind("ids-");
        if (at == std::string::npos) return false;
        int shell = 0, parent = 0;
        char tail[8] = {0};
        if (std::sscanf(screen.c_str() + at, "ids-%d-%d-%7s", &shell, &parent, tail) < 3) return false;
        gShell = shell;
        gLasting = parent;
        return shell > 0 && parent > 0;
    });
    MCGuiStep("the shell's parent is a detached host of its own, not the connection's relay", [] {
        MCGuiNote("shell %d, lasting host %d, relay %d", gShell, gLasting, gHostA);
        GCHECK(gLasting > 0 && gLasting != gHostA && !gone(gLasting));
        // Its socket is in the lasting folder (this world's temporary folder).
        const std::string name = Shared::hostSocketName(gFolder);
        gSocketOfLasting = SessionLink::lastingSocketFolder() + "/" + name;
        GCHECK(g_file_test(gSocketOfLasting.c_str(), G_FILE_TEST_EXISTS));
        GCHECK(gSocketOfLasting.rfind("/tmp/mcgui-", 0) == 0);
        MCGuiSetKeyWindow(nullptr);
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("before the drop\n");
    });
    MCGuiWait("typed, unsaved", 10, [] {
        Document* d = tabOf(gFile);
        return d && d->dirty() && textOf(d) == "first line\ntyped over ssh\nbefore the drop\n";
    });

    // ---- the connection drops; the stand-in then refuses, so the window is
    // seen trying
    MCGuiStep("the connection drops, and ssh is refused for a while", [] {
        GCHECK(!remoteWindow().editor->sessionLink()->lost());
        gDocId = tabOf(gFile)->id();
        MCGuiWriteFile(gBlock, "x");
        gShownBefore = ConnectWindow::shownCount();
        // The stand-in leads its own process group (the spawn's session): the
        // relay goes with it, but not the lasting host (a session of its own).
        if (gHostA > 0) kill(-gHostA, SIGKILL);
    });
    MCGuiWait("the window says it is reconnecting, with no Reconnect button", 20, [] {
        const MCGuiAppWindow w = remoteWindow();
        if (!w.editor || !w.editor->sessionLink()->lost() || !w.autoReconnect) return false;
        GtkWidget* b = MCGuiFindButton(w.statusBar, "Reconnect");
        if (statusText().find("Reconnecting; your text is kept here meanwhile") != std::string::npos) gSawNote = true;
        return titleOf(w).find("(reconnecting)") != std::string::npos && (!b || !gtk_widget_get_visible(b));
    });
    MCGuiStep("the lasting host and its shell run on; the text takes typing", [] {
        GCHECK(!gone(gLasting) && !gone(gShell));
        GCHECK(gSawNote);   // the status bar said so
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("while away\n");
    });
    MCGuiWait("typed while away", 10, [] {
        Document* d = tabOf(gFile);
        return d && d->dirty() && textOf(d) == "first line\ntyped over ssh\nbefore the drop\nwhile away\n";
    });
    MCGuiWait("two tries made and refused, still reconnecting", 30, [] {
        const MCGuiAppWindow w = remoteWindow();
        return w.reconnectAttempt && *w.reconnectAttempt >= 2 && w.autoReconnect &&
               titleOf(w).find("(reconnecting)") != std::string::npos;
    });
    MCGuiStep("ssh works again", [] { unlink(gBlock.c_str()); });
    MCGuiWait("connected by itself, to the same host", 40, [] {
        Document* d = tabOf(gFile);
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && !w.editor->sessionLink()->lost() && d && d->hostSavesFile() && !w.autoReconnect &&
               titleOf(w).find("(reconnecting)") == std::string::npos &&
               titleOf(w).find("(disconnected)") == std::string::npos;
    });
    MCGuiStep("the document is the host's own: its id, its text with what was typed away", [] {
        gHostB = standInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        GCHECK(gone(gHostA));
        GCHECK(ConnectWindow::shownCount() == gShownBefore);   // the tries by themselves showed no window
        GCHECK(!gone(gLasting));   // the same host
        Document* d = tabOf(gFile);
        GCHECK(d && d->dirty());
        GCHECK(d && d->id() == gDocId);   // taken up again, not opened anew
        GCHECK_EQ_STR(textOf(d), "first line\ntyped over ssh\nbefore the drop\nwhile away\n");
        GCHECK_EQ_STR(remoteWindow().editor->sessionLink()->checkDocuments({{d->id(), textOf(d)}}), "");
        GCHECK(statusText().find("your files, undo and terminals are as they were") != std::string::npos);
        gSent = 0;
    });
    MCGuiWait("the terminal is the same shell (its variable, pid and parent)", 40, [] {
        return runUntil("echo \"same-$MARK-$$-$PPID-done\"",
                        "same-kept-" + std::to_string(gShell) + "-" + std::to_string(gLasting) + "-done");
    });
    MCGuiStep("Ctrl+S saves it through the new connection", [] {
        MCGuiSetKeyWindow(nullptr);
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved by the host", 10, [] {
        Document* d = tabOf(gFile);
        return readFile(gFile) == "first line\ntyped over ssh\nbefore the drop\nwhile away\n" && d && !d->dirty();
    });

    // ---- a host that never comes back: the tries run out, Reconnect shows
    MCGuiStep("the connection drops again, ssh refused for good", [] {
        MCGuiWriteFile(gBlock, "x");
        if (gHostB > 0) kill(-gHostB, SIGKILL);
    });
    MCGuiWait("trying again", 20, [] {
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && w.editor->sessionLink()->lost() && w.autoReconnect;
    });
    MCGuiStep("the schedule is the core's; the last try's turn comes", [] {
        GCHECK(Shared::reconnectDelayMs(0) == 500 && Shared::reconnectDelayMs(1) == 1000);
        GCHECK(Shared::reconnectDelayMs(6) == 30000 && Shared::reconnectDelayMs(Shared::kReconnectAttempts - 1) == 30000);
        GCHECK(Shared::reconnectDelayMs(Shared::kReconnectAttempts) == -1);
        // The tries so far set to all but the last: the next one is a real
        // try, refused, and then none is left.
        const MCGuiAppWindow w = remoteWindow();
        if (w.reconnectAttempt) *w.reconnectAttempt = Shared::kReconnectAttempts - 1;
    });
    MCGuiWait("the tries run out: disconnected, Reconnect shows, its words", 60, [] {
        const MCGuiAppWindow w = remoteWindow();
        GtkWidget* b = w.editor ? MCGuiFindButton(w.statusBar, "Reconnect") : nullptr;
        return w.editor && !w.autoReconnect && b && gtk_widget_get_visible(b) &&
               titleOf(w).find("(disconnected)") != std::string::npos &&
               statusText().find("Could not reconnect to standin:") == 0;
    });
    MCGuiStep("ssh works but slowly; Reconnect, by the user", [] {
        MCGuiNote("status: %s", statusText().c_str());
        GCHECK(!gone(gLasting));   // the host kept waiting
        GCHECK(ConnectWindow::shownCount() == gShownBefore);   // all the tries by themselves: none
        unlink(gBlock.c_str());
        MCGuiWriteFile(gSlow, "x");
        unlink(gPidFile.c_str());
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Reconnect"));
    });
    MCGuiWait("Reconnect says so in the status bar, with Stop; no connecting window", 20, [] {
        GtkWidget* stop = MCGuiFindButton(remoteWindow().statusBar, "Stop");
        return standInPid() > 0 && stop && gtk_widget_get_visible(stop) &&
               statusText().find("Reconnecting to standin:") == 0;
    });
    MCGuiStep("no window was shown for it; ssh is running; Stop", [] {
        GCHECK(ConnectWindow::shownCount() == gShownBefore);
        GCHECK(ConnectWindow::pending().empty());
        GtkWidget* r = MCGuiFindButton(remoteWindow().statusBar, "Reconnect");
        GCHECK(!r || !gtk_widget_get_visible(r));
        gCancelledPid = standInPid();
        GCHECK(gCancelledPid > 0 && !gone(gCancelledPid));
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Stop"));
    });
    MCGuiWait("ssh gone, the window still disconnected with Reconnect shown, Stop hidden", 20, [] {
        const MCGuiAppWindow w = remoteWindow();
        GtkWidget* b = w.editor ? MCGuiFindButton(w.statusBar, "Reconnect") : nullptr;
        GtkWidget* stop = w.editor ? MCGuiFindButton(w.statusBar, "Stop") : nullptr;
        return gone(gCancelledPid) && kill(-gCancelledPid, 0) != 0 && b && gtk_widget_get_visible(b) &&
               (!stop || !gtk_widget_get_visible(stop)) && statusText().find("Stopped connecting to standin:") == 0;
    });
    MCGuiStep("still lost and disconnected, nothing opened or said; Reconnect again", [] {
        GCHECK(remoteWindow().editor->sessionLink()->lost());
        GCHECK(titleOf(remoteWindow()).find("(disconnected)") != std::string::npos);
        GCHECK(alertText().empty());
        GCHECK(ConnectWindow::shownCount() == gShownBefore);
        unlink(gPidFile.c_str());
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Reconnect"));
    });
    MCGuiWait("trying again, with Stop", 20, [] {
        GtkWidget* stop = MCGuiFindButton(remoteWindow().statusBar, "Stop");
        return standInPid() > 0 && stop && gtk_widget_get_visible(stop);
    });
    MCGuiStep("Esc in the window", [] {
        gCancelledPid = standInPid();
        GCHECK(gCancelledPid > 0 && !gone(gCancelledPid));
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("escape");
    });
    MCGuiWait("Esc stopped it too: ssh gone, Reconnect shown", 20, [] {
        const MCGuiAppWindow w = remoteWindow();
        GtkWidget* b = w.editor ? MCGuiFindButton(w.statusBar, "Reconnect") : nullptr;
        return gone(gCancelledPid) && kill(-gCancelledPid, 0) != 0 && b && gtk_widget_get_visible(b) &&
               statusText().find("Stopped connecting to standin:") == 0;
    });
    MCGuiStep("Reconnect a third time, slow no more", [] {
        GCHECK(remoteWindow().editor->sessionLink()->lost());
        GCHECK(ConnectWindow::shownCount() == gShownBefore);
        unlink(gSlow.c_str());
        unlink(gPidFile.c_str());
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Reconnect"));
    });
    MCGuiWait("connected again, to the same host", 30, [] {
        Document* d = tabOf(gFile);
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && !w.editor->sessionLink()->lost() && d && d->hostSavesFile() &&
               titleOf(w).find("(disconnected)") == std::string::npos;
    });
    MCGuiStep("a file made on disk after Reconnect; no connecting window for any of the three", [] {
        GCHECK(ConnectWindow::shownCount() == gShownBefore);
        gHostB = standInPid();
        GCHECK(gHostB > 0 && !gone(gLasting));
        GCHECK(tabOf(gFile) && tabOf(gFile)->id() == gDocId && !tabOf(gFile)->dirty());
        gNoticesBefore = remoteWindow().remoteWs->notices;
        MCGuiWriteFile(gFolder + "/after-reconnect.txt", "x\n");
    });
    MCGuiWait("it appears in the tree, from the new connection", 30, [] {
        return hasRow("after-reconnect.txt", 0) && remoteWindow().remoteWs->notices > gNoticesBefore;
    });
    MCGuiStep("the terminal, still the same shell", [] {
        Terminal* t = remoteWindow().terminal;
        GCHECK(t != nullptr);
        if (t) t->focus();
        gSent = 0;
    });
    MCGuiWait("it runs a command on the host, with its variable", 40, [] {
        return runUntil("echo \"again-$MARK-$((7*8))\"", "again-kept-56");
    });

    // ---- the window closes: its shell and file close on the lasting host, which then ends
    MCGuiStep("the window closes", [] {
        GCHECK(alertText().empty());
        MCGuiSetKeyWindow(nullptr);
        gtk_window_close(remoteWindow().window);
    });
    MCGuiWait("gone", 15, [] { return remoteWindow().editor == nullptr && MCGuiAppWindows().size() < gWindowsBefore; });
    MCGuiWait("the connection's relay ends with it", 20, [] { return gone(gHostB); });
    MCGuiWait("its shell ended on the host, and the host, with nothing left, ended after its idle time", 90, [] {
        return gone(gShell) && gone(gLasting);
    });
    MCGuiStep("nothing left", [] {
        GCHECK(!gCopies.empty() && !g_file_test(gCopies.c_str(), G_FILE_TEST_EXISTS));   // the window's folder of copies went with it
        if (gHostB > 0 && !gone(gHostB)) kill(-gHostB, SIGKILL);   // the suite's own, by its recorded pid
        if (gLasting > 0 && !gone(gLasting)) kill(gLasting, SIGTERM);
        if (gShell > 0 && !gone(gShell)) kill(gShell, SIGKILL);
        unlink(gSocketOfLasting.c_str());
        unlink(gBlock.c_str());
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        EnvLock::envUnset("MUTIBLE_SSH");
        MCGuiSetKeyWindow(nullptr);   // the X focus back on a window that is open
    });

    // ---- (b) the GTK window's own host shares a lasting host that is running for the folder
    MCGuiStep("a lasting host started for another folder, as `mutible host --stdio` starts one", [] {
        const std::string dir = rp(MCGuiScratch()) + "/lasting-shared";
        g_mkdir_with_parents(dir.c_str(), 0700);
        gLastingFolderPath = rp(dir);
        MCGuiWriteFile(gLastingFolderPath + "/shared.txt", "shared\n");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "0.15");
        std::string why;
        gHostPidB = Shared::startDetached({MCGuiBuildDir() + "/mutible", "host", "--folder", gLastingFolderPath,
                                           "--lasting"},
                                          rp(MCGuiScratch()) + "/lasting-shared.log", &why);
        GCHECK(gHostPidB > 0);
        GCHECK(why.empty());
    });
    MCGuiWait("its socket is in the lasting folder", 20, [] {
        const std::vector<std::string> dirs =
            Shared::withLastingFolder(ControlChannel::socketFolders(), SessionLink::lastingSocketFolder());
        gSocketOfLasting = Shared::findHostSocket(gLastingFolderPath, dirs);
        return !gSocketOfLasting.empty();
    });
    MCGuiStep("MUTIBLE_HOST unset (the default: a host in the app), a window on that folder", [] {
        GCHECK(gSocketOfLasting.rfind(SessionLink::lastingSocketFolder() + "/host-", 0) == 0);
        gHadHost = getenv("MUTIBLE_HOST") != nullptr;
        gHostMode = gHadHost ? getenv("MUTIBLE_HOST") : "";
        EnvLock::envUnset("MUTIBLE_HOST");
        gWindowsBefore = MCGuiAppWindows().size();
        MCGuiOpenWindow(gLastingFolderPath);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiStep("it attached to that host over its socket, and made none of its own", [] {
        SessionLink::Link* l = MCGuiApp().editor ? MCGuiApp().editor->sessionLink() : nullptr;
        GCHECK(l != nullptr);
        if (!l) return;
        MCGuiNote("link kind %d, socket %s", (int)l->kind(), l->socketPath().c_str());
        GCHECK(l->kind() == SessionLink::Link::Kind::Socket);
        GCHECK_EQ_STR(l->socketPath(), gSocketOfLasting);
        GCHECK(l->startedHost() == 0);   // it started nothing
        GCHECK(l->client().idBase() >= Shared::kIdRange && !l->lost());
        GCHECK(!gone(gHostPidB));
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("the window gone", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore; });
    MCGuiStep("a folder with no lasting host keeps the default: a host in the app", [] {
        const std::string dir = rp(MCGuiScratch()) + "/no-lasting";
        g_mkdir_with_parents(dir.c_str(), 0700);
        MCGuiOpenWindow(rp(dir));
        MCGuiSetKeyWindow(nullptr);
        SessionLink::Link* l = MCGuiApp().editor ? MCGuiApp().editor->sessionLink() : nullptr;
        GCHECK(l && l->kind() == SessionLink::Link::Kind::InProcess);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("that window gone", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore; });
    MCGuiStep("the lasting host of this check ended by its pid, its socket removed", [] {
        if (gHadHost) EnvLock::envSet("MUTIBLE_HOST", gHostMode.c_str());
        else EnvLock::envUnset("MUTIBLE_HOST");
        if (gHostPidB > 0 && !gone(gHostPidB)) kill(gHostPidB, SIGTERM);
    });
    MCGuiWait("it ended", 30, [] { return gone(gHostPidB); });
    MCGuiStep("nothing of it left", [] {
        unlink(gSocketOfLasting.c_str());
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}
