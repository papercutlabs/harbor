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
| Copy a code block | Click its top-right ⧉ icon; copies text only, without running it |
| Open a message link | Click any word or space in its label; dragging still selects |
| Download an attachment | Click its file row; saves to Downloads without opening it |
| Open or play an attachment | Message actions → More message tools → Open / play attachment |
| Copy selected text | Drag across message text; release copies automatically |
| Return to composing | Start typing or press Esc again |
| Find a conversation | Option+K; Ctrl+K fallback |
| Commands | Option+/; Ctrl+O fallback; footer button. A plain `/` is always draft text. |
| Keyboard help | Commands → Keyboard help; Desk keys is inside this view |
| Running versions | Commands → About; Harbor build and live serving Desk revision |
| Voice pause / stop | Space while browsing pauses or resumes; Message actions → More message tools → Stop playback |
| Attach | Ctrl+T; paste/drop file paths; clipboard image via Ctrl+V |
| Attach caption | Type in the attachment preview; Shift+Enter for newline |
| Confirm attachment | Enter on Send, or click Send |
| Dismiss popup | Esc or click outside; outside click is consumed |
| Older/newer history | PageUp/PageDown or mouse wheel |
| Follow latest | Ctrl+G |
| Refresh active conversation | Ctrl+R |
| Copy a code block exactly | Message actions → Copy code (a chooser when there are several) |
| Visit the replied-to message | Message actions → Go to replied message; Commands → Return to previous position |
| Search this conversation or topic | Commands → Search or pinned messages; type, then Enter (searches Telegram) |
| Visit pinned messages | Commands → Search or pinned messages → Pinned messages (opening never pins or unpins) |
| React, open links, spoilers, quotes | Message actions → More message tools → React…, Open a link…, Reveal spoiler, Expand quote |

## Formatting

Telegram messages keep their original text and formatting ranges (TDLib UTF-16 offsets) beside nchat's Markdown-style text, so Harbor shows bold, italic, underline, strikethrough, code, block and expandable quotes, spoilers and named links without guessing from markup. A Telegram message without formatting shows literal text, stars and backticks included. Older cached messages without ranges use the previous Markdown reading. Shell blocks omit the language label. Each code block has a top-right copy icon, beside its first line when there is room and above it when that line fills the width. Other named code blocks keep a small `‹language›` header; whole-message and code copies never include headers, quote bars or line wrapping. Spoilers show as `▒` until Reveal spoiler, also in previews and reply excerpts. Strikethrough uses a combining long-stroke overlay because ncurses has no strike attribute; `HARBOR_STRIKE=dim` selects a dimmed fallback. The overlay is a Unicode fallback, not a terminal strike capability: how it looks depends on the font, and with `HARBOR_STRIKE=dim` struck text takes the dimmed metadata colour instead of its link or code colour. Tabs inside code display as one space in the conversation; copies keep the original tabs. Forwarding uses the decoded message text (never markup) and refuses while a spoiler is hidden. Picker previews of native messages are shown literally.

Cache schema 13 adds a nullable `messages.richText` column (`ALTER TABLE … ADD COLUMN`). Existing rows are kept and read as before. An older Harbor or nchat reading a schema 13 cache selects named columns, so it ignores the new column and logs only its usual newer-schema notice. Drafts live in Harbor's separate state file and are untouched.

Schema 13 is a Harbor-owned number: upstream nchat stops at 12 and may assign its own 13 later. To reconcile with a future upstream schema, keep the change additive and check the column before adding it; the 12→13 step already checks `pragma_table_info('messages')` for `richText`, so it is idempotent and can be renumbered after upstream's own steps without breaking existing caches.

Live edits re-fetch the message with `sequence=false`, and the message cache does not store such results, so a cached edited message keeps its pre-edit text and richText (the pair stays consistent) until an in-sequence re-fetch replaces both. This gap is pre-existing for text and outside this change; live edit caching is not fixed here.

Forum topics appear alongside ordinary chats as `Group > Topic`. Option+K searches both parts of the name, retains pin ordering, and shows actual last-message times. Previews are shown only where the row has room. Topics open directly and retain independent drafts and history.

At fewer than 100 terminal columns, Harbor gives the conversation the full width. Wider windows retain the conversation list. Draft text, cursor, reply/edit context, and reading position are stored per conversation in a private state file. New replies follow the viewport when it is at the bottom; when reading older messages, a new-message action returns to the latest.

## Media and terminal support

