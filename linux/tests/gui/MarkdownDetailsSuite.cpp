// MarkdownDetailsSuite.cpp — <details> folded in the preview
// (`./build/mutible-guitest markdown_details`): a closed block shows its
// summary with a closed triangle and hides its body, an open one shows both
// with an open triangle, a click on a summary opens or closes its block, and
// the raw tags never show.
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"

#include <memory>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

GtkTextBuffer* shown() { return gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView())); }

std::string shownText() {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(shown(), &s, &e);
    char* raw = gtk_text_buffer_get_text(shown(), &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

bool has(const char* word) { return shownText().find(word) != std::string::npos; }

// Where `word`'s first occurrence is, in the view's window: x and y of its
// second character (the span a click must reach), the line's height, the
// scroll and the view's height.
struct Where {
    int wx = 0, wy = 0, height = 0, scroll = 0, viewHeight = 0;
    bool found = false;
    bool operator==(const Where& o) const {
        return found == o.found && wx == o.wx && wy == o.wy && height == o.height &&
               scroll == o.scroll && viewHeight == o.viewHeight;
    }
};

Where whereIs(const char* word) {
    Where w;
    const std::string t = shownText();
    const size_t at = t.find(word);
    if (at == std::string::npos) return w;
    GtkTextView* view = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(shown(), &it, (int)g_utf8_strlen(t.c_str(), (gssize)at) + 1);
    GdkRectangle box;
    gtk_text_view_get_iter_location(view, &it, &box);
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, box.x, box.y, &w.wx, &w.wy);
    w.height = box.height;
    w.scroll = (int)gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view)));
    w.viewHeight = gtk_widget_get_height(GTK_WIDGET(view));
    w.found = true;
    return w;
}

// Waits until `word` is where a click will reach it. A page set just now has
// its lines' heights estimated until GTK has validated them in idle slices,
// so the spot read from the layout moves under a click made too early (a
// loaded machine takes longer to validate); the same spot at three polls in
// a row, inside the view, is a laid out one.
void waitPlaced(const char* word) {
    auto last = std::make_shared<Where>();
    auto same = std::make_shared<int>(0);
    MCGuiWait(std::string("\"") + word + "\" laid out and inside the view", 10, [=] {
        const Where w = whereIs(word);
        *same = (w.found && w == *last) ? *same + 1 : 0;
        *last = w;
        return *same >= 3 && w.wy >= 0 && w.wy + w.height <= w.viewHeight;
    });
}

// A click on the first character of `word`'s first occurrence. The spot is
// in the log, so a click that toggles nothing says where it went.
void clickOn(const char* word) {
    const Where w = whereIs(word);
    GCHECK(w.found);
    if (!w.found) return;
    MCGuiNote("click on \"%s\" at %d,%d (line %d high, scrolled %d, view %d high)", word, w.wx,
              w.wy, w.height, w.scroll, w.viewHeight);
    MCGuiClick(ed()->textView(), w.wx + 2, w.wy + w.height / 2);
}

// GTK's double-click time and a tenth of a second more, in seconds.
double doubleClickSeconds() {
    int ms = 400;
    if (GtkSettings* st = gtk_settings_get_default()) g_object_get(st, "gtk-double-click-time", &ms, nullptr);
    return ms / 1000.0 + 0.1;
}

}  // namespace

MC_GUI_SUITE(markdown_details) {
    MCGuiStep("a closed and an open details block", [] {
        MCGuiWriteFile(F("details.md"),
                       "# Page\n\n<details>\n<summary>Closed head</summary>\n\nHidden body alpha\n\n"
                       "</details>\n\n<details open>\n<summary>Open head</summary>\n\n"
                       "Shown body beta\n\n</details>\n\nAfter gamma\n");
        MCGuiOpenFileKept(F("details.md"));
    });
    MCGuiWait("the file open", 10, [] { return ed()->active() && ed()->active()->path() == F("details.md"); });
    MCGuiStep("shown as a preview", [] {
        if (!ed()->inPreview()) ed()->togglePreview();
    });
    MCGuiWait("the preview", 10, [] { return has("After gamma"); });
    MCGuiStep("the closed block shows its summary only, the open one both", [] {
        MCGuiNote("text: %s", shownText().c_str());
        GCHECK(has("\xE2\x96\xB8 Closed head"));
        GCHECK(!has("Hidden body alpha"));
        GCHECK(has("\xE2\x96\xBE Open head"));
        GCHECK(has("Shown body beta"));
        GCHECK(!has("<details") && !has("<summary") && !has("</details"));
    });
    waitPlaced("Closed head");
    MCGuiStep("a click on the closed summary", [] { clickOn("Closed head"); });
    MCGuiWait("its body shows", 10, [] { return has("Hidden body alpha"); });
    MCGuiStep("the triangle is open, the tags still hidden", [] {
        GCHECK(has("\xE2\x96\xBE Closed head"));
        GCHECK(has("Shown body beta"));
        GCHECK(!has("<details") && !has("<summary") && !has("</details"));
    });
    // Time is the condition: a second press on the same spot within GTK's
    // double-click time is a double-click, which a link on the page leaves
    // alone (Editor::onPressed acts on n == 1), so the check waits it out.
    MCGuiPause(doubleClickSeconds());
    waitPlaced("Closed head");
    MCGuiStep("a click again", [] { clickOn("Closed head"); });
    MCGuiWait("its body hides", 10, [] { return !has("Hidden body alpha"); });
    MCGuiStep("closed again", [] { GCHECK(has("\xE2\x96\xB8 Closed head")); });
    waitPlaced("Open head");
    MCGuiStep("a click on the open block", [] { clickOn("Open head"); });
    MCGuiWait("the open block's body hides", 10, [] { return !has("Shown body beta"); });
    MCGuiStep("the rest is untouched", [] {
        GCHECK(has("\xE2\x96\xB8 Open head") && has("After gamma"));
        ed()->closeAllTabs();
    });
}
