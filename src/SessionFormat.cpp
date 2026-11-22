// SessionFormat.cpp — see SessionFormat.h.
#include "SessionFormat.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Formatter.h"
#include "SessionLsp.h"

namespace Shared {

namespace {

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point t) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

std::string envValue(const std::vector<std::string>& env, const std::string& name) {
    const std::string prefix = name + "=";
    for (const std::string& e : env)
        if (e.compare(0, prefix.size(), prefix) == 0) return e.substr(prefix.size());
    return std::string();
}

std::string homeOf(const std::vector<std::string>& env) {
    const std::string h = envValue(env, "HOME");
    if (!h.empty()) return h;
    struct passwd pw, *found = nullptr;
    char buf[4096];
    if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_dir) return found->pw_dir;
    return std::string();
}

std::string realOf(const std::string& p) {
    char buf[PATH_MAX];
    return !p.empty() && ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

std::string parentOf(const std::string& p) {
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool runnable(const std::string& p) {
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); i++) {
        if (i) out += i + 1 == names.size() ? (names.size() > 2 ? ", and " : " and ") : ", ";
        out += names[i];
    }
    return out;
}

const char* kindName(FormatRun::Kind k) {
    switch (k) {
    case FormatRun::Kind::Run: return "run";
    case FormatRun::Kind::Unavailable: return "unavailable";
    case FormatRun::Kind::NeedsConsent: return "consent";
    case FormatRun::Kind::Failed: return "failed";
    }
    return "failed";
}

}  // namespace

// ---------------------------------------------------------------- the request

Json formatRequestJson(const FormatRequest& r) {
    Json j = Json::object();
    j.set("op", Json("format"));
    j.set("path", Json(r.path));
    if (!r.setting.empty()) j.set("setting", Json(r.setting));
    if (!r.consent.empty()) j.set("consent", Json(r.consent));
    if (r.firstLine > 0 && r.lastLine >= r.firstLine) {
        j.set("first", Json((std::int64_t)r.firstLine));
        j.set("last", Json((std::int64_t)r.lastLine));
    }
    if (r.projectOnly) j.set("projectOnly", Json(true));
    if (r.timeout > 0) j.set("timeout", Json(r.timeout));
    return j;
}

FormatRequest formatRequestFrom(const Json& j) {
    FormatRequest r;
    r.path = j["path"].asString();
    r.setting = j["setting"].asString();
    r.consent = j["consent"].asString();
    if (r.consent != "allow" && r.consent != "deny") r.consent.clear();
    const std::int64_t first = j["first"].asInt(0), last = j["last"].asInt(0);
    if (first > 0 && last >= first && last < INT_MAX) {
        r.firstLine = (int)first;
        r.lastLine = (int)last;
    }
    r.projectOnly = j["projectOnly"].asBool();
    r.timeout = j["timeout"].asNumber(10);
    if (!(r.timeout > 0)) r.timeout = 10;
    if (r.timeout > kMaxFormatSeconds) r.timeout = kMaxFormatSeconds;
    return r;
}

std::string formatSettingRefusal(const std::string& lang, const std::string& setting) {
    if (setting.empty() || setting == "off") return "";
    const std::vector<std::string> words = Formatter::arguments(setting, "/x");
    if (words.empty()) return "";
    const Formatter::Launch launch = Formatter::launched(setting);
    const std::string name = Formatter::programName(launch.tool.empty() ? words[0] : launch.tool);
    if (Formatter::isKnownProgram(name)) return "";
    return "format." + (lang.empty() ? std::string("<language>") : lang) + " runs " + name +
           ", which the other machine does not run for a window: over SSH it runs the formatters mutible "
           "knows (" + joinNames(Formatter::knownPrograms()) + "), with that machine's own configuration.";
}

// ---------------------------------------------------------------- the host's disk

FormatRun::Env hostFormatEnv(const std::vector<std::string>& env) {
    FormatRun::Env e;
    e.fs.exists = [](const std::string& p) {
        struct stat st;
        return ::stat(p.c_str(), &st) == 0;
    };
    e.fs.read = [](const std::string& p) {
        // A project file: a few kilobytes; nothing past 4 MB is read.
        std::ifstream in(p, std::ios::binary);
        if (!in) return std::string();
        std::string out;
        char buf[65536];
        while (in && out.size() < (std::size_t(4) << 20)) {
            in.read(buf, sizeof buf);
            out.append(buf, (std::size_t)in.gcount());
        }
        return out;
    };
    e.executable = runnable;
    const std::string home = homeOf(env);
    const std::vector<std::string> dirs = serverSearchDirs(envValue(env, "PATH"), home, realServerFs());
    e.find = [dirs, home](const std::string& word) -> std::string {
        return findServerProgram(word, dirs, home, realServerFs());
    };
    e.realPath = [](const std::string& p) {
        const std::string r = realOf(p);
        return r.empty() ? p : r;
    };
    e.virtualEnv = envValue(env, "VIRTUAL_ENV");
    e.home = home;
    e.askAgainMenu = "mutible";
    return e;
}

