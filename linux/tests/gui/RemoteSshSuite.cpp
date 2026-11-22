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
//     host; New Folder is refused with its note; Ctrl+P lists the host's
//     files without the ignored log and opens src/util.c; Find in Folder
//     finds "2 matches in 2 files" with the log left out, a row opens
//     src/main.c, and Replace All goes through the host's replace (a host
//     without it refuses with a note; the closed file is rewritten, the clean
//     tab shows the new text unsaved, a tab with changes is left alone and
//     named, the question and the count in the local words); after
//     Reconnect a file made on disk appears from the new host;
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

#include "ControlPaths.h"
#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "FileTree.h"
#include "PalettePanel.h"
#include "RemoteWorkspace.h"
#include "Search.h"
#include "SessionLink.h"
#include "Terminal.h"

namespace {

std::string gFolder, gFile, gSsh, gArgs, gPidFile, gBin, gBlock;
std::string gBig;
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
    MCGuiStep("the stand-in back", [] {
        EnvLock::envSet("MUTIBLE_SSH", gSsh.c_str());
        gErrors.clear();
        MCGuiCommandLine({"ssh://standin" + gFile}, gFolder, &gErrors);
    });

    // ---- the window
    MCGuiWait("a window on the remote host", 30, [] { return remoteWindow().editor != nullptr; });
    MCGuiWait("notes.txt read through the host", 20, [] {
        return textOf(tabOf(gFile)) == "first line\n" && remoteWindow().editor->currentPath() == gFile;
    });
    MCGuiStep("its title, its tab, how ssh was run", [] {
        MCGuiSetKeyWindow(nullptr);
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
    MCGuiStep("New Folder is refused, with its note, and no folder is made", [] {
        g_action_group_activate_action(G_ACTION_GROUP(remoteWindow().window), "newfolder", nullptr);
    });
    MCGuiWait("the note", 10, [] { return statusText().find("New Folder does not work in a window over SSH") == 0; });
    MCGuiStep("nothing made on disk", [] {
        MCGuiNote("status: %s", statusText().c_str());
        GCHECK(statusText().find("the files are on standin:" + gFolder) != std::string::npos);
        GCHECK(!g_file_test((gFolder + "/src/untitled folder").c_str(), G_FILE_TEST_EXISTS));
        GCHECK(remoteWindow().tree->namePopover() == nullptr);
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
    MCGuiStep("the question's words, as the local one's; Replace", [] {
        const std::string t = alertText();
        MCGuiNote("question: %s", t.c_str());
        GCHECK(t.find("Each becomes “pin”. Files open in a tab are changed there, unsaved, and can be "
                      "undone there; the others are written to disk. Left alone, since their tabs have unsaved "
                      "changes: fresh.c.") != std::string::npos);
        GtkWindow* top = MCGuiTopWindow();
        MCGuiSetKeyWindow(top);
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(top), "Replace"));
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
    MCGuiStep("ssh works; Reconnect, by the user", [] {
        MCGuiNote("status: %s", statusText().c_str());
        GCHECK(!gone(gLasting));   // the host kept waiting
        unlink(gBlock.c_str());
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Reconnect"));
    });
    MCGuiWait("connected again, to the same host", 30, [] {
        Document* d = tabOf(gFile);
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && !w.editor->sessionLink()->lost() && d && d->hostSavesFile() &&
               titleOf(w).find("(disconnected)") == std::string::npos;
    });
    MCGuiStep("a file made on disk after Reconnect", [] {
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
