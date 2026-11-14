# Harbor 0.1.7

Drag across message text to select it. Releasing the mouse automatically copies the selection, with a visible highlight and “Copied selection” confirmation. No modifier key or separate copy command is needed. Clicking without dragging still opens message actions, now on release.

Selection works forward or backward, across wrapped lines and multiple messages. It copies rendered text without Markdown markers, excludes sender/time metadata and box padding, joins soft wraps, preserves real paragraph breaks, and handles wide and combining characters. Typing clears the highlight and continues composing; Esc clears it. Drafts are retained.

Incoming UI events wait while the mouse button is held to prevent text moving under the selection; they are processed after release. A lost mouse-up is bounded by a 30-second idle safeguard, and any keyboard action or wheel scrolling also cancels the held selection. Terminal mouse reporting uses button motion rather than all pointer motion, and is disabled on exit.

Validation: `selection_smoke.py` sends actual SGR press/drag/release events through the native executable and reads copied text from a private named macOS pasteboard. It covers automatic copying, reverse selection, soft wrapping versus real newlines, multiple messages, Unicode, unchanged drafts, popup suppression during a drag, and normal click actions. It never changes the user's general clipboard.

Use the update banner in Harbor 0.1.6, or restart with Ctrl+Q then `harbor`.