Images and video thumbnails use Kitty graphics in supported terminals (including the user's cmux/Ghostty setup), with a half-block color fallback elsewhere. Previews are letterboxed to the terminal cell proportions; partial scrolling clips the source image. File-row clicks save a copy in the configured Downloads directory, adding a numbered suffix when the name already exists. The status shows the saved path. Saving never opens or executes the downloaded file. The explicit Open / play attachment action offers an external opener; voice messages play in-app through FFmpeg decoding and the system audio player. Space pauses/resumes active playback. Switching conversations stops playback.

`HARBOR_GRAPHICS=blocks` forces the fallback; `HARBOR_GRAPHICS=kitty` forces graphics. In tmux the fallback is the default. `HARBOR_REDUCED_MOTION=1` disables the short menu transition. Nerd Font receipts use one integrated double-check glyph; `HARBOR_ICONS=plain` selects portable text receipts.

Shift+Enter accepts CSI-u/xterm encodings and legacy Escape+Return. Option+Backspace accepts legacy Escape+Delete/Backspace and enhanced CSI-u/xterm encodings. A terminal that intercepts a chord may require a terminal binding; the fallback chords above avoid that dependency.

## Build and validation

Run `utils/build-harbor` with CMake, Go 1.26 or later, FFmpeg, Python 3, and the nchat build dependencies installed. The script builds the Harbor target into a new versioned release directory and switches its managed Harbor launchers atomically. It refuses an existing release directory and does not overwrite loaded libraries or unmanaged commands.

Native PTY checks run against the offline demo protocol, with temporary account directories. `HARBOR_EXECUTABLE=/absolute/path/to/harbor` selects a specific installed binary:

- `tests/harbor/topics_picker_smoke.py`: topics, history/draft/send isolation, pin-first ordering, timestamps/previews, arrow encodings, and stable selection during live updates.
- `harbor-forum-test`: actual TDLib forum-history and send-topic request fields.
- `harbor-richtext-producer-test`: actual td_api entities through mention expansion (the shared ProcessMentionEntities helper), the native conversion, the stored encoding and the renderer; the legacy `getMarkdownText` text is unchanged.
- `harbor-format-test`, `harbor-cache-richtext-test`: UTF-16 ranges, literal zero-entity text, malformed ranges, code/link/spoiler/quote handling; additive cache migration, in-sequence re-fetch (not live edits) and restart reload through MessageCache; the cache test needs SQLite >= 3.35 for `DROP COLUMN`.
- `tests/harbor/rich_actions_smoke.py`: native formatting at 56 and 110 columns (targets brought into view with PageUp/PageDown, no larger screen), strike overlay, wrapping at 56 columns, drag copy without code headers, concealed spoilers in reply excerpts and picker previews, code/text copies into a private clipboard file, link chooser, reply visit/return, immediate explicit original failure (`HARBOR_DEMO_FETCH_ERROR=1`) without a stale return mark, search and pins (including failures and topic identity), reactions and stale replies, all with drafts kept. `HARBOR_TEST_EVENT_LOG` records search reply receipt, mouse deferral and acceptance or the reason a reply was ignored; the demo request log records each delayed search emission.
- `tests/harbor/run_checks.py` runs the lanes of `tests/harbor/check_inventory.json` in order and stops at the first failure: static, format, producer, cache, media, input_decoding (`go test` in `src/harbor/input`), native_smoke, formatting, right_click, restart_position, draft_scroll, send_replacement, rich, selection, desk, topics. `HARBOR_CHECK_LANE` selects one. It never builds; it prints each binary's path, size, mtime and sha256 first, so rebuild before running. The producer lane is reported SKIP, with its reason, only in a build tree without a Telegram backend.
- `tests/harbor/native_smoke.py`: interaction, drafts, restarts, recovery, incoming replies, media actions, graphics transfers, setup protection.
- `tests/harbor/refinement_smoke.py`: modified Return, word wrapping/cursor geometry, padding-era layout, outside dismissal, editable captions.
- `tests/harbor/word_delete_smoke.py`: legacy/enhanced Option+Backspace, Ctrl+W, Unicode, cursor-position edits, captions.
- `tests/harbor/line_delete_smoke.py`: Cmd+Backspace (legacy Ctrl+U or Kitty CSI-u Super+Backspace) deletes only the current display row: end, middle, line start, Unicode, wrapped lines, captions; Option+Backspace and Backspace unchanged. Xterm modifier 9 decodes as Meta, so it is not claimed as Super+Backspace coverage.
- `tests/harbor/right_click_smoke.py`: primary-action context menu (React…, links, spoiler and quote actions stay in Message tools, which is checked to offer them), reply/forward/copy, quiet left-click, and unchanged drafts.
- `tests/harbor/selection_smoke.py`: SGR drag/release copying, wrapping, multiple messages, Unicode, and a private test clipboard.
- `tests/harbor/graphics_smoke.py`: decoded portrait PNG proportions, letterboxing, source cropping during scrolling.
- `harbor-media-test`: subprocess quoting/timeouts, file parsing, image/video decoding and actual voice playback/pause. Its optional clipboard test uses a named test pasteboard.

Validation scope: automated checks use the real native executable and an offline Protocol, rather than mocked UI functions. The user's live screenshots establish Telegram authentication, text delivery, receipt rendering, and inline image rendering in cmux. We do not send test messages to live chats. Terminal-protocol capture proves the emitted PNG/crop data; exact on-screen graphics behavior remains dependent on the emulator. Video playback opens the system handler; it is not decoded into an in-terminal video player.

Harbor uses the pinned Ultraviolet input reader through a statically linked Go/C bridge; ncurses only draws the interface. The reader normalizes legacy and Kitty keyboard input, SGR mouse events, and bracketed paste. Unsupported control reports and key releases do not become draft text. Run `go test -race ./...` in `src/harbor/input` for stream and stdin-ownership tests. Click the composer or attachment caption to position its cursor using the same layout map as rendering.
