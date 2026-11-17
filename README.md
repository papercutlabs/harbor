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

[Interaction guide and terminal support](doc/harbor.md) · [Latest changes](doc/harbor-0.1.19.md)

## Development checks

After building, install the Python test dependencies and run the native checks:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/harbor/requirements.txt
.venv/bin/python tests/harbor/native_smoke.py
.venv/bin/python tests/harbor/selection_smoke.py
.venv/bin/python tests/harbor/desk_handoff_smoke.py
```

Additional PTY checks cover formatting, history loading, updates, media, and word deletion. They use isolated offline demo accounts and do not send Telegram messages. macOS clipboard checks use a private named pasteboard. Set `HARBOR_EXECUTABLE` to test a specific installed binary.

The native implementation has been exercised on macOS/cmux. Other terminal and Linux paths are present but have less runtime coverage. Video playback opens an external player; it is not an in-terminal video player. The update prompt applies already-installed releases rather than downloading or building them automatically.

## Office Desk entries

Started by Office Desk, Harbor receives `HARBOR_DESK_COMMANDS`, a file of tab-separated lines `label<TAB>hint<TAB>argv0<TAB>argv…`. Each line becomes a `Desk · <label>` entry in the Commands menu. Choosing one saves state, releases the terminal (mouse, paste and enhanced-key modes, graphics), runs that argv directly on the same terminal with no shell, then restores the modes and repaints. The Harbor process, chats, drafts, cursors, reply/edit context and reading positions are untouched. A child that could not start or exited non-zero leaves its own output on screen until a new key (`press any key to return to Harbor`; input queued before the message, such as the release of the click that chose the entry, and mouse reports do not count); an interrupted child (Ctrl+C) returns at once. Each outcome is also reported in the status line. Option-/ (or Ctrl+O) opens Commands from any draft and leaves it unchanged; a plain `/` is always draft text. Without the variable Harbor has no Desk entries.

## Upstream and license

Harbor reuses nchat's Telegram backend and retains the upstream history and notices. The original project documentation is preserved in [nchat's README](doc/nchat-upstream-readme.md). See [LICENSE](LICENSE) and the generated third-party notices included in installed builds for dependency terms.
