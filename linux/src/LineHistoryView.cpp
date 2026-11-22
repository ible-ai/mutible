// LineHistoryView.cpp — see LineHistoryView.h. The Mac's
// mac/src/EditorLineHistory.mm, over the core's LineHistory.h.
#include "LineHistoryView.h"

#include "AgentModel.h"
#include "AgentTranscript.h"
#include "CodeView.h"
#include "DocumentView.h"
#include "Editor.h"
#include "Gutter.h"
#include "Json.h"

#include <time.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr size_t kNone = static_cast<size_t>(-1);
constexpr int kHoverDelayMs = 500;
constexpr int kTypingPauseMs = 600;
constexpr size_t kKeptFiles = 8;
constexpr double kBlameLimit = 30;

bool gInline = false;
bool gInlineLoaded = false;

// The switch as the last session left it ($XDG_STATE_HOME/mutible/
// line-history.json), read once, as the Mac keeps it in its defaults.
void loadInline() {
    if (gInlineLoaded) return;
    gInlineLoaded = true;
    gchar* raw = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(LineHistoryView::statePath().c_str(), &raw, &len, nullptr)) return;
    Json j;
    const bool ok = Json::parse(std::string(raw, len), j);
    g_free(raw);
    if (ok && j.isObject()) gInline = j["inline"].asBool();
}

void saveInline() {
    const std::string path = LineHistoryView::statePath();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    Json j = Json::object();
    j.set("inline", Json(gInline));
    const std::string text = j.dump() + "\n";
    // A temporary file renamed over the old one, so a crash leaves either.
    g_file_set_contents(path.c_str(), text.c_str(), (gssize)text.size(), nullptr);
}

std::vector<LineHistoryView*>& views() {
    static std::vector<LineHistoryView*> v;
    return v;
}

double threadCpuMs() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// The date as the box writes it: "Nov 22, 2026 at 14:02" (the Mac's medium
// date and short time), in the local time zone.
std::string dateText(long long t) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tt, &tm)) return std::string();
    char buf[64];
    strftime(buf, sizeof buf, "%b %e, %Y at %H:%M", &tm);
    std::string s = buf;
    const size_t two = s.find("  ");   // %e pads a day below 10
    if (two != std::string::npos) s.erase(two, 1);
    return s;
}

long long nowSeconds() { return (long long)time(nullptr); }

std::string dirOf(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : slash == 0 ? "/" : p.substr(0, slash);
}

std::string baseOf(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

void deliver(std::shared_ptr<bool> alive, std::function<void()> fn) {
    struct Delivery {
        std::shared_ptr<bool> alive;
        std::function<void()> fn;
    };
    auto* d = new Delivery{std::move(alive), std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn();
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void runJob(gpointer data, gpointer) {
    auto* job = static_cast<std::function<void()>*>(data);
    (*job)();
    delete job;
}

}  // namespace

// A file's last answer and the text it was for.
struct LineHistoryView::Entry {
    std::string path, text;
    std::shared_ptr<const LineHistory::Answer> answer;
    unsigned gitGeneration = 0, modelGeneration = 0;
};

// A candidate session: where its transcript is, what it is called, and the
// file's name as its calls gave it.
struct Candidate {
    std::string transcript, title, file;
};

// ==================================================================== life
std::string LineHistoryView::statePath() {
    return std::string(g_get_user_state_dir()) + "/mutible/line-history.json";
}

void LineHistoryView::forgetSwitchForTests() { gInlineLoaded = false; gInline = false; }

bool LineHistoryView::inlineOn() {
    loadInline();
    return gInline;
}

void LineHistoryView::setInline(bool on) {
    loadInline();
    if (gInline == on) return;
    gInline = on;
    saveInline();
    for (LineHistoryView* v : views()) v->inlineChanged();
}

LineHistoryView::LineHistoryView(Editor* editor) : editor_(editor), view_(editor->textView()) {
    alive_ = std::make_shared<bool>(true);
    collected_ = std::make_shared<std::map<std::string, Collected>>();
    // One thread: git and the transcripts, one look at a time, in order.
    pool_ = g_thread_pool_new(runJob, nullptr, 1, FALSE, nullptr);
    views().push_back(this);
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onViewBuffer), this);
    // The popover is the view's child only by gtk_widget_set_parent, which
    // GtkTextView's own dispose does not know about (Lsp.cpp's trap).
    g_signal_connect(view_, "unrealize", G_CALLBACK(onUnrealize), this);
    if (Gutter* g = gutter()) g->onHoverLine = [this](size_t line) { hoverLine(line); };
    pass_ = editor_->codeView()->addPass(CodeView::Layer::AboveText, 25,
        [this](GtkSnapshot* s, const GdkRectangle& visible) { drawLabels(s, visible); });
    // A press on the words opens what they say: in the capture phase, so the
    // text view's own selection gesture never sees it.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int, double x, double y,
                                                      gpointer p) {
        auto* self = static_cast<LineHistoryView*>(p);
        if (self->pressedAt(x, y))
            gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
    }), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
    // The pointer resting on the words shows the box.
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y,
                                                      gpointer p) {
        static_cast<LineHistoryView*>(p)->pointerOverText(x, y);
    }), this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
        static_cast<LineHistoryView*>(p)->pointerOverText(-1, -1);
    }), this);
    gtk_widget_add_controller(view_, motion);
    attachBuffer();
}

