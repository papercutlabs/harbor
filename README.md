# Harbor

A keyboard-first Telegram TUI built on [nchat](https://github.com/d99kris/nchat) and TDLib. Designed for a narrow terminal, with a quiet blue palette and the conversation kept close to the composer.

Harbor is a native terminal application, not a web mockup. It builds as a separate executable and keeps its own account directory, so stock nchat is preserved.

## Features

- Full-width conversations in narrow terminals; persistent chat list in wider windows.
- Type to compose, Enter to send, Shift+Enter for a newline.
- Option+K chat search, with Ctrl+K as a fallback. Forum topics appear as Group > Topic, with last-message times and pin-first ordering.
- Colored message formatting, replies, edits, reactions, and message actions.
- Per-chat drafts and reading positions; automatic initial history loading.
- Drag to select message text; releasing copies it automatically.
- Inline photos and video thumbnails, with a terminal color fallback.
- In-app voice playback, external video opening, and attachment previews with captions.
- A restart prompt when a newer Harbor release is installed locally.

## Build and run

Install the [nchat build dependencies](doc/nchat-upstream-readme.md#dependencies), plus FFmpeg and Python 3. On macOS, Harbor also uses the system audio player and clipboard.

```sh
git clone https://github.com/papercutlabs/harbor.git
cd harbor
utils/build-harbor
~/.local/bin/harbor --demo
```

The build script installs into a new directory under `~/.local/harbor/releases/` and updates its managed Harbor launcher. It does not replace nchat or overwrite a running release.

For a real Telegram account, run `harbor -s` once and complete Telegram authentication. After setup, use `harbor`. Configuration defaults to `~/.config/harbor`; the offline demo uses a separate directory. Updates do not require account setup again.

[Interaction guide and terminal support](doc/harbor.md) · [Latest changes](doc/harbor-0.1.13.md)

## Development checks

After building, install the Python test dependencies and run the native checks:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/harbor/requirements.txt
.venv/bin/python tests/harbor/native_smoke.py
.venv/bin/python tests/harbor/selection_smoke.py
```

Additional PTY checks cover formatting, history loading, updates, media, and word deletion. They use isolated offline demo accounts and do not send Telegram messages. macOS clipboard checks use a private named pasteboard. Set `HARBOR_EXECUTABLE` to test a specific installed binary.

The native implementation has been exercised on macOS/cmux. Other terminal and Linux paths are present but have less runtime coverage. Video playback opens an external player; it is not an in-terminal video player. The update prompt applies already-installed releases rather than downloading or building them automatically.

## Upstream and license

Harbor reuses nchat's Telegram backend and retains the upstream history and notices. The original project documentation is preserved in [nchat's README](doc/nchat-upstream-readme.md). See [LICENSE](LICENSE) and the generated third-party notices included in installed builds for dependency terms.
