// ConnectWindow.cpp — see ConnectWindow.h.
#include "ConnectWindow.h"

namespace {
std::vector<std::shared_ptr<ConnectWindow>>& list() {
    static std::vector<std::shared_ptr<ConnectWindow>> l;
    return l;
}
}  // namespace

const std::vector<std::shared_ptr<ConnectWindow>>& ConnectWindow::pending() { return list(); }

static unsigned gShown = 0;
unsigned ConnectWindow::shownCount() { return gShown; }

std::shared_ptr<ConnectWindow> ConnectWindow::show(const std::shared_ptr<SessionLink::Link>& link,
                                                   GtkWindow* parent, std::function<void()> cancelled) {
    std::shared_ptr<ConnectWindow> w(new ConnectWindow());
    w->link_ = link;
    w->cancelled_ = std::move(cancelled);
    w->pid_ = link->commandPid();
    w->title_ = Shared::sshLabel(link->target()) + " — mutible";

    w->window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w->window_), w->title_.c_str());
    gtk_window_set_resizable(GTK_WINDOW(w->window_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(w->window_), 420, -1);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(w->window_), parent);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    w->spinner_ = gtk_spinner_new();
    gtk_spinner_set_spinning(GTK_SPINNER(w->spinner_), TRUE);
    gtk_box_append(GTK_BOX(row), w->spinner_);
    w->label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->label_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->label_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(w->label_, TRUE);
    gtk_box_append(GTK_BOX(row), w->label_);
    gtk_box_append(GTK_BOX(box), row);
    GtkWidget* addr = gtk_label_new(link->address().c_str());
    gtk_label_set_xalign(GTK_LABEL(addr), 0);
    gtk_label_set_ellipsize(GTK_LABEL(addr), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(addr, "dim-label");
    gtk_widget_add_css_class(addr, "caption");
    gtk_widget_set_margin_start(addr, 26);
    gtk_box_append(GTK_BOX(box), addr);
    w->cancelButton_ = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(w->cancelButton_, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(w->cancelButton_, "Stop connecting: ssh is ended");
    gtk_box_append(GTK_BOX(box), w->cancelButton_);
    gtk_window_set_child(GTK_WINDOW(w->window_), box);

    // The list holds the window's reference; the signals hold the raw
    // pointer and go with the window, which close() destroys.
    ConnectWindow* raw = w.get();
    g_signal_connect(w->cancelButton_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<ConnectWindow*>(p)->cancel();
    }), raw);
    g_signal_connect(w->window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer p) -> gboolean {
        static_cast<ConnectWindow*>(p)->cancel();
        return TRUE;   // cancel() closed it already
    }), raw);
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType,
                                                          gpointer p) -> gboolean {
        if (key != GDK_KEY_Escape) return FALSE;
        static_cast<ConnectWindow*>(p)->cancel();
        return TRUE;
    }), raw);
    gtk_widget_add_controller(w->window_, keys);

    gShown++;
    list().push_back(w);
    link->onProgress([raw]() { raw->update(); });
    w->update();
    gtk_window_present(GTK_WINDOW(w->window_));
    return w;
}

ConnectWindow::~ConnectWindow() {
    if (link_) link_->onProgress(nullptr);
}

void ConnectWindow::update() {
    if (over_ || !link_) return;
    progress_ = link_->progress();
    const std::string now = progress_.line(link_->target());
    if (now == line_) return;
    line_ = now;
    lines_.push_back(now);
    gtk_label_set_text(GTK_LABEL(label_), now.c_str());
}

void ConnectWindow::close() {
    if (over_) return;
    over_ = true;
    if (link_) link_->onProgress(nullptr);
    std::shared_ptr<ConnectWindow> keep = shared_from_this();   // the list may hold the last reference
    auto& l = list();
    for (auto it = l.begin(); it != l.end(); ++it)
        if (*it == keep) {
            l.erase(it);
            break;
        }
    GtkWidget* win = window_;
    window_ = nullptr;
    gtk_window_destroy(GTK_WINDOW(win));
}

void ConnectWindow::end() {
    if (over_) return;
    update();
    close();
}

void ConnectWindow::cancel() {
    if (over_) return;
    if (!link_ || !link_->connecting()) return;   // connected or failed already
    link_->cancelConnecting();   // ssh's group killed by its pid
    update();
    std::function<void()> f = std::move(cancelled_);
    close();
    if (f) f();
}
