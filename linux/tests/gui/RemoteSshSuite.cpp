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
//     src/main.c, and Replace All is refused, the file untouched; after
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
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string gFolder, gFile, gSsh, gArgs, gPidFile, gBin;
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
        gArgs = scratch + "/ssh-args.txt";
        gPidFile = scratch + "/ssh-pid.txt";
        gSsh = scratch + "/ssh-stand-in";
        writeScript(gSsh,
                    "#!/bin/sh\n"
                    "# A stand-in for ssh (RemoteSshSuite.cpp): its arguments recorded,\n"
                    "# its options skipped to the --, then the host, then the remote\n"
                    "# command run as ssh's remote shell runs it.\n"
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
    MCGuiStep("Replace All is refused", [] {
        SearchPanel* p = remoteWindow().search;
        p->showReplace();
        MCGuiSetKeyWindow(GTK_WINDOW(p->window()));
        MCGuiKey("ctrl+alt+enter");   // Replace All
    });
    MCGuiWait("its note", 10, [] {
        return remoteWindow().search->statusText().find("Replace in Folder does not work in a window over SSH") == 0;
    });
    MCGuiStep("the files untouched; the search closed", [] {
        GCHECK_EQ_STR(readFile(gFolder + "/src/main.c"), "int main() { return 0; }  /* needle one */\n");
        GCHECK_EQ_STR(readFile(gFolder + "/src/util.c"), "int util() { return 1; }  /* needle two */\n");
        gtk_widget_set_visible(remoteWindow().search->window(), FALSE);
        MCGuiSetKeyWindow(nullptr);
        remoteWindow().editor->openFile(gFile);   // notes.txt for what follows
    });
    MCGuiWait("notes.txt shown again", 10, [] { return remoteWindow().editor->currentPath() == gFile; });

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

    // ---- the connection drops
    MCGuiStep("the connection drops", [] {
        GCHECK(!remoteWindow().editor->sessionLink()->lost());
        // The stand-in leads its own process group (the spawn's session): the
        // host and anything it started go with it.
        if (gHostA > 0) kill(-gHostA, SIGKILL);
    });
    MCGuiWait("the window says it is disconnected", 20, [] {
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && w.editor->sessionLink()->lost() && titleOf(w).find("(disconnected)") != std::string::npos;
    });
    MCGuiStep("the text stays, and takes more typing; a save says why not", [] {
        const MCGuiAppWindow w = remoteWindow();
        Document* d = tabOf(gFile);
        GCHECK_EQ_STR(textOf(d), "first line\ntyped over ssh\n");
        GCHECK(d && !d->hostSavesFile());
        GCHECK(w.editor->sessionLink()->failure().find("standin") != std::string::npos);
        GtkWidget* b = MCGuiFindButton(w.statusBar, "Reconnect");
        GCHECK(b != nullptr && gtk_widget_get_visible(b));
        gtk_widget_grab_focus(w.editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("while away\n");
    });
    MCGuiWait("typed while away", 10, [] {
        Document* d = tabOf(gFile);
        return d && d->dirty() && textOf(d) == "first line\ntyped over ssh\nwhile away\n";
    });
    MCGuiStep("Ctrl+S: refused, the file untouched", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("the refusal's words", 10, [] {
        return alertText().find("the connection there is lost") != std::string::npos ||
               allText(GTK_WIDGET(remoteWindow().window)).find("the connection there is lost") != std::string::npos;
    });
    MCGuiStep("the file on disk is as the host left it; Reconnect", [] {
        MCGuiNote("refusal: %s", alertText().c_str());
        GCHECK_EQ_STR(readFile(gFile), "first line\ntyped over ssh\n");
        GCHECK(tabOf(gFile)->dirty());
        dismissAlert();
    });
    MCGuiWait("the refusal dismissed", 10, [] { return alertText().empty(); });
    MCGuiStep("Reconnect", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiClick(MCGuiFindButton(remoteWindow().statusBar, "Reconnect"));
    });
    MCGuiWait("connected again", 30, [] {
        Document* d = tabOf(gFile);
        const MCGuiAppWindow w = remoteWindow();
        return w.editor && !w.editor->sessionLink()->lost() && d && d->hostSavesFile() &&
               titleOf(w).find("(disconnected)") == std::string::npos;
    });
    MCGuiStep("the file is open on the new host with everything typed; Ctrl+S saves it there", [] {
        gHostB = standInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        GCHECK(gone(gHostA));
        Document* d = tabOf(gFile);
        GCHECK(d && d->dirty());
        GCHECK_EQ_STR(remoteWindow().editor->sessionLink()->checkDocuments({{d->id(), textOf(d)}}), "");
        MCGuiSetKeyWindow(nullptr);
        gtk_widget_grab_focus(remoteWindow().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved by the new host", 10, [] {
        Document* d = tabOf(gFile);
        return readFile(gFile) == "first line\ntyped over ssh\nwhile away\n" && d && !d->dirty();
    });
    MCGuiStep("a file made on disk after Reconnect", [] {
        gNoticesBefore = remoteWindow().remoteWs->notices;
        MCGuiWriteFile(gFolder + "/after-reconnect.txt", "x\n");
    });
    MCGuiWait("it appears in the tree, from the new host", 30, [] {
        return hasRow("after-reconnect.txt", 0) && remoteWindow().remoteWs->notices > gNoticesBefore;
    });
    MCGuiStep("the terminal's new shell, there", [] {
        Terminal* t = remoteWindow().terminal;
        GCHECK(t != nullptr);
        if (t) t->focus();
        gSent = 0;
    });
    MCGuiWait("it runs a command on the new host", 40, [] {
        return runUntil("echo \"again-$SSH_STANDIN-$((7*8))\"", "again-yes-56");
    });

    // ---- the window closes, and its host with it
    MCGuiStep("the window closes", [] {
        GCHECK(alertText().empty());
        MCGuiSetKeyWindow(nullptr);
        gtk_window_close(remoteWindow().window);
    });
    MCGuiWait("gone", 15, [] { return remoteWindow().editor == nullptr && MCGuiAppWindows().size() < gWindowsBefore; });
    MCGuiWait("the host it ran ends with it", 20, [] { return gone(gHostB); });
    MCGuiStep("nothing left", [] {
        if (gHostB > 0 && !gone(gHostB)) kill(-gHostB, SIGKILL);   // the suite's own, by its recorded pid
        EnvLock::envUnset("MUTIBLE_SSH");
        MCGuiSetKeyWindow(nullptr);   // the X focus back on a window that is open
    });
}
