// LineHistoryView.h — Show Line History on Linux: who last changed each line
// of the file on show, a commit or a Claude Code session. The Mac's
// MCLineHistory (mac/src/EditorLineHistory.mm, A7); the words are the
// core's (LineHistory.h: `git blame --porcelain --contents -` with the
// buffer's text on stdin, so the lines are the buffer's, unsaved edits
// included; a session's uncommitted edit found exactly in the file; or
// "Not committed").
//
// Quiet by default: the pointer resting on a line's number, dot or change
// bar in the gutter (Gutter::onHoverLine) shows that line's history in a
// small box beside it after half a second, and following the pointer down
// the gutter once it shows. A click in the box opens the commit on its page
// (the one a commit in the Source Control graph opens). View > Show Line
// History (no key) puts the same words beside every line, muted, in every
// window until it is turned off or mutible quits (the Mac keeps the switch
// in its defaults; here it lasts the session, as Word Wrap does); a click on
// them opens the commit, and the pointer resting on them shows the box.
//
// Not here, unlike the Mac: the page of what a session changed in a file
// (the Agents panel has none on Linux), so a click on a session's words
// says so in the status bar; and in a window over SSH Claude Code's
// sessions, which are this machine's transcripts, never the host's: blame
// runs on the host (GitRun.h's runner) and the rest of a line is "Not
// committed".
//
// Work is off the main loop, on a thread of its own and only while wanted
// (the words shown, or the box asked for): git and the transcripts there,
// the text copied once a look. A look starts 0.6 s after typing stops, at
// once for another file; until it ends the lines from the first edit on
// show nothing rather than another line's words. The last answer of each of
// eight files is kept with its text and shown again at once when the tab
// comes back unchanged, while git and the Agents model have not moved. The
// Source Control panel refreshing, the host noticing git moved, and the
// Agents model publishing look again while the words are wanted.
//
// Claude Code's transcripts are only read, from the Agents model's folder
// ($MUTIBLE_CLAUDE_PROJECTS, else ~/.claude/projects, AgentModel.h): the
// window's Agents model once its panel has been shown, else a model of the
// view's own, made the first time the box or the words ask for one.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "GitRun.h"
#include "LineHistory.h"

class AgentModel;
class Editor;
class Gutter;

class LineHistoryView {
public:
    explicit LineHistoryView(Editor* editor);
    ~LineHistoryView();
    LineHistoryView(const LineHistoryView&) = delete;
    LineHistoryView& operator=(const LineHistoryView&) = delete;

    // The words beside every line: one switch for every window, kept for
    // the next launch in statePath() as the Mac keeps it in its defaults.
    // setInline tells every window's view.
    static bool inlineOn();
    static void setInline(bool on);
    // $XDG_STATE_HOME/mutible/line-history.json.
    static std::string statePath();
    // Read the file again at the next inlineOn(), as a new launch would.
    static void forgetSwitchForTests();

    // What the window gives it.
    // The window's Agents model while its Agents panel exists, else null (a
    // model of its own is made on first use, so a hover alone never makes the
    // panel); never used over SSH. The window's folder, for that model.
    std::function<AgentModel*()> agentModel;
    std::function<std::string()> folder;
    // A click: the commit on its page.
    std::function<void(const std::string& hash, const std::string& subject)> onShowCommit;
    // What it says in the status bar.
    std::function<void(const std::string& text)> onNote;

    // A window over SSH: blame runs on the host's git through `runner`.
    void setHost(GitRunnerPtr runner);
    // git or the Agents model moved: look again while wanted.
    void refreshSoon();

    // The gutter's hover: the box after the pointer rests, kNoLine to close
    // it. Lines are the editor's, 0-based.
    void hoverLine(size_t line);
    // The box for a line now (looking first when the answer is not in yet).
    void showPopupForLine(size_t line);
    void closePopup();
    bool popupVisible() const;
    const std::string& popupText() const { return popupText_; }
    size_t popupLine() const { return popupLine_; }
    GtkWidget* popover() const { return popover_; }

