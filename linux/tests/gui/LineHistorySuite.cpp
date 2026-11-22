// LineHistorySuite.cpp — `./build/mutible-guitest line_history`: Show Line
// History on Linux (LineHistoryView.h, the core's LineHistory.h), the Mac's
// tests/gui/LineHistorySuite.mm. A scratch repository holds hello.py in two
// commits by two authors; a Claude Code session (a transcript in a scratch
// projects folder, named through MUTIBLE_CLAUDE_PROJECTS: the real
// ~/.claude is never read or written) then edited the file to add a
// function, and a line was typed by hand after. Resting on a line number
// shows the line's commit (hash, author, subject), the session (its title,
// the prompt the edit answered, when) or "Not committed"; View > Show Line
// History, through the palette, puts the words beside every line and a
// second press takes them away; a click on a commit's words opens the
// commit's page. An edit hides the words from the edited line on until the
// next look, which finds them on their new lines.
#include "GitChecks.h"

#include "AgentModel.h"
#include "AgentsPanel.h"
#include "CodeView.h"
#include "DocumentView.h"
#include "EditorPage.h"
#include "EnvLock.h"
#include "Gutter.h"
#include "LineHistoryView.h"

#include <time.h>

#include <cstdlib>

using namespace gitchecks;

namespace {

std::string gRepo, gReal, gFile, gProjects;
const char* kSid = "sess-line-history";

Editor* Ed() { return RepoWindow().editor; }
Gutter* Gut() { return Ed()->codeView()->gutter(); }
LineHistoryView* History() { return RepoWindow().lineHistory; }
GtkTextBuffer* Buf() { return Ed()->buffer(); }

std::string Stamp(int ago) {
    time_t t = time(nullptr) - ago;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

// A JSON string, quoted.
std::string Q(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

std::string Head(const char* type, int ago) {
    return std::string("{\"parentUuid\":null,\"isSidechain\":false,\"type\":\"") + type +
           "\",\"timestamp\":\"" + Stamp(ago) + "\",\"cwd\":" + Q(gReal) + ",\"sessionId\":\"" +
           kSid + "\",\"version\":\"9.9.9\",\"gitBranch\":\"main\",";
}

std::string Prompt(int ago, const std::string& text) {
    return Head("user", ago) + "\"message\":{\"role\":\"user\",\"content\":" + Q(text) + "}}\n";
}

std::string EditUse(int ago, const std::string& id, const std::string& was, const std::string& now) {
    return Head("assistant", ago) + "\"message\":{\"id\":\"msg_" + id +
           "\",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
           "\",\"name\":\"Edit\",\"input\":{\"file_path\":" + Q(gReal + "/hello.py") +
           ",\"old_string\":" + Q(was) + ",\"new_string\":" + Q(now) +
           ",\"replace_all\":false}}]}}\n";
}

std::string Result(int ago, const std::string& id) {
    return Head("user", ago) + "\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":\"" + id +
           "\",\"type\":\"tool_result\",\"content\":\"The file has been updated successfully.\","
           "\"is_error\":false}]}}\n";
}

// Lines: 0 def area, 1 the second commit's return, 2-3 blank, 4-5 the
// session's perimeter, 6-7 blank, 8 def main, 9 print, 10 typed by hand.
const char* kNow = "def area(r):\n"
                   "    return 3.14159 * r * r\n"
                   "\n"
                   "\n"
                   "def perimeter(r):\n"
                   "    return 2 * 3.14159 * r\n"
                   "\n"
                   "\n"
                   "def main():\n"
                   "    print(area(2))\n"
                   "# typed by hand\n";

// The rect of line `line`'s row, once laid out and on screen.
bool RowReady(size_t line) {
    return Gut()->rectOfLine(line).size.height > 0 && MCGuiOnScreen(Gut()->widget());
}

// The pointer on a line's number, as the motion controller reports it.
void RestOn(size_t line) {
    const graphene_rect_t r = Gut()->rectOfLine(line);
    Gut()->pointerAt(Gut()->numbersRight() - 4, r.origin.y + r.size.height / 2);
}

bool BoxHas(const std::string& what) {
    return History()->popupVisible() && History()->popupText().find(what) != std::string::npos;
}

std::string Text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(Buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(Buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

bool MenuHas(GMenuModel* m, const std::string& action, const std::string& label) {
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        char *a = nullptr, *l = nullptr;
        g_menu_model_get_item_attribute(m, i, "action", "s", &a);
        g_menu_model_get_item_attribute(m, i, "label", "s", &l);
        const bool hit = a && l && action == a && label == l;
        g_free(a);
        g_free(l);
        if (hit) return true;
        GMenuModel* sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
        if (!sub) sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sub) {
            const bool found = MenuHas(sub, action, label);
            g_object_unref(sub);
            if (found) return true;
        }
    }
    return false;
}

bool SwitchOn() {
    GVariant* v = g_action_group_get_action_state(G_ACTION_GROUP(MCGuiApplication()), "togglelinehistory");
    const bool on = v && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return on;
}

std::string gHash0, gHash1;
unsigned gLooks = 0;

}  // namespace

MC_GUI_SUITE(line_history) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the line history checks are skipped"); });
        return;
    }
    gRepo = MCGuiScratch() + "/line-history";
    // The harness's world names its own author in the environment, which
    // beats `-c user.name`: the two authors are set there.
    const std::string ann = "GIT_AUTHOR_NAME='Ann Lee' GIT_AUTHOR_EMAIL=ann@example.com "
                            "GIT_COMMITTER_NAME='Ann Lee' GIT_COMMITTER_EMAIL=ann@example.com ";
    const std::string bo = "GIT_AUTHOR_NAME='Bo Chen' GIT_AUTHOR_EMAIL=bo@example.com "
                           "GIT_COMMITTER_NAME='Bo Chen' GIT_COMMITTER_EMAIL=bo@example.com ";
    MCGuiStep("the settings: the language servers off", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    Shell("a repository: hello.py in two commits by two authors",
          "set -e; mkdir -p '" + gRepo + "'; cd '" + gRepo + "'; " + G() +
              " init -q -b main; printf 'def area(r):\\n    return 3 * r * r\\n\\n\\ndef main():\\n"
              "    print(area(2))\\n' > hello.py; " + G() + " add -A; GIT_AUTHOR_DATE=2026-09-01T10:00:00 "
              "GIT_COMMITTER_DATE=2026-09-01T10:00:00 " + ann + G() + " commit -qm 'First version'; "
              "printf 'def area(r):\\n    return 3.14159 * r * r\\n\\n\\ndef main():\\n"
              "    print(area(2))\\n' > hello.py; " + G() + " add -A; GIT_AUTHOR_DATE=2026-10-01T10:00:00 "
              "GIT_COMMITTER_DATE=2026-10-01T10:00:00 " + bo + G() + " commit -qm 'Better pi'");
    MCGuiStep("the transcript of a session that added perimeter, in a scratch projects folder", [] {
        char* real = realpath(gRepo.c_str(), nullptr);
        gReal = real ? real : gRepo;
        free(real);
        gFile = gRepo + "/hello.py";
        gProjects = MCGuiScratch() + "/line-history-claude/projects";
        const std::string t =
            Prompt(600, "Now add a perimeter function\nand nothing else yet") +
            EditUse(598, "e1", "def main():\n",
                    "def perimeter(r):\n    return 2 * 3.14159 * r\n\n\ndef main():\n") +
            Result(597, "e1") + Head("ai-title", 596) + "\"aiTitle\":\"Add a perimeter\"}\n";
        MCGuiWriteFile(gProjects + "/-line-history/" + kSid + ".jsonl", t);
        EnvLock::envSet("MUTIBLE_CLAUDE_PROJECTS", gProjects.c_str(), TRUE);
        EnvLock::envSet("MUTIBLE_CLAUDE_TMP", (MCGuiScratch() + "/line-history-claude/tmp").c_str(), TRUE);
        MCGuiWriteFile(gFile, kNow);   // the session's edit and a line typed by hand
        if (LineHistoryView::inlineOn()) LineHistoryView::setInline(false);
    });
    OpenRepoWindow(gRepo, false);
    MCGuiStep("hello.py shown", [] {
        GCHECK(History() != nullptr);
        GCHECK(Gut()->onHoverLine != nullptr);
        MCGuiOpenFileKept(gFile);
    });
    MCGuiWait("it is on show, its rows laid out", 15, [] {
        return Ed()->currentPath() == gFile && Text() == kNow && RowReady(10);
    });

    // ------------------------------------------------------------- the box
    MCGuiStep("resting on the first line's number: not before the pointer rests", [] {
        gHash0 = GitText({"rev-parse", "--short=7", "HEAD~1"});
        gHash1 = GitText({"rev-parse", "--short=7", "HEAD"});
        while (!gHash0.empty() && gHash0.back() == '\n') gHash0.pop_back();
        while (!gHash1.empty() && gHash1.back() == '\n') gHash1.pop_back();
        GCHECK(gHash0.size() == 7 && gHash1.size() == 7);
        GCHECK(!History()->popupVisible());
        RestOn(0);
        GCHECK(!History()->popupVisible());
    });
    MCGuiWait("the box shows the first commit", 15, [] { return BoxHas("Ann Lee"); });
    MCGuiStep("its short hash, author, age and subject", [] {
        const std::string t = History()->popupText();
        MCGuiNote("box: %s", t.c_str());
        GCHECK(t.rfind(gHash0 + " \xC2\xB7 Ann Lee \xC2\xB7 ", 0) == 0);
        GCHECK(t.find(" ago (") != std::string::npos);
        GCHECK(t.find("\nFirst version\n") != std::string::npos);
        GCHECK(t.find("Click to open the commit.") != std::string::npos);
        GCHECK(History()->popupLine() == 0);
        // Moving down the gutter: the box follows at once.
        RestOn(1);
    });
    MCGuiWait("the box for the second commit's line", 5, [] {
        return History()->popupLine() == 1 && BoxHas("Bo Chen");
    });
    MCGuiStep("the second author's commit", [] {
        const std::string t = History()->popupText();
        GCHECK(t.rfind(gHash1 + " \xC2\xB7 Bo Chen \xC2\xB7 ", 0) == 0);
        GCHECK(t.find("\nBetter pi\n") != std::string::npos);
        RestOn(9);
    });
    MCGuiWait("a line of the first commit again", 5, [] {
        return History()->popupLine() == 9 && BoxHas("Ann Lee") && BoxHas("First version");
    });
    MCGuiStep("a line typed by hand", [] { RestOn(10); });
    MCGuiWait("is not committed", 5, [] {
        return History()->popupLine() == 10 && History()->popupText().rfind("Not committed\n", 0) == 0;
    });
    MCGuiStep("the pointer leaves the gutter", [] { Gut()->pointerAt(-1, -1); });
    MCGuiWait("the box closes", 5, [] { return !History()->popupVisible(); });

    // ------------------------------------------------------------ a session
    MCGuiWait("the session is indexed by the Agents model", 15, [] {
        AgentModel* m = History()->modelInUse();
        if (!m) return false;
        std::shared_ptr<const AgentModelData::Snapshot> snap = m->snapshot();
        if (!snap) return false;
        for (const AgentModelData::Work& w : snap->sessions)
            if (w.title == "Add a perimeter") return true;
        return false;
    });
    MCGuiStep("resting on the session's line number: the Agents panel was never made", [] {
        GCHECK(RepoWindow().agents == nullptr);
        RestOn(4);
    });
    MCGuiWait("the box shows the session", 15, [] { return BoxHas("Add a perimeter"); });
    MCGuiStep("its title, the prompt the edit answered, and when", [] {
        const std::string t = History()->popupText();
        MCGuiNote("box: %s", t.c_str());
        GCHECK(t.rfind("Written by a Claude Code session, not committed\n", 0) == 0);
        GCHECK(t.find("\nPrompt: Now add a perimeter function\n") != std::string::npos);
        GCHECK(t.find("\nEdited ") != std::string::npos && t.find(" minutes ago (") != std::string::npos);
        // No page of what a session changed on Linux to click to.
        GCHECK(t.find("Click to") == std::string::npos);
        RestOn(1);   // a committed line is its commit's, not the session's
    });
    MCGuiWait("the commit's box again", 5, [] { return History()->popupLine() == 1 && BoxHas("Bo Chen"); });
    MCGuiStep("leaves", [] { Gut()->pointerAt(-1, -1); });
    MCGuiWait("closed", 5, [] { return !History()->popupVisible(); });

    // ------------------------------------------------------- beside every line
    MCGuiStep("View > Show Line History has no key and is in the menu", [] {
        GCHECK(!SwitchOn() && !LineHistoryView::inlineOn());
        GCHECK(MenuHas(gtk_application_get_menubar(MCGuiApplication()), "app.togglelinehistory",
                       "Show Line History"));
        MCGuiFocusKeyWindow();
        gtk_widget_grab_focus(Ed()->textView());
        gLooks = History()->looks();
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("the palette opens", 10, [] { return RepoWindow().palette && RepoWindow().palette->isOpen(); });
    MCGuiStep("it lists the command", [] { MCGuiType("Show Line History"); });
    MCGuiWait("one row, under View", 10, [] {
        const std::vector<std::string> l = RepoWindow().palette->labels();
        return l.size() == 1 && l[0] == "View > Show Line History";
    });
    MCGuiStep("Return runs it, and it has no key", [] {
        GCHECK_EQ_STR(RepoWindow().palette->keys().empty() ? std::string() : RepoWindow().palette->keys()[0],
                      std::string());
        MCGuiKey("return");
    });
    MCGuiWait("the words are on", 10, [] { return SwitchOn() && LineHistoryView::inlineOn(); });
    MCGuiStep("the switch is kept for the next launch", [] {
        gchar* raw = nullptr;
        gsize len = 0;
        const bool read = g_file_get_contents(LineHistoryView::statePath().c_str(), &raw, &len, nullptr);
        const std::string kept = read ? std::string(raw, len) : std::string();
        g_free(raw);
        GCHECK_EQ_STR(kept, std::string("{\"inline\":true}\n"));
        // Read again, as a new launch would: still on.
        LineHistoryView::forgetSwitchForTests();
        GCHECK(LineHistoryView::inlineOn());
    });
    MCGuiWait("the words are drawn beside the lines", 15, [] {
        return History()->labelRectForLine(0).size.width > 0 && History()->labelRectForLine(1).size.width > 0 &&
               History()->labelRectForLine(10).size.width > 0;
    });
    MCGuiStep("a commit, a session, or not committed, in words", [] {
        LineHistoryView* h = History();
        const std::string a = h->labelForLine(0), b = h->labelForLine(1), s = h->labelForLine(4);
        MCGuiNote("line 0: %s | line 1: %s | line 4: %s", a.c_str(), b.c_str(), s.c_str());
        const std::string tail = " \xC2\xB7 First version";
        GCHECK(a.rfind(gHash0 + " Ann Lee, ", 0) == 0 && a.size() > tail.size() &&
               a.compare(a.size() - tail.size(), tail.size(), tail) == 0);
        GCHECK(b.rfind(gHash1 + " Bo Chen, ", 0) == 0 && b.find("\xC2\xB7 Better pi") != std::string::npos);
        GCHECK(s.rfind("Claude Code: Add a perimeter, ", 0) == 0 && s.find(" minutes ago") != std::string::npos);
        GCHECK(h->labelForLine(5) == s);
        GCHECK_EQ_STR(h->labelForLine(10), std::string("Not committed"));
        GCHECK_EQ_STR(h->labelForLine(11), std::string());   // past the last line
        // After the line's text, on its row.
        const graphene_rect_t r = h->labelRectForLine(1);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(Buf(), &it, 1);
        gtk_text_iter_forward_to_line_end(&it);
        GdkRectangle end;
        gtk_text_view_get_iter_location(GTK_TEXT_VIEW(Ed()->textView()), &it, &end);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(Ed()->textView()), GTK_TEXT_WINDOW_WIDGET,
                                              end.x + end.width, end.y, &wx, &wy);
        GCHECK(r.origin.x > wx && r.origin.y <= wy + 2 && r.origin.y + r.size.height >= wy);
        MCGuiSnapshot(Ed()->textView(), "line-history-inline");
    });
    MCGuiStep("a click on a commit's words opens its page", [] {
        const graphene_rect_t r = History()->labelRectForLine(0);
        MCGuiClick(Ed()->textView(), r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
    });
    MCGuiWait("the commit's page is on show", 10, [] {
        Document* d = Ed()->active();
        return d && d->editorPage() && d->editorPage()->key().rfind("commit:", 0) == 0;
    });
    MCGuiStep("hello.py again", [] { MCGuiOpenFileKept(gFile); });
    MCGuiWait("its words at once, from the kept answer or a look", 15, [] {
        return Ed()->currentPath() == gFile && History()->labelRectForLine(10).size.width > 0 &&
               History()->labelForLine(10) == "Not committed";
    });
    MCGuiStep("a click on the session's words says there is no page", [] {
        const graphene_rect_t r = History()->labelRectForLine(4);
        GCHECK(r.size.width > 0);
        MCGuiClick(Ed()->textView(), r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
    });
    MCGuiWait("the status bar says so", 10, [] {
        return std::string(gtk_label_get_text(GTK_LABEL(RepoWindow().statusLabel))).find("no page") !=
               std::string::npos;
    });
    MCGuiStep("the pointer moves onto the second commit's words", [] {
        const graphene_rect_t r = History()->labelRectForLine(1);
        GCHECK(r.size.width > 0);
        MCGuiMoveTo(Ed()->textView(), r.origin.x + 4, r.origin.y + r.size.height / 2);
    });
    MCGuiWait("the box for the second commit's words", 10, [] {
        return History()->popupLine() == 1 && BoxHas("Bo Chen");
    });
    MCGuiStep("the pointer leaves the words, for the status bar", [] {
        MCGuiMoveTo(RepoWindow().statusLabel);
    });
    MCGuiWait("the box closes", 10, [] { return !History()->popupVisible(); });

    // ------------------------------------------------------------- an edit
    MCGuiStep("a line typed at the top: the words below wait for the next look", [] {
        gLooks = History()->looks();
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(Buf(), &it);
        gtk_text_buffer_place_cursor(Buf(), &it);
        gtk_widget_grab_focus(Ed()->textView());
        MCGuiType("import math\n");
    });
    MCGuiWait("typed", 10, [] { return Text().rfind("import math\ndef area", 0) == 0; });
    MCGuiStep("no words from the edited line on", [] {
        GCHECK(History()->labelForLine(1).empty() && History()->labelForLine(2).empty());
    });
    MCGuiWait("looked again after the pause", 15, [] {
        return History()->looks() > gLooks && !History()->labelForLine(1).empty();
    });
    MCGuiStep("each line's words on its new line; the new line not committed", [] {
        LineHistoryView* h = History();
        GCHECK_EQ_STR(h->labelForLine(0), std::string("Not committed"));
        GCHECK(h->labelForLine(1).rfind(gHash0 + " Ann Lee, ", 0) == 0);
        GCHECK(h->labelForLine(2).rfind(gHash1 + " Bo Chen, ", 0) == 0);
        GCHECK(h->labelForLine(5).rfind("Claude Code: Add a perimeter, ", 0) == 0);
        GCHECK_EQ_STR(h->labelForLine(11), std::string("Not committed"));
        // The line taken out again.
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(Buf(), &a);
        gtk_text_buffer_get_iter_at_line(Buf(), &b, 1);
        gtk_text_buffer_delete(Buf(), &a, &b);
    });
    MCGuiWait("as it was", 10, [] { return Text() == kNow; });

    // ---------------------------------------------------- turned off again
    MCGuiStep("the switch pressed again takes the words away", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "togglelinehistory", nullptr);
    });
    MCGuiWait("off, and nothing drawn", 10, [] {
        return !SwitchOn() && !LineHistoryView::inlineOn() &&
               History()->labelRectForLine(0).size.width == 0;
    });
    MCGuiStep("and kept off", [] {
        LineHistoryView::forgetSwitchForTests();
        GCHECK(!LineHistoryView::inlineOn());
    });
    MCGuiStep("the box still comes on a rest", [] {
        RestOn(0);
    });
    MCGuiWait("for the gutter's hover", 15, [] { return BoxHas("Ann Lee"); });
    MCGuiStep("the environment put back", [] {
        Gut()->pointerAt(-1, -1);
        EnvLock::envUnset("MUTIBLE_CLAUDE_PROJECTS");
        EnvLock::envUnset("MUTIBLE_CLAUDE_TMP");
    });
    CloseRepoWindow();
}
