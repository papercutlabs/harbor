# Harbor 0.1.15

Terminal input now goes through the pinned Ultraviolet reader used by Bubble Tea v2, with a small statically linked Go/C adapter. Harbor no longer uses ncurses keyboard decoding or a list of per-sequence registrations. The interface and rendering remain in C++/ncurses.

The reader handles legacy and Kitty keyboard input, UTF-8, mouse events and bracketed paste. Unsupported reports and key releases are ignored; repeat events work as key presses. The same reader backs the key inspector. Authentication stops and joins the reader before handing stdin to Telegram, then starts a fresh reader afterwards.

Click the composer to place the cursor. Click positioning shares the rendering layout and accounts for wrapped lines, scrolling within a long draft, wide characters and combining marks. Attachment captions support cursor placement too. Message text still uses drag-to-copy and right-click actions.

Builds now require Go 1.26 or later. Module versions and checksums are pinned in src/harbor/input/go.mod and go.sum. Go is only a build dependency; no Go installation is required to run the installed executable.

Validation includes the native UI regression suite; horizontal arrow variants; split input/UTF-8, unknown sequences, repeats and releases; mouse selection; composer and caption clicks; Option+Backspace variants; topic selection; update/restart; and a race-enabled reader ownership test. No live Telegram messages were sent during testing.