LineHistoryView::~LineHistoryView() {
    *alive_ = false;
    views().erase(std::remove(views().begin(), views().end(), this), views().end());
    for (guint* t : {&timer_, &hoverTimer_, &closeTimer_})
        if (*t) g_source_remove(*t);
    timer_ = hoverTimer_ = closeTimer_ = 0;
    if (popover_) {
        if (gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
        gtk_widget_unparent(popover_);
    }
    popover_ = nullptr;
    detachBuffer();
    if (ownModel_) ownModel_->onChange = nullptr;
    ownModel_.reset();
    g_signal_handlers_disconnect_by_data(view_, this);
    GListModel* ctrls = gtk_widget_observe_controllers(view_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    if (Gutter* g = gutter()) g->onHoverLine = nullptr;
    if (pass_ >= 0) editor_->codeView()->removePass(pass_);
    // A look already running finishes on the pool's thread; what it delivers
    // finds alive_ false and is dropped. The pool frees itself after.
    g_thread_pool_free(pool_, FALSE, FALSE);
}

Gutter* LineHistoryView::gutter() const {
    return editor_->codeView() ? editor_->codeView()->gutter() : nullptr;
}

void LineHistoryView::setHost(GitRunnerPtr runner) {
    runner_ = std::move(runner);
    kept_.clear();
    answer_.reset();
    refreshSoon();
}

void LineHistoryView::post(std::function<void()> job) {
    if (runner_) job = [runner = runner_, job = std::move(job)] {
        GitRunScope scope(runner);
        job();
    };
    g_thread_pool_push(pool_, new std::function<void()>(std::move(job)), nullptr);
}

void LineHistoryView::onViewBuffer(GObject*, GParamSpec*, gpointer self) {
    static_cast<LineHistoryView*>(self)->attachBuffer();
}

void LineHistoryView::onUnrealize(GtkWidget*, gpointer selfp) {
    auto* self = static_cast<LineHistoryView*>(selfp);
    if (self->popover_) {
        if (gtk_widget_get_visible(self->popover_))
            gtk_popover_popdown(GTK_POPOVER(self->popover_));
        gtk_widget_unparent(self->popover_);
    }
    self->popover_ = nullptr;
    self->popoverLabel_ = nullptr;
}

// ============================================================ the buffer
void LineHistoryView::attachBuffer() {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (b == buffer_) return;
    detachBuffer();
    closePopup();
    if (b) {
        buffer_ = GTK_TEXT_BUFFER(g_object_ref(b));
        // The first line an edit touches: lines before it keep their words.
        g_signal_connect(b, "insert-text", G_CALLBACK(+[](GtkTextBuffer*, GtkTextIter* at, char*, int,
                                                          gpointer self) {
            static_cast<LineHistoryView*>(self)->edited((size_t)gtk_text_iter_get_line(at));
        }), this);
        g_signal_connect(b, "delete-range", G_CALLBACK(+[](GtkTextBuffer*, GtkTextIter* from,
                                                           GtkTextIter*, gpointer self) {
            static_cast<LineHistoryView*>(self)->edited((size_t)gtk_text_iter_get_line(from));
        }), this);
    }
    // Another file in the view: its kept answer, or a look.
    if (wanted() && !takeKept()) scheduleAfter(50);
    else redraw();
}

void LineHistoryView::detachBuffer() {
    if (!buffer_) return;
    g_signal_handlers_disconnect_by_data(buffer_, this);
    g_object_unref(buffer_);
    buffer_ = nullptr;
}

void LineHistoryView::edited(size_t line) {
    validBefore_ = std::min(validBefore_, line);
    if (looking_) editedSince_ = std::min(editedSince_, line);
    if (popupVisible() && answerLine(popupLine_) < 0) closePopup();
    redraw();
    if (wanted()) scheduleAfter(kTypingPauseMs);
}

AgentModel* LineHistoryView::model() {
    if (runner_) return nullptr;   // the transcripts are this machine's, the file the host's
    if (AgentModel* m = agentModel ? agentModel() : nullptr) {
        if (ownModel_) {
            ownModel_->onChange = nullptr;
            ownModel_.reset();
        }
        return m;
    }
    if (!ownModel_ && folder) {
        ownModel_ = std::make_unique<AgentModel>(folder());
        ownModel_->onChange = [this] { refreshSoon(); };
    }
    return ownModel_.get();
}

bool LineHistoryView::wanted() const {
    return inlineOn() || wantPopupLine_ != kNone || popupVisible();
}

// The file to look at now: the file on show while its editable source is.
std::string LineHistoryView::pathToLook() const {
    if (!runner_ && gitExecutable().empty()) return std::string();
    Document* d = editor_->active();
    if (!d || !editor_->isSource() || d->path().empty() || d->loading() ||
        d->largeFileFeaturesOff() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return std::string();
    return d->path();
}

void LineHistoryView::inlineChanged() {
    if (inlineOn()) {
        if (!takeKept()) scheduleAfter(50);
    } else if (!wanted()) {
        answer_.reset();
    }
    redraw();
}

void LineHistoryView::refreshSoon() {
    gitGeneration_++;
    if (wanted()) scheduleAfter(50);
}

void LineHistoryView::scheduleAfter(int ms) {
    if (timer_) g_source_remove(timer_);
    timer_ = g_timeout_add(ms, [](gpointer p) -> gboolean {
        auto* self = static_cast<LineHistoryView*>(p);
        self->timer_ = 0;
        if (self->wanted()) self->look();
        return G_SOURCE_REMOVE;
    }, this);
}

void LineHistoryView::redraw() { editor_->codeView()->queueDraw(); }

// ==================================================================== kept
bool LineHistoryView::takeKept() {
    const std::string path = pathToLook();
    if (path.empty()) return false;
    AgentModel* m = model();
    const unsigned mg = m && m->snapshot() ? m->snapshot()->generation : 0;
    const std::string now = editor_->text();
    for (const auto& e : kept_)
        if (e->path == path && e->gitGeneration == gitGeneration_ && e->modelGeneration == mg &&
            e->text == now) {
            answer_ = e->answer;
            answerPath_ = path;
            answerBuffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
            validBefore_ = kNone;
            redraw();
            return true;
        }
    return false;
}

void LineHistoryView::keep(const std::string& path, const std::string& text,
                           std::shared_ptr<const LineHistory::Answer> answer, unsigned gitGen,
                           unsigned modelGen) {
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(),
                               [&](const std::unique_ptr<Entry>& e) { return e->path == path; }),
                kept_.end());
    auto e = std::make_unique<Entry>();
    e->path = path;
    e->text = text;
    e->answer = std::move(answer);
    e->gitGeneration = gitGen;
    e->modelGeneration = modelGen;
    kept_.push_back(std::move(e));
    while (kept_.size() > kKeptFiles) kept_.erase(kept_.begin());
}

// ===================================================================== look
// On the main loop: what to ask; on the worker: blame of the buffer's text
// and the sessions' edits; back on the main loop, the answer, if the file
// is still on show.
void LineHistoryView::look() {
    if (looking_) {
        lookAgain_ = true;
        return;
    }
    const std::string path = pathToLook();
    if (path.empty()) {
        answer_.reset();
        answerPath_.clear();
        redraw();
        return;
    }
    const double c0 = threadCpuMs();
    // The sessions that wrote or edited a file of this name; the worker
    // checks the paths properly. Over SSH none: the transcripts are this
    // machine's, the file is the host's.
    std::vector<Candidate> candidates;
    unsigned mg = 0;
    {
        if (AgentModel* m = model()) {
            std::shared_ptr<const AgentModelData::Snapshot> snap = m->snapshot();
            if (snap) {
                mg = snap->generation;
                const std::string name = baseOf(path);
                std::vector<const AgentModelData::Work*> todo;
                for (const AgentModelData::Work& w : snap->sessions) todo.push_back(&w);
                while (!todo.empty()) {
                    const AgentModelData::Work* w = todo.back();
                    todo.pop_back();
                    for (const AgentModelData::Work& sub : w->subagents) todo.push_back(&sub);
                    const std::string title = !w->title.empty() ? w->title : w->name;
                    for (const AgentModelData::Group& g : w->groups)
                        for (const AgentModelData::File& f : g.files)
                            if (f.writes + f.edits > 0 && baseOf(f.path) == name)
                                candidates.push_back({w->transcript, title, f.path});
                }
            }
        }
    }
    auto text = std::make_shared<std::string>(editor_->text());
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    const unsigned number = ++lookNumber_;
    const unsigned gg = gitGeneration_;
    looking_ = true;
    editedSince_ = kNone;
    const double mainBefore = threadCpuMs() - c0;
    std::shared_ptr<bool> alive = alive_;
    auto collected = collected_;
    const bool overSsh = (bool)runner_;
    post([this, alive, path, text, candidates, number, gg, mg, buffer, collected, overSsh,
          mainBefore] {
        // On the worker: git blame of the buffer as it is, then the sessions.
        LineHistory::GitOutcome out;
        {
            GitResult r = runGitWithin(dirOf(path), LineHistory::blameArgs(baseOf(path)),
                                       kBlameLimit, text.get());
            out.ok = r.ok();
            out.timedOut = r.timedOut;
            out.status = r.status;
            out.out = std::move(r.out);
            out.err = std::move(r.err);
        }
        std::vector<LineHistory::SessionInput> sessions;
        if (!overSsh) {
            AgentTranscript::Fs fs = AgentTranscript::posixFs();
            for (const Candidate& c : candidates) {
                if (c.transcript.empty() || !AgentEdits::samePath(fs, c.file, path)) continue;
                const AgentTranscript::Stat st = fs.stat(c.transcript);
                if (!st.exists) continue;
                Collected& got = (*collected)[c.transcript + "\n" + path];
                if (got.size != st.size || got.mtime != st.mtime) {
                    got.changes = AgentEdits::collect(fs, c.transcript, c.file).changes;
                    got.size = st.size;
                    got.mtime = st.mtime;
                }
                LineHistory::SessionInput s;
                s.info.title = c.title;
                s.info.transcript = c.transcript;
                s.file = c.file;
                s.changes = got.changes;
                sessions.push_back(std::move(s));
            }
        }
        auto answer = std::make_shared<const LineHistory::Answer>(
            LineHistory::assemble(*text, out, std::move(sessions)));
        deliver(alive, [this, path, text, answer, number, gg, mg, buffer, mainBefore] {
            const double c1 = threadCpuMs();
            looking_ = false;
            if (number == lookNumber_ && pathToLook() == path &&
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_)) == buffer) {
                answer_ = answer;
                answerPath_ = path;
                answerBuffer_ = buffer;
                // Lines from the first edit since the look began wait for the next.
                validBefore_ = editedSince_;
                keep(path, *text, answer, gg, mg);
                lookedPath_ = path;
                looks_++;
                redraw();
                if (wantPopupLine_ != kNone) showBox(wantPopupLine_, wantAnchor_, wantFromLabel_, true);
                else if (popupVisible()) showBox(popupLine_, wantAnchor_, popupFromLabel_, true);
                lastMainCpuMs_ = mainBefore + (threadCpuMs() - c1);
            }
            if (lookAgain_ || editedSince_ != kNone) {
                lookAgain_ = false;
                if (wanted()) scheduleAfter(50);
            }
        });
    });
}

