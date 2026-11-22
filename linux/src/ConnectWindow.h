// ConnectWindow.h — a window over SSH connecting (stage 8; the Mac's
// MCRemoteConnect; docs/notes/session-host.md, "The GTK port over SSH",
// "The connecting window"): a small window shown while ssh connects and
// mutible starts there, for the first connect of `mutible ssh://host/path`
// (not for Reconnect or the tries by themselves: the status bar says so).
//   - Its line says how far it got (Shared::ConnectProgress, updated by the
//     link's progress listener): "Connecting to host…", "Starting mutible
//     on host…" (once `mutible host --stdio` said it started, on ssh's
//     standard error), "Opening <folder> on host…" (the Welcome came).
//   - Cancel (the button, Esc, or the close button) kills ssh's process
//     group (Link::cancelConnecting) and closes the window; the link's
//     `done` is never called, so `cancelled` is told instead.
//   - end() closes it (connected, or the failure's alert is about to show).
// Main thread. Not modal; presented once, never again.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SessionLink.h"

class ConnectWindow : public std::enable_shared_from_this<ConnectWindow> {
public:
    // Shows the window for `link` (connecting); `parent` (may be null) is
    // the window it is a child of. `cancelled` is called once when Cancel
    // ended the try.
    static std::shared_ptr<ConnectWindow> show(const std::shared_ptr<SessionLink::Link>& link, GtkWindow* parent,
                                               std::function<void()> cancelled);
    // Every connecting window now, oldest first (for the checks).
    static const std::vector<std::shared_ptr<ConnectWindow>>& pending();
    // How many have been shown since the app started (checks: a try by itself shows none).
    static unsigned shownCount();

    ~ConnectWindow();
    // The connect succeeded or failed: the window closes (Opening is the
    // last line it said), `cancelled` is not called.
    void end();
    // The same as the Cancel button.
    void cancel();

    GtkWidget* window() const { return window_; }
    bool over() const { return over_; }
    // What the line says now, and every line it has said.
    const std::string& line() const { return line_; }
    const std::vector<std::string>& linesShown() const { return lines_; }
    // How far it got (a stage may pass between two looks: passed() says so).
    Shared::ConnectProgress progress() const { return progress_; }
    // ssh's pid (0 when none).
    pid_t commandPid() const { return pid_; }
    const std::string& title() const { return title_; }

private:
    ConnectWindow() = default;
    void update();
    void close();

    std::shared_ptr<SessionLink::Link> link_;
    GtkWidget* window_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* spinner_ = nullptr;
    GtkWidget* cancelButton_ = nullptr;
    std::function<void()> cancelled_;
    std::string line_, title_;
    std::vector<std::string> lines_;
    Shared::ConnectProgress progress_;
    pid_t pid_ = 0;
    bool over_ = false;
};