// ---------------------------------------------------------------- running it

ProgramRun runProgram(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                      const std::string& cwd, const std::string& input, int timeoutMs,
                      const std::atomic<bool>* cancel) {
    ProgramRun r;
#ifndef __APPLE__
    // A formatter that stops reading is EPIPE on this thread, never a
    // signal to the host (the Mac's pipe has F_SETNOSIGPIPE instead); one
    // left pending is taken off at the end.
    sigset_t pipeSet, before;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &before);
    struct Restore {
        sigset_t pipe, before;
        ~Restore() {
            struct timespec zero = {0, 0};
            while (sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE) {
            }
            pthread_sigmask(SIG_SETMASK, &before, nullptr);
        }
    } restore{pipeSet, before};
#endif
    ServerProcess proc;
    std::string why;
    if (!proc.start(argv, env, cwd, &why)) {
        r.launchError = "could not be started (" + why + ")";
        return r;
    }
    proc.write(input);
    proc.closeInput();
    const Clock::time_point started = Clock::now();
    Clock::time_point termedAt{}, exitedAt{};
    bool termed = false, killed = false, exited = false, errOpen = proc.errFd() >= 0;
    int status = -1;
    char buf[65536];
    for (;;) {
        if (!termed) {
            const bool stop = cancel && cancel->load();
            if (stop || msSince(started) >= timeoutMs || r.out.size() > kMaxFormatOutput) {
                proc.signal(SIGTERM);
                termed = true;
                termedAt = Clock::now();
                if (stop) r.cancelled = true;
                else r.timedOut = true;
            }
        }
        if (termed && !killed && msSince(termedAt) >= 1000) {
            proc.signal(SIGKILL);
            killed = true;
        }
        if (!exited && proc.reap(&status)) {
            exited = true;
            exitedAt = Clock::now();
        }
        if (exited && proc.outEnded() && !errOpen) break;
        // Exited, but something it started holds its output open.
        if (exited && msSince(exitedAt) > 2000) {
            if (!termed) r.launchError = "left its output open after exiting";
            break;
        }
        if (killed && msSince(termedAt) > 4000) break;
        pollfd p[3];
        nfds_t n = 0;
        int outAt = -1, errAt = -1, inAt = -1;
        if (!proc.outEnded()) {
            outAt = (int)n;
            p[n++] = pollfd{proc.outFd(), POLLIN, 0};
        }
        if (errOpen) {
            errAt = (int)n;
            p[n++] = pollfd{proc.errFd(), POLLIN, 0};
        }
        if (proc.wantsWrite()) {
            inAt = (int)n;
            p[n++] = pollfd{proc.inFd(), POLLOUT, 0};
        }
        const int got = n ? ::poll(p, n, 50) : (::usleep(10000), 0);
        if (got < 0 && errno != EINTR) break;
        if (got <= 0) continue;
        if (inAt >= 0 && p[inAt].revents) proc.flush();
        if (outAt >= 0 && p[outAt].revents) proc.readOut(r.out);
        if (errAt >= 0 && p[errAt].revents) {
            const ssize_t m = ::read(proc.errFd(), buf, sizeof buf);
            if (m > 0) {
                if (r.err.size() < kMaxFormatErr) r.err.append(buf, std::min((std::size_t)m, kMaxFormatErr - r.err.size()));
            } else if (m == 0 || (errno != EAGAIN && errno != EINTR)) {
                errOpen = false;
            }
        }
    }
    if (exited) {
        if (WIFEXITED(status)) r.status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) r.status = 128 + WTERMSIG(status);
    }
    return r;   // the process's destructor kills and reaps anything left
}

// ---------------------------------------------------------------- the host's answer

FormatAnswer formatRefused(const std::string& why, const std::string& reason) {
    FormatAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("format"));
    a.json.set("error", Json(why));
    a.json.set("reason", Json(reason));
    return a;
}