// ================================================================== lines
long LineHistoryView::answerLine(size_t line) const {
    if (!answer_ || line >= answer_->lines.size() || line >= validBefore_) return -1;
    if (answerBuffer_ != gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_)) ||
        answerPath_ != pathToLook())
        return -1;
    return (long)line;
}

std::string LineHistoryView::labelForLine(size_t line) const {
    const long k = answerLine(line);
    return k < 0 ? std::string() : answer_->label((size_t)k, nowSeconds());
}

std::string LineHistoryView::detailForLine(size_t line) const {
    const long k = answerLine(line);
    if (k < 0) return std::string();
    std::string text = answer_->detail((size_t)k, nowSeconds(), dateText);
    // There is no page of what a session changed on Linux to click to.
    if (answer_->lines[(size_t)k].kind == LineHistory::Kind::Session) {
        const size_t at = text.rfind("\nClick to see");
        if (at != std::string::npos) text.erase(at);
    }
    return text;
}

bool LineHistoryView::openLine(size_t line) {
    const long k = answerLine(line);
    if (k < 0) return false;
    const LineHistory::Line& l = answer_->lines[(size_t)k];
    if (l.kind == LineHistory::Kind::Commit) {
        const LineHistory::Commit& c = answer_->blame.commits[(size_t)l.commit];
        if (onShowCommit) onShowCommit(c.hash, c.summary);
        return true;
    }
    if (l.kind == LineHistory::Kind::Session) {
        if (onNote) onNote("mutible on Linux has no page of what a Claude Code session changed in a file");
        return false;
    }
    gtk_widget_error_bell(view_);
    return false;
}

