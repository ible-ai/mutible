// HostIntegration.h — the shell integration a session's host sets up
// itself, from its own machine's mutible, for a terminal a renderer on
// another machine opens (TermSpec's `integration`, stage 8: the Mac over
// SSH). The renderer's own integration is a file of its disk, which the
// remote shell cannot read; the host has the same scripts where its
// mutible is installed, and lays them over the shell it starts as the apps
// do over theirs:
//   zsh   ZDOTDIR at a folder of the host's own, whose .zshenv
//         (ShellIntegration::zshenvStub, grid mode) sources the script's
//         copy there and hands the user's ZDOTDIR back
//         (MUTIBLE_USER_ZDOTDIR when there was one); still a login shell.
//   bash  `bash --rcfile <the copy>`, which bash reads only when it is not
//         a login shell, so MUTIBLE_BASH_LOGIN=1 asks the script to read
//         what a login bash reads (/etc/profile, then the first of
//         ~/.bash_profile, ~/.bash_login and ~/.profile) in place of
//         ~/.bashrc.
// Each with the renderer's nonce in MUTIBLE_NONCE, which the marks carry
// and the renderer checks; any other shell, or one whose script this host
// does not have, runs as it would have without (the renderer is told: the
// TermOpened's name says which shell, "" none).
//
// The scripts are copied once, at the first terminal that wants one, into
// "Mutible-host-<pid>-<start>" (0700, the user's own) in TMPDIR, else /tmp
// (never XDG_RUNTIME_DIR, which systemd removes with the user's last
// session: over ssh, a dropped connection), so a shell started later never
// reads a newer install's script (ShellIntegration.h: the protocol is the
// running program's); copies the system cleared meanwhile are made again,
// the folder goes with the host, and a later host removes those of hosts
// gone (a SIGKILL). POSIX; the host's thread.
#pragma once

#include <string>
#include <vector>

#include "HostPty.h"

namespace Shared {

// Where an installed mutible keeps a file it ships: beside the program
// (a build folder), in a bundle's Resources (<dir>/../Resources: the Mac's
// mutible-cli is in Contents/MacOS), or in <dir>/../share/mutible (the
// Linux package's /usr/bin and /usr/share/mutible). `exe` is the program's
// path, its links resolved first (a /usr/local/bin/mutible that links to
// a build). "" when none of them holds a regular file of that name.
std::string shippedFile(const std::string& exe, const std::string& name);
// This process's program, links resolved; "" when it cannot be told.
std::string ownProgramPath();

// The integration laid over `p` (ownShellSpec's: argv[0] "-zsh" or
// "-bash" for a login shell) for its shell, when that shell's name is in
// `accepted` and `folder` holds its script (`zsh`, `bash` say which do):
// the shell's name ("zsh", "bash"), or "" with `p` as it was. No nonce, no
// integration. Pure.
std::string integrateShell(PtySpec& p, const std::string& folder, bool zsh, bool bash,
                           const std::vector<std::string>& accepted, const std::string& nonce);

class HostIntegration {
public:
    HostIntegration() = default;
    ~HostIntegration();   // its folder removed
    HostIntegration(const HostIntegration&) = delete;
    HostIntegration& operator=(const HostIntegration&) = delete;

    // The installed scripts (shippedFile's), "" for one there is not.
    void setScripts(std::string zsh, std::string bash);
    bool any() const { return !zshScript_.empty() || !bashScript_.empty(); }
    // Where the folder goes ("" for TMPDIR, else /tmp): checks.
    void setBase(std::string base) { base_ = std::move(base); }

    // integrateShell with this host's copies, made at the first call that
    // could use one. "" when none was laid over `p`.
    std::string apply(PtySpec& p, const std::vector<std::string>& accepted, const std::string& nonce);
    const std::string& folder() const { return folder_; }

private:
    bool prepare();
    std::string zshScript_, bashScript_, base_, folder_;
    bool tried_ = false, zsh_ = false, bash_ = false;
};

}  // namespace Shared