FormatAnswer runFormatRequest(const std::string& folder, const Json& request, const std::string& input,
                              const std::vector<std::string>& env, const std::atomic<bool>* cancel) {
    const FormatRequest req = formatRequestFrom(request);
    if (req.path.empty() || req.path[0] != '/' || req.path.find('\0') != std::string::npos)
        return formatRefused("A format request names its file's whole path.", "args");
    if (input.size() > kMaxFormatInput) return formatRefused("That is more text than a formatter is sent.", "args");
    const std::string root = realOf(folder);
    if (root.empty()) return formatRefused("The session's folder is not there.", "outside");
    // The file's folder by its real place (the file itself may be new), held
    // inside the session's folder.
    std::string name = req.path.substr(req.path.rfind('/') + 1);
    const std::string dir = realOf(parentOf(req.path));
    if (name.empty() || name == "." || name == ".." || dir.empty() || !Formatter::insideFolder(dir, root))
        return formatRefused("The other machine formats files in the window's folder only, not " + req.path + ".",
                             "outside");
    const std::string path = (dir == "/" ? std::string() : dir) + "/" + name;
    const std::string lang = Formatter::languageFor(path);
    const std::string settingWhy = formatSettingRefusal(lang, req.setting);
    if (!settingWhy.empty()) return formatRefused(settingWhy, "setting");

    const FormatRun::Env fenv = hostFormatEnv(env);
    const FormatRun::Decision d =
        FormatRun::decide(path, root, req.setting, req.consent, req.firstLine, req.lastLine, fenv, req.projectOnly);
    FormatAnswer a;
    a.json = Json::object();
    a.json.set("op", Json("format"));
    a.json.set("kind", Json(kindName(d.kind)));
    if (!d.tool.empty()) a.json.set("tool", Json(d.tool));
    if (!d.style.empty()) a.json.set("style", Json(d.style));
    if (d.kind != FormatRun::Kind::Run) {
        a.json.set("message", Json(d.message));
        if (d.serverMayFormat) a.json.set("serverMayFormat", Json(true));
        if (d.missing) a.json.set("missing", Json(true));
        if (!d.consentFolder.empty()) a.json.set("consentFolder", Json(d.consentFolder));
        if (!d.consentConfig.empty()) a.json.set("consentConfig", Json(d.consentConfig));
        return a;
    }
    // Planned from the table, or a setting naming one of its programs.
    if (req.setting.empty() && !Formatter::isKnownProgram(Formatter::programName(d.argv[0])))
        return formatRefused("The other machine runs only the formatters mutible knows.", "setting");
    std::string cwd = d.cwd;
    const std::string realCwd = realOf(cwd);
    if (realCwd.empty() || !isDir(realCwd) || !Formatter::insideFolder(realCwd, root)) cwd = root;
    // Its environment: the host's, its program's folder first on PATH (a
    // script's #!/usr/bin/env node finds the node beside it), then the
    // usual folders; UTF-8 for Python; PWD where it runs.
    std::string pathVar = parentOf(d.argv[0]);
    for (const std::string& s : serverSearchDirs(envValue(env, "PATH"), fenv.home, realServerFs()))
        if (s != pathVar) pathVar += ":" + s;
    std::vector<std::string> penv;
    for (const std::string& e : env)
        if (e.compare(0, 5, "PATH=") != 0 && e.compare(0, 11, "PYTHONUTF8=") != 0 && e.compare(0, 4, "PWD=") != 0)
            penv.push_back(e);
    penv.push_back("PATH=" + pathVar);
    penv.push_back("PYTHONUTF8=1");
    penv.push_back("PWD=" + cwd);
    const ProgramRun run = runProgram(d.argv, penv, cwd, input, (int)(req.timeout * 1000), cancel);
    std::string launchError = run.launchError;
    if (run.cancelled) launchError = "was stopped";
    const FormatRun::Outcome o = FormatRun::outcome(d.tool, path, run.timedOut, req.timeout, launchError,
                                                    run.status, run.out, run.err, input.size());
    a.json.set("ok", Json(o.ok));
    if (d.ranged) a.json.set("ranged", Json(true));
    if (!o.ok) a.json.set("message", Json(o.message));
    else a.out = o.text;
    return a;
}

// ---------------------------------------------------------------- the renderer's side

FormatReply formatReplyFrom(const Json& answer, std::string&& out) {
    FormatReply r;
    if (answer["op"].asString() != "format" || !answer["error"].isNull()) {
        r.kind = "error";
        r.message = answer["error"].isNull() ? std::string("The other machine did not format the file.")
                                             : answer["error"].asString();
        r.reason = answer["reason"].asString();
        return r;
    }
    r.kind = answer["kind"].asString();
    if (r.kind != "run" && r.kind != "unavailable" && r.kind != "consent" && r.kind != "failed") r.kind = "failed";
    r.ok = r.kind == "run" && answer["ok"].asBool();
    r.message = answer["message"].asString();
    r.tool = answer["tool"].asString();
    r.style = answer["style"].asString();
    r.ranged = answer["ranged"].asBool();
    r.serverMayFormat = answer["serverMayFormat"].asBool();
    r.missing = answer["missing"].asBool();
    r.consentFolder = answer["consentFolder"].asString();
    r.consentConfig = answer["consentConfig"].asString();
    if (r.ok) r.text = std::move(out);
    if (!r.ok && r.kind == "run" && r.message.empty()) r.message = r.tool + " did not format the file.";
    return r;
}

std::string formatTooOldText(const std::string& host) {
    return "Format Document needs a newer mutible on " + host +
           ": the one there cannot run formatters for this window. Upgrade it there, then Reconnect.";
}

std::string formatLostText(const std::string& host) {
    return "The connection to " + host + " is lost: Reconnect, then format again.";
}

std::string formatMissingText(const std::string& message, const std::string& host) {
    return message + " This window's formatters run on " + host + ": install one there, then format again.";
}

}  // namespace Shared