// ================================================================ drawing
void LineHistoryView::drawLabels(GtkSnapshot* s, const GdkRectangle& visible) {
    drawn_.clear();
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    if (!inlineOn() || !answer_ || !gtk_text_view_get_editable(tv)) return;
    if (answerBuffer_ != gtk_text_view_get_buffer(tv) || answerPath_ != pathToLook()) return;
    GtkTextIter it;
    int ignore = 0;
    gtk_text_view_get_line_at_y(tv, &it, visible.y, &ignore);
    GdkRGBA color{0.5f, 0.5f, 0.5f, 1};
    if (Gutter* g = gutter()) color = g->numberColor();
    PangoLayout* layout = gtk_widget_create_pango_layout(view_, "");
    int charWidth = 0, lineHeight = 0;
    pango_layout_set_text(layout, "0", -1);
    pango_layout_get_pixel_size(layout, &charWidth, &lineHeight);
    for (int guard = 0; guard < 400; guard++) {
        int ly = 0, lh = 0;
        gtk_text_view_get_line_yrange(tv, &it, &ly, &lh);
        if (ly > visible.y + visible.height) break;
        const size_t line = (size_t)gtk_text_iter_get_line(&it);
        const long k = lh > 0 ? answerLine(line) : -1;
        if (k >= 0) {
            const std::string text = answer_->label((size_t)k, nowSeconds());
            if (!text.empty()) {
                // After the line's last fragment.
                GtkTextIter end = it;
                if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
                GdkRectangle at;
                gtk_text_view_get_iter_location(tv, &end, &at);
                pango_layout_set_text(layout, text.c_str(), -1);
                int tw = 0, th = 0;
                pango_layout_get_pixel_size(layout, &tw, &th);
                const int x = at.x + charWidth * 3;
                const int y = at.y + std::max(0, (at.height - th) / 2);
                GdkRGBA c = color;
                c.alpha *= 0.85f;
                gtk_snapshot_save(s);
                const graphene_point_t origin = GRAPHENE_POINT_INIT((float)x, (float)y);
                gtk_snapshot_translate(s, &origin);
                gtk_snapshot_append_layout(s, layout, &c);
                gtk_snapshot_restore(s);
                drawn_.push_back({line, GdkRectangle{x, at.y, tw, std::max(at.height, th)}});
            }
        }
        if (!gtk_text_iter_forward_line(&it)) break;
    }
    g_object_unref(layout);
}

