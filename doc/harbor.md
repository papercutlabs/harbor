# Harbor

Harbor is the native Telegram TUI in this nchat fork. It shares nchat's Telegram/TDLib backend and builds as a separate executable. It uses its own account directory, `~/.config/harbor`; stock `nchat` and `~/.config/nchat` are retained.

## Run

- `harbor`: open the configured Telegram account.
- `harbor -s`: first-time account setup; follow Telegram authentication prompts. Existing users do not need setup again after an update.
- `harbor --demo`: isolated offline demo, without Telegram credentials.
- `harbor --version`: inspect the installed release.
- Ctrl+Q exits. After installing an update, exit and run `harbor` again. Already running clients keep using their original executable and libraries.

## Interaction

| Action | Input |
| --- | --- |
| Compose | Start typing |
| Send | Enter |
| Newline | Shift+Enter; Ctrl+J fallback |
| Delete previous word | Option+Backspace; Ctrl+W fallback |
| Browse messages | Esc; Up/Down select |
| Selected message actions | Right-click a message; or Esc, arrows, Enter |
| Copy selected text | Drag across message text; release copies automatically |
| Return to composing | Start typing or press Esc again |
| Find a conversation | Option+K; Ctrl+K fallback |
| Commands | `/` in an empty composer; footer button |
| Attach | Ctrl+T; paste/drop file paths; clipboard image via Ctrl+V |
| Attach caption | Type in the attachment preview; Shift+Enter for newline |
| Confirm attachment | Enter on Send, or click Send |
| Dismiss popup | Esc or click outside; outside click is consumed |
| Older/newer history | PageUp/PageDown or mouse wheel |
| Follow latest | Ctrl+G |
| Refresh active conversation | Ctrl+R |

Forum topics appear alongside ordinary chats as `Group > Topic`. Option+K searches both parts of the name, retains pin ordering, and shows actual last-message times. Previews are shown only where the row has room. Topics open directly and retain independent drafts and history.

At fewer than 100 terminal columns, Harbor gives the conversation the full width. Wider windows retain the conversation list. Draft text, cursor, reply/edit context, and reading position are stored per conversation in a private state file. New replies follow the viewport when it is at the bottom; when reading older messages, a new-message action returns to the latest.

## Media and terminal support

Images and video thumbnails use Kitty graphics in supported terminals (including the user's cmux/Ghostty setup), with a half-block color fallback elsewhere. Previews are letterboxed to the terminal cell proportions; partial scrolling clips the source image. Selecting an attachment offers an external opener; voice messages play in-app through FFmpeg decoding and the system audio player. Space pauses/resumes active playback. Switching conversations stops playback.

`HARBOR_GRAPHICS=blocks` forces the fallback; `HARBOR_GRAPHICS=kitty` forces graphics. In tmux the fallback is the default. `HARBOR_REDUCED_MOTION=1` disables the short menu transition. Nerd Font receipts use one integrated double-check glyph; `HARBOR_ICONS=plain` selects portable text receipts.

Shift+Enter accepts CSI-u/xterm encodings and legacy Escape+Return. Option+Backspace accepts legacy Escape+Delete/Backspace and enhanced CSI-u/xterm encodings. A terminal that intercepts a chord may require a terminal binding; the fallback chords above avoid that dependency.

## Build and validation

Run `utils/build-harbor` with CMake, Go 1.26 or later, FFmpeg, Python 3, and the nchat build dependencies installed. The script builds the Harbor target into a new versioned release directory and switches its managed Harbor launchers atomically. It refuses an existing release directory and does not overwrite loaded libraries or unmanaged commands.

Native PTY checks run against the offline demo protocol, with temporary account directories. `HARBOR_EXECUTABLE=/absolute/path/to/harbor` selects a specific installed binary:

- `tests/harbor/topics_picker_smoke.py`: topics, history/draft/send isolation, pin-first ordering, timestamps/previews, arrow encodings, and stable selection during live updates.
- `harbor-forum-test`: actual TDLib forum-history and send-topic request fields.
- `tests/harbor/native_smoke.py`: interaction, drafts, restarts, recovery, incoming replies, media actions, graphics transfers, setup protection.
- `tests/harbor/refinement_smoke.py`: modified Return, word wrapping/cursor geometry, padding-era layout, outside dismissal, editable captions.
- `tests/harbor/word_delete_smoke.py`: legacy/enhanced Option+Backspace, Ctrl+W, Unicode, cursor-position edits, captions.
- `tests/harbor/right_click_smoke.py`: three-action context menu, reply/forward/copy, quiet left-click, and unchanged drafts.
- `tests/harbor/selection_smoke.py`: SGR drag/release copying, wrapping, multiple messages, Unicode, and a private test clipboard.
- `tests/harbor/graphics_smoke.py`: decoded portrait PNG proportions, letterboxing, source cropping during scrolling.
- `harbor-media-test`: subprocess quoting/timeouts, file parsing, image/video decoding and actual voice playback/pause. Its optional clipboard test uses a named test pasteboard.

Validation scope: automated checks use the real native executable and an offline Protocol, rather than mocked UI functions. The user's live screenshots establish Telegram authentication, text delivery, receipt rendering, and inline image rendering in cmux. We do not send test messages to live chats. Terminal-protocol capture proves the emitted PNG/crop data; exact on-screen graphics behavior remains dependent on the emulator. Video playback opens the system handler; it is not decoded into an in-terminal video player.

Harbor uses the pinned Ultraviolet input reader through a statically linked Go/C bridge; ncurses only draws the interface. The reader normalizes legacy and Kitty keyboard input, SGR mouse events, and bracketed paste. Unsupported control reports and key releases do not become draft text. Run `go test -race ./...` in `src/harbor/input` for stream and stdin-ownership tests. Click the composer or attachment caption to position its cursor using the same layout map as rendering.
