# Harbor

A keyboard-first Telegram TUI built on [nchat](https://github.com/d99kris/nchat) and TDLib. Designed for a narrow terminal, with a quiet blue palette and the conversation kept close to the composer.

Harbor is a native terminal application, not a web mockup. It builds as a separate executable and keeps its own account directory, so stock nchat is preserved.

## Features

- Full-width conversations in narrow terminals; persistent chat list in wider windows.
- Type to compose, Enter to send, Shift+Enter for a newline.
- Option+K chat search, with Ctrl+K as a fallback. Forum topics appear as Group > Topic, with last-message times and pin-first ordering.
- Colored message formatting, replies, edits, reactions, and message actions.
- Per-chat drafts and reading positions; automatic initial history loading.
- Click to position the composer cursor, including wrapped text and attachment captions.
- Drag to select message text; releasing copies it automatically.
- Inline photos and video thumbnails, with a terminal color fallback.
- In-app voice playback, external video opening, and attachment previews with captions.
- A restart prompt when a newer Harbor release is installed locally.

## Prebuilt runtime for Office

Office's normal update installs the exact macOS arm64 bundle selected by `office/harbor-release.json` in mac-mini-client. It does not build Harbor or install Homebrew on the laptop. The bundle contains Harbor, its media conversion tool, demo resources, libmagic data and license/source notices. Existing Telegram profiles remain in their selected directories.

The hosted `Harbor macOS bundle` workflow builds and smoke-tests the bundle. A dispatch produces a dry-run artifact. A tag named `harbor-<native-version>-<full-source-commit>` publishes the exact matching private release. Its generated `harbor-release.json` is the consumer pin; copy it into Office only after the release exists. The archive checksum binds the uploaded bytes, including the exact source manifest and CPU target. Rebuilding a dry run does not establish the checksum of a later release.

## Operational observations

The private operational journal records native run identity, loaded version and source revision, semantic UI actions and typed request outcomes. Account and conversation keys are opaque identifiers. Keystrokes, draft or message text, clipboard contents, search contents and draft-derived UI comparison tokens are excluded.

The authenticated UI control offers a separate window-metadata read, listed in `harbor --help`. It returns the exact native run and loaded build for its PID and cmux surface, plus opaque conversation identity and current view mode. PID alone is insufficient because an in-place restart keeps it. This read writes no journal event, so observing a window cannot retrigger diagnostics. A launch workspace is historical metadata; Office’s current cmux read supplies the current location.

## Build from source (developers)

Install the [nchat build dependencies](doc/nchat-upstream-readme.md#dependencies), plus Go 1.26 or later, FFmpeg and Python 3. On macOS, Harbor also uses the system audio player and clipboard.

```sh
git clone https://github.com/papercutlabs/harbor.git
cd harbor
utils/build-harbor
~/.local/bin/harbor --demo
```

The build script installs into a new directory under `~/.local/harbor/releases/` and updates its managed Harbor launcher. It does not replace nchat or overwrite a running release.

For a real Telegram account, run `harbor -s` once and complete Telegram authentication. After setup, use `harbor`. Configuration defaults to `~/.config/harbor`; the offline demo uses a separate directory. Updates do not require account setup again.

Before arranging a terminal for Harbor, Office runs `harbor --check-session` with the same startup arguments. The check uses the effective configuration directory, including the existing order of `--demo` and `-d DIR` (or `--confdir DIR`). It validates initial conversation arguments without opening a conversation. Exit 0 means no running session or a compatible welcome; exit 1 prints the original session error on stderr. It creates no directories, logs, locks, profiles or backend. A live backend sees one transient same-user connection with no requests. Connection and welcome reads share a two-second limit. A stale same-user socket is left intact for ordinary startup to recover. Unknown path types, inaccessible peers, malformed welcomes and incompatible versions fail. Setup, removal, export, keydump, backend, UI-control, help and version modes cannot be combined with the check.

Session versions normally must match exactly. The verified upgrade exceptions are client 0.1.23 with backend 0.1.22, client 0.1.24 with backend 0.1.22 or 0.1.23, client 0.1.25 with backend 0.1.24, client 0.1.26 with backend 0.1.25, client 0.1.27 with backend 0.1.26, and client 0.1.28 with backend 0.1.27. Each unequal upgrade pair is qualified explicitly. The 0.1.27 backend-generation record is local and does not extend the session wire format. No other unequal version pair is accepted. Existing windows and their drafts can stay open during those upgrades.

[Interaction guide and terminal support](doc/harbor.md) · [Latest changes](doc/harbor-0.1.19.md)

## Development checks

After building, install the Python test dependencies and run the native checks:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/harbor/requirements.txt
.venv/bin/python tests/harbor/native_smoke.py
.venv/bin/python tests/harbor/selection_smoke.py
.venv/bin/python tests/harbor/desk_handoff_smoke.py
.venv/bin/python tests/harbor/session_check_smoke.py
```

Additional PTY checks cover formatting, history loading, updates, media, and word deletion. They use isolated offline demo accounts and do not send Telegram messages. macOS clipboard checks use a private named pasteboard. Set `HARBOR_EXECUTABLE` to test a specific installed binary.

The native implementation has been exercised on macOS/cmux. Other terminal and Linux paths are present but have less runtime coverage. Video playback opens an external player; it is not an in-terminal video player. The update prompt applies already-installed releases rather than downloading or building them automatically.

## Office Desk entries

`HARBOR_DESK_STRIP_FILE` opts one Harbor UI into a four-row Desk header across the full terminal width. The conversation, account and chat history sit beneath it. Other UIs on the same session keep their normal layout. The producer atomically replaces a UTF-8 file of at most 16 KiB: `pcl.desk.strip.v1<TAB><Unix epoch milliseconds>` on the first line, then exactly four plain-text rows. A final newline is optional. Harbor checks the file while idle and clips each row to the available display cells. Missing, malformed, future-dated or more than 90-second-old data shows four unavailable rows. Terminal controls are rejected. Desk command handoff and return continue to preserve the draft and reading position.

The inline Office owner may also pass `HARBOR_DESK_UPDATE_FILE`. A private regular file containing `pcl.desk.update.v1<TAB><owner PID><TAB><40 lowercase hex revision><NEWLINE>` requests an update. Harbor accepts only its direct parent PID and rejects malformed, oversized, non-private or future-modified files. Harbor acknowledges only after two minutes without terminal input, with the current chat at latest, every chat draft/reply/edit empty, and no menu, Desk child, route, authentication, mouse interaction or selection active. It first saves state, then atomically replaces the request with the same fields under `pcl.desk.updated.v1` and exits this UI normally. A user quit leaves the request unchanged. Office owns the restart; Harbor never launches an updater or stops sibling UIs.

Update-enabled UIs with a valid `CMUX_SURFACE_ID` save to `harbor-state.surface-<hex surface ID>`. On the first load, if that file is absent, a shared-session UI may read its own locked numeric slot file without changing it. It never reads another slot. Existing unavailable or non-regular surface state blocks automatic updates, as does a missing surface identity or failed state save. Reopening the same surface restores its state even if another UI has acquired its former numeric slot. UIs without update opt-in retain their existing numeric state paths.

Started by Office Desk, Harbor receives `HARBOR_DESK_COMMANDS`, a file of tab-separated lines `label<TAB>hint<TAB>argv0<TAB>argv…`. Each line becomes a `Desk · <label>` entry in the Commands menu. Choosing one saves state, releases the terminal (mouse, paste and enhanced-key modes, graphics), runs that argv directly on the same terminal with no shell, then restores the modes and repaints. The Harbor process, chats, drafts, cursors, reply/edit context and reading positions are untouched. A child that could not start or exited non-zero leaves its own output on screen until a new key (`press any key to return to Harbor`; input queued before the message, such as the release of the click that chose the entry, and mouse reports do not count); an interrupted child (Ctrl+C) returns at once. Each outcome is also reported in the status line. Option-/ (or Ctrl+O) opens Commands from any draft and leaves it unchanged; a plain `/` is always draft text. Without the variable Harbor has no Desk entries.

## Upstream and license

Harbor reuses nchat's Telegram backend and retains the upstream history and notices. The original project documentation is preserved in [nchat's README](doc/nchat-upstream-readme.md). See [LICENSE](LICENSE) and the generated third-party notices included in installed builds for dependency terms.