graphene_rect_t LineHistoryView::labelRectForLine(size_t line) const {
    for (const Drawn& d : drawn_)
        if (d.line == line) {
            int wx = 0, wy = 0;
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, d.box.x,
                                                  d.box.y, &wx, &wy);
            return GRAPHENE_RECT_INIT((float)wx, (float)wy, (float)d.box.width, (float)d.box.height);
        }
    return GRAPHENE_RECT_INIT(0, 0, 0, 0);
}

size_t LineHistoryView::labelLineAt(double x, double y) const {
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET, (int)x,
                                          (int)y, &bx, &by);
    for (const Drawn& d : drawn_)
        if (bx >= d.box.x && bx < d.box.x + d.box.width && by >= d.box.y &&
            by < d.box.y + d.box.height)
            return d.line;
    return kNone;
}

bool LineHistoryView::pressedAt(double x, double y) {
    const size_t line = labelLineAt(x, y);
    if (line == kNone) return false;
    openLine(line);
    return true;
}

void LineHistoryView::pointerOverText(double x, double y) {
    const size_t line = labelLineAt(x, y);
    if (line == kNone) {
        if (popupFromLabel_ || wantFromLabel_) hoverLine(kNone);
        return;
    }
    // The box under the words, in the view's coordinates.
    const graphene_rect_t anchor = labelRectForLine(line);
    if (line == hoveringLine_ && (popupVisible() || hoverTimer_)) return;
    hoveringLine_ = line;
    wantFromLabel_ = true;
    wantAnchor_ = anchor;
    wantPopupLine_ = line;
    if (hoverTimer_) g_source_remove(hoverTimer_);
    const int delay = popupVisible() ? 0 : kHoverDelayMs;
    hoverTimer_ = g_timeout_add(delay, [](gpointer p) -> gboolean {
        auto* self = static_cast<LineHistoryView*>(p);
        self->hoverTimer_ = 0;
        if (self->wantPopupLine_ != kNone) self->showPopupForLine(self->wantPopupLine_);
        return G_SOURCE_REMOVE;
    }, this);
}

