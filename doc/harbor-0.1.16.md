# Harbor 0.1.16

Editing a draft no longer resumes following latest or clears unseen messages. Typing, newlines, pasted text and composer cursor interaction leave the conversation at its reading position. Incoming messages no longer infer follow mode from the current viewport position.

Choose Latest (Ctrl+G or the Latest/new-message action) to resume following. Sending a message still moves to latest. Draft and viewport state are independent and continue to survive chat switching and restart.

Native regression tests verify typing, multiline drafts, paste, chat switching, a reply arriving while drafting in older history, and explicit Latest behavior. Restart-position tests also pass.
