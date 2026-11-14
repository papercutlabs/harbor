# Harbor 0.1.5

Compact terminal formatting is now native: blue bold emphasis and headings, green inline/fenced code without a separate inline badge, muted quote rails, italic text, and underlined links. Recognized paired formatting markers are hidden; unmatched markers, escaped punctuation, and code contents stay literal. Link destinations remain visible. The original message is preserved for copying, editing, quoting, and protocol requests. Struck text is dimmed because terminal strikethrough is not portable.

Short conversations now align to the bottom of the history area, just above the composer. This works when changing chats, resizing, composing multiple lines, and receiving/sending new messages. Deliberately scrolled reading positions are retained.

Restart with Ctrl+Q, then `harbor`; setup is not needed again.

Validation: C++ formatting parser tests and real native PTY tests cover colors, nested formatting, literals, Unicode, line wrapping, bottom placement after switching/resizing, retaining previous messages, and preserving older reading positions. The PTY emulator now handles standard CSI S/T scroll operations emitted by ncurses, so efficient history scrolling is represented correctly in test captures.
