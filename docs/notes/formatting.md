# Formatting and install hints

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Formatting (Mac)

- Shift+Option+F pipes the buffer through an external formatter.
  `format.<python|cpp|go|rust|shell|markdown|json|yaml|css|latex>` (`{file}`,
  `off`) runs as written. Else
  the nearest folder's config (up to the open folder or a `.git`) decides:
  `[tool.pyink]`, `.style.yapf`, `[yapf]` in setup.cfg, `[tool.black]`,
  ruff.toml, .ruff.toml or any `[tool.ruff]` (lint settings count, but pyink,
  yapf or black in the same folder win), `.clang-format`, rustfmt.toml, an
  `.editorconfig` shell section; for .js .mjs .cjs .jsx .ts .mts .cts .tsx,
  before clang-format, Prettier (`.prettierrc*`, `prettier.config.*`,
  "prettier" in package.json, or Prettier as a dependency or in
  node_modules/.bin, since it needs no config file;
  `prettier --stdin-filepath {file}`) or Biome
  (biome.json[c]; `biome format --stdin-file-path={file}`), from
  `node_modules/.bin` first (`Formatter::nodeBinDirs`). That tool runs without
  style flags, and when it is missing the user is told to install it, since
  another would restyle the project. Without config, Google's defaults:
  `clang-format --style=file --fallback-style=Google`, pyink (`--line-length 80
  --pyink-use-majority-quotes`, Google's published 4-space guide) then yapf
  `--style=google`, gofmt, rustfmt with Cargo.toml's edition, `shfmt -i 2 -ci
  -bn`, `latexindent -g /dev/null` (with `-l={config}`, the project's
  `.latexindent.yaml` or `localSettings.yaml` by path, since the text comes
  on stdin). Markdown, JSON, YAML and CSS (scss, less) have no default: only
  the project's Prettier (Biome for .json, .jsonc, .css) or a format.<lang>
  command formats them, so their plan is often empty.
- **The server fallback** (2026-11-07, `Lsp+Formatting.mm`): only when
  nothing is planned at all (`Formatter::serverMayFormat`: no formatter for
  the extension, or an empty plan with no setting), the first running
  server offering `documentFormattingProvider` (`Lsp::routeFormatting`,
  main first, rangeFormatting for a selection where offered) formats it
  through the workspace-edit applier as one "Format Document" step; 5 s
  limit. Never for a language with a default (clangd, pylsp and Ruff format
  in other styles), a configured project tool that is missing, `off`, or a
  settings command. format.on-save covers such files too.
- `MCFormatText`: `MCFindProgram` (never xcrun shims), venv bins first for
  Python; stdin to stdout in the file's folder, wide PATH, `PYTHONUTF8=1`, 10 s
  then SIGTERM and SIGKILL. A non-zero exit, empty output or non-UTF-8 leaves
  the buffer alone with stderr's first 6 lines in a popover. Paths are planned
  resolved; a result for changed text is dropped; a selection passes `--lines`
  (clang-format, yapf) or `--line-ranges` (pyink, black).
- **A formatter in the folder's own .venv or node_modules needs the user's
  yes** (round 4, D9): the same per-folder answer as the servers'
  (`MCFolderToolsConsent`/`MCSetFolderToolsConsent` in Lsp.h, over
  `MCPythonEnvironmentConsent`), decided by `Formatter::chooseProgram`.
  Unanswered, the result says `needsConsent` and the editor asks (Allow,
  Don't Allow, Cancel, a sheet; Cmd+S too) and formats again. Deny looks
  outside the folder, never at another formatter. A `format.<lang>` command
  never asks while the tool exists outside the folder (the owner's
  `format.python = ruff ...`); `$VIRTUAL_ENV`'s bin is trusted; a project copy
  outside the window's folder is never used. **A configuration that runs
  code counts as the folder's program** (2026-11-07,
  `Formatter::codeConfigFile`, `configRunsCode`): Prettier's JS and TS
  configs, a JSON/YAML/TOML one or package.json key naming plugins, a parser
  by path or a shared config module, Biome's plugins or a package's
  `extends`, latexindent's `substitution` (Perl `eval`, which
  `switchesViaYaml` can turn on). Then any copy of the tool, the user's own
  command included, asks first, and Deny runs nothing (`Source::Refused`).
  Prettier's and Biome's files are looked for up to `/`, past the open
  folder and .git, as the tools look (2026-11-08: a repository with only
  docs/ open ran its prettier.config.js unasked). A package runner
  (`Formatter::launched`: npx, pnpx, bunx, `bun x`, `pnpm exec`/`dlx`, `npm
  exec`, yarn) is the program it launches: that tool's configuration
  counts, and with the folder's own node_modules/.bin copy (or a
  package.json script, or yarn's bins) the runner is the folder's program
  (`chooseLaunched`).
  `format.<lang> = prettier` alone (markdown, json, yaml, css, JS and TS
  files) is the opt-in for the user's own Prettier: the project's config
  where there is one, else `--prose-wrap preserve --config-precedence
  prefer-file`.
- `format.on-save`: Cmd+S (non-nil sender) formats then saves; mutible's own
  saves and Save All never format. A Cmd+S is never lost (`_formatSaveTab`:
  leaving the tab saves it unformatted). **Three values** (2026-11-16,
  JOBS C2): false, true (whatever Format Document runs, Google's style
  included) and `project` (`Formatter::projectChose`: only a formatter the
  project's configuration chose or format.<lang> names; never the Google
  default or a server; `MCFormatText`'s and `FormatRun::decide`'s
  `projectOnly`, so the GTK port follows). Edit > Format on Save
  (`FormatOnSave.{h,mm}`, `mutible.toggleFormatOnSave`) is checked for
  either on value and writes false or `project`. **The offer**: a Cmd+S
  that wrote the file, with format.on-save off, asks
  `MCFindFormatOnSaveOffer` off the main thread (`Formatter::offersOnSave`:
  the project's own configuration, not the settings, chose a formatter, and
  it is installed) and shows an `MCInstallBar` under the install bar
  (`MCFormatOnSaveOffer`, one per window, `_formatOnSaveOffer` in
  +FormatIvars; laid out by `layoutFormatOnSaveOfferIn:`): Turn On writes
  `project`, Don't Ask Again mutes `format-on-save` in the install bar's
  list, ×. Once per launch across windows; nothing is formatted by the
  answer itself (`make guitest SUITES=format_on_save`).
- `editor.organize-imports-on-save` (2026-11-06): before the formatter, Cmd+S
  runs `LspSession organizeImportsBeforeSave:` (the servers'
  `source.organizeImports`, Ruff's first, the all-or-nothing workspace edit),
  through `organizeOrFormatBeforeSaving:` with the same `_formatSaveTab`
  marker; its `done` comes once, after the edit, nothing, a failure, or 3 s
  (askCodeActions' limit; 5 at most). Never on auto-save (nil sender).

## Over SSH (stage 9's third slice, the Mac; 2026-11-22)

A window over SSH (`mutible ssh://host/path`) formats on the other machine:
its host plans and runs the formatter there (core `src/SessionFormat.{h,cpp}`,
FsRead "format", Welcome flag 1024; the protocol and the host's side are in
docs/notes/session-host.md, "Formatters over SSH").

- **The seam** (`formatSourceQuietly:projectOnly:then:`): with a
  `remoteWorkspace` the request goes to `MCRemoteWorkspace
  formatText:path:setting:consent:...` instead of `MCFormatText`, and comes
  back as the same `MCFormatResult`, so everything after it (the consent
  sheet, the server fallback, the note, `replaceSourceWithText:`, format on
  save's `finishFormatSave`) is unchanged. The window sends the tab's text,
  its remote path, format.<lang> as the settings say it, the lines of a
  selection, `projectOnly`, the 10 s limit, and the folder's answer from
  `MCFolderToolsConsent([self folderToolsKey])`.
- **On the host**: `FormatRun::decide` (the GTK port's decisions, the same
  words as `MCFormatText`) with `hostFormatEnv`: that machine's disk for the
  project's files, `serverSearchDirs` and `findServerProgram` (SessionLsp.h)
  for the program, its own $VIRTUAL_ENV and home. Run by `runProgram`
  (`ServerProcess`: posix_spawn, a session of its own, no shell, no other
  descriptor) with the text on stdin, in the file's folder (inside the
  session's folder, else the folder), PATH the program's folder first,
  `PYTHONUTF8=1`, `PWD` set; `FormatRun::outcome` says what its exit means.
- **The formatter table is the allow list**: a format.<lang> command runs
  only when its program, or what a package runner launches, is one of
  `Formatter::knownPrograms()` (`formatSettingRefusal`, reason "setting":
  "format.shell runs tr, which the other machine does not run for a
  window: ..."). The file must be inside the session's folder (links
  resolved); a reader and a guest are refused.
- **Consent per host and folder**: `folderToolsKey` is the link's key
  (ssh://machine and folder, as `LspSession consentRoot`), for the request
  and for the sheet's answer (`askToRunFolderFormatter:`), so it never
  applies to a folder of this Mac's with the same path; the host's
  NeedsConsent answer carries the tool, `consentFolder` and
  `consentConfig` as the Mac's own would.
- **What differs**: a missing formatter (`FormatRun::Decision::missing`)
  says so with the machine named (`Shared::formatMissingText`) and no
  `installPrograms` (no install view: this Mac's commands would not put it
  there); a host too old says `formatTooOldText`, a lost one
  `formatLostText`; the window stops waiting 30 s past the limit. Format on
  Save's offer (`offerFormatOnSaveAfterSaving`) is not made in a remote
  window (it would read this Mac's disk). The server fallback needs nothing
  new: the window's servers are the host's already.
- **For GTK** (not done): `linux/src/FormatRunner.cpp` runs
  `FormatRun::decide` and the process on this computer. A GTK window over
  SSH needs the same seam: send `formatRequestJson` through
  `Client::requestFormat` on its link, read `formatReplyFrom`, key consent
  by the link's key, and drop the install commands for a missing one.
- **Checks**: `hostformat:request`, `:setting`, `:run` (a scripted shfmt:
  the text uppercased, run in the file's folder with Google's arguments; a
  setting run as written, one outside the table refused with nothing run,
  one not installed "missing"; a complaint, a cancel and a timeout; a text
  file `unavailable`; a .venv black waiting for consent, then run; outside
  the folder and through a link out refused; a new file), `:wire` (through
  the encoding, a guest and a reader refused, an older host sent nothing).
  GUI: remote_ssh's formatter block (RemoteSshSuite.mm's header).

## Install hints (Mac)

- One table, `Install::server` / `Install::formatter` (Python's first way is
  `uv tool install basedpyright`, which brings its own Node; Homebrew's node
  broke pyright here). `suggest` picks the first way whose installer
  `MCInstallerAvailable` finds, else adds `brew install uv` or a link;
  `$MUTIBLE_INSTALL_HAVE` fakes installers. `install:readme` ties it to
  `docs/REFERENCE.md`.
- `MCInstallBar` under the strip when a wanted server is `NotInstalled`, once
  per language per launch (`MCInstallBarTakeOffer`), until ×, Don't Show Again
  (`MCInstallHintsMuted`), or the server appearing. `MCInstallView` (command,
  Copy, Run in Terminal, alternatives) is in the status item's popover, the
  servers card and Format Document's note. Run in Terminal only types
  (`typeCommand:`). Copy uses `MCInstallPasteboard()` (`$MUTIBLE_PASTEBOARD`).
  `lookAgainForMissingServer` runs on window key and
  `MCTerminalCommandDidFinishNotification`, so no restart is needed.

## Traps

- Over SSH: **the stand-in's folder is this Mac's**, so a formatter run
  here on the window's path (MCFormatText) would pass remote_ssh with the
  routing broken; the check counts the formats asked of the host
  (`MCRemoteWorkspace formats`) and has the formatter print the stand-in's
  variable, which only the host's environment has.