    // A line's words beside it and in the box; "" while not known (no look
    // yet, or edited since).
    std::string labelForLine(size_t line) const;
    std::string detailForLine(size_t line) const;
    // What a click on a line's words does; false (with a note) for a line
    // not committed and from no session, or a session's.
    bool openLine(size_t line);
    // Where a line's words are drawn, in the text view's widget
    // coordinates; empty while not shown.
    graphene_rect_t labelRectForLine(size_t line) const;
    // A click at a point of the text view's widget coordinates: what the
    // click on its words does (the editor never sees it); false when no
    // words are there.
    bool pressedAt(double x, double y);
    // The pointer resting at a point of the text view's widget coordinates.
    void pointerOverText(double x, double y);

    // For the checks.
    unsigned looks() const { return looks_; }              // looks finished
    const std::string& lookedPath() const { return lookedPath_; }
    double lastLookMainCpuMs() const { return lastMainCpuMs_; }

    // The Agents model its sessions come from (made if need be).
    AgentModel* modelInUse() { return model(); }

    struct Entry;   // a file's kept answer

private:
    struct Look;
    bool wanted() const;
    AgentModel* model();
    std::string pathToLook() const;
    void attachBuffer();
    void detachBuffer();
    void edited(size_t line);
    void scheduleAfter(int ms);
    void look();
    bool takeKept();
    void keep(const std::string& path, const std::string& text,
              std::shared_ptr<const LineHistory::Answer> answer, unsigned gitGen, unsigned modelGen);
    void post(std::function<void()> job);
    void inlineChanged();
    void redraw();
    void drawLabels(GtkSnapshot* s, const GdkRectangle& visible);
    // The answer's line for an editor line, -1 when there is none for it now.
    long answerLine(size_t line) const;
    void showBox(size_t line, const graphene_rect_t& anchor, bool fromLabel, bool afterLook = false);
    void makePopover();
    graphene_rect_t gutterLineRect(size_t line) const;
    size_t labelLineAt(double x, double y) const;
    void popupLeft();
    Gutter* gutter() const;
    static void onViewBuffer(GObject*, GParamSpec*, gpointer self);
    static void onUnrealize(GtkWidget*, gpointer self);

    Editor* editor_ = nullptr;
    GtkWidget* view_ = nullptr;
    GThreadPool* pool_ = nullptr;
    std::shared_ptr<bool> alive_;
    GitRunnerPtr runner_;
    int pass_ = -1;
    std::unique_ptr<AgentModel> ownModel_;   // when the window has no Agents panel

    GtkTextBuffer* buffer_ = nullptr;      // the view's, held while listened to
    guint timer_ = 0, hoverTimer_ = 0, closeTimer_ = 0;

    // The answer on show, the file and buffer it was for, and the first
    // line edited since (lines from it on wait for the next look).
    std::shared_ptr<const LineHistory::Answer> answer_;
    std::string answerPath_;
    GtkTextBuffer* answerBuffer_ = nullptr;   // compared, never followed
    size_t validBefore_ = static_cast<size_t>(-1);
    size_t editedSince_ = static_cast<size_t>(-1);
    bool looking_ = false, lookAgain_ = false;
    unsigned lookNumber_ = 0, gitGeneration_ = 0;
    unsigned looks_ = 0;
    std::string lookedPath_;
    double lastMainCpuMs_ = 0;

    std::vector<std::unique_ptr<Entry>> kept_;   // oldest first

    // The words drawn beside the lines on screen: line, rect in buffer coordinates.
    struct Drawn {
        size_t line;
        GdkRectangle box;
    };
    std::vector<Drawn> drawn_;

    // The box.
    GtkWidget* popover_ = nullptr;
    GtkWidget* popoverLabel_ = nullptr;
    size_t popupLine_ = static_cast<size_t>(-1);
    size_t wantPopupLine_ = static_cast<size_t>(-1);
    graphene_rect_t wantAnchor_{};
    bool wantFromLabel_ = false;
    bool pointerInPopup_ = false;
    bool popupFromLabel_ = false;
    std::string popupText_;
    size_t hoveringLine_ = static_cast<size_t>(-1);   // the gutter's or the words'

    // Claude Code's calls on a file, kept while its transcript is unchanged
    // (the worker's alone).
    struct Collected {
        unsigned long long size = 0;
        double mtime = 0;
        std::vector<AgentEdits::Change> changes;
    };
    std::shared_ptr<std::map<std::string, Collected>> collected_;
};