// =================================================================== the box
graphene_rect_t LineHistoryView::gutterLineRect(size_t line) const {
    Gutter* g = gutter();
    if (!g) return GRAPHENE_RECT_INIT(0, 0, 0, 0);
    const graphene_rect_t r = g->rectOfLine(line);
    graphene_point_t from = GRAPHENE_POINT_INIT(r.origin.x, r.origin.y), to;
    if (!gtk_widget_compute_point(g->widget(), view_, &from, &to)) to = from;
    return GRAPHENE_RECT_INIT(to.x, to.y, r.size.width, r.size.height);
}

bool LineHistoryView::popupVisible() const {
    return popover_ && popupLine_ != kNone && gtk_widget_get_visible(popover_);
}

void LineHistoryView::hoverLine(size_t line) {
    hoveringLine_ = line;
    if (line == kNone) {
        // The pointer left: the box goes unless the pointer went into it.
        wantPopupLine_ = kNone;
        wantFromLabel_ = false;
        if (hoverTimer_) g_source_remove(hoverTimer_);
        hoverTimer_ = 0;
        if (popupVisible()) {
            if (closeTimer_) g_source_remove(closeTimer_);
            closeTimer_ = g_timeout_add(250, [](gpointer p) -> gboolean {
                auto* self = static_cast<LineHistoryView*>(p);
                self->closeTimer_ = 0;
                if (!self->pointerInPopup_) self->closePopup();
                return G_SOURCE_REMOVE;
            }, this);
        }
        return;
    }
    if (closeTimer_) g_source_remove(closeTimer_);
    closeTimer_ = 0;
    wantFromLabel_ = false;
    wantPopupLine_ = line;
    wantAnchor_ = gutterLineRect(line);
    if (hoverTimer_) g_source_remove(hoverTimer_);
    hoverTimer_ = 0;
    // Once the box shows it follows the pointer down the gutter at once;
    // otherwise it waits for the pointer to rest.
    const int delay = popupVisible() ? 0 : kHoverDelayMs;
    hoverTimer_ = g_timeout_add(delay, [](gpointer p) -> gboolean {
        auto* self = static_cast<LineHistoryView*>(p);
        self->hoverTimer_ = 0;
        if (self->wantPopupLine_ != kNone) self->showPopupForLine(self->wantPopupLine_);
        return G_SOURCE_REMOVE;
    }, this);
}

// The box for a line now; looking first when the answer is not in yet.
void LineHistoryView::showPopupForLine(size_t line) {
    graphene_rect_t anchor = wantAnchor_;
    bool fromLabel = wantFromLabel_;
    if (wantPopupLine_ != line) {
        anchor = gutterLineRect(line);
        fromLabel = false;
    }
    wantPopupLine_ = line;
    wantAnchor_ = anchor;
    wantFromLabel_ = fromLabel;
    showBox(line, anchor, fromLabel);
}

void LineHistoryView::showBox(size_t line, const graphene_rect_t& anchor, bool fromLabel,
                              bool afterLook) {
    if (!gtk_widget_get_mapped(view_)) return;
    const std::string detail = detailForLine(line);
    if (detail.empty()) {
        // Not known yet: look, and the box shows when the look ends; after
        // a look the line has no words, and the box is not wanted.
        if (afterLook) {
            wantPopupLine_ = kNone;
            if (popupVisible()) closePopup();
        } else if (!looking_) {
            scheduleAfter(0);
        }
        return;
    }
    wantPopupLine_ = kNone;
    makePopover();
    popupLine_ = line;
    popupText_ = detail;
    popupFromLabel_ = fromLabel;
    gtk_label_set_text(GTK_LABEL(popoverLabel_), detail.c_str());
    const long k = answerLine(line);
    const bool clickable = k >= 0 && answer_->lines[(size_t)k].kind == LineHistory::Kind::Commit;
    gtk_widget_set_cursor_from_name(popover_, clickable ? "pointer" : "default");
    GdkRectangle r = {(int)anchor.origin.x, (int)anchor.origin.y,
                      std::max(1, (int)anchor.size.width), std::max(1, (int)anchor.size.height)};
    gtk_popover_set_position(GTK_POPOVER(popover_), fromLabel ? GTK_POS_BOTTOM : GTK_POS_RIGHT);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &r);
    gtk_popover_popup(GTK_POPOVER(popover_));
}

void LineHistoryView::makePopover() {
    if (popover_) return;
    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "minicode-line-history-popover");
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), FALSE);
    popoverLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(popoverLabel_), 0);
    gtk_label_set_wrap(GTK_LABEL(popoverLabel_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(popoverLabel_), 60);
    gtk_label_set_selectable(GTK_LABEL(popoverLabel_), FALSE);
    gtk_widget_set_margin_start(popoverLabel_, 6);
    gtk_widget_set_margin_end(popoverLabel_, 6);
    gtk_widget_set_margin_top(popoverLabel_, 4);
    gtk_widget_set_margin_bottom(popoverLabel_, 4);
    gtk_popover_set_child(GTK_POPOVER(popover_), popoverLabel_);
    gtk_widget_set_parent(popover_, view_);
    // A click in the box opens the commit.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double, double,
                                                      gpointer p) {
        auto* self = static_cast<LineHistoryView*>(p);
        const size_t line = self->popupLine_;
        if (line == kNone) return;
        self->openLine(line);
    }), this);
    gtk_widget_add_controller(popover_, GTK_EVENT_CONTROLLER(click));
    // The pointer in the box keeps it.
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect_swapped(motion, "enter", G_CALLBACK(+[](LineHistoryView* self) {
        self->pointerInPopup_ = true;
        if (self->closeTimer_) g_source_remove(self->closeTimer_);
        self->closeTimer_ = 0;
    }), this);
    g_signal_connect_swapped(motion, "leave", G_CALLBACK(+[](LineHistoryView* self) {
        self->pointerInPopup_ = false;
        self->popupLeft();
    }), this);
    gtk_widget_add_controller(popover_, motion);
    g_signal_connect_swapped(popover_, "closed", G_CALLBACK(+[](LineHistoryView* self) {
        self->popupLine_ = kNone;
        self->pointerInPopup_ = false;
    }), this);
}

void LineHistoryView::popupLeft() {
    if (closeTimer_) g_source_remove(closeTimer_);
    closeTimer_ = g_timeout_add(250, [](gpointer p) -> gboolean {
        auto* self = static_cast<LineHistoryView*>(p);
        self->closeTimer_ = 0;
        if (!self->pointerInPopup_ && self->hoveringLine_ == kNone) self->closePopup();
        return G_SOURCE_REMOVE;
    }, this);
}

void LineHistoryView::closePopup() {
    wantPopupLine_ = kNone;
    if (hoverTimer_) g_source_remove(hoverTimer_);
    hoverTimer_ = 0;
    if (popover_ && gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
    popupLine_ = kNone;
    popupText_.clear();
    if (!wanted()) answer_.reset();
}
