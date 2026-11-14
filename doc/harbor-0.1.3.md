# Harbor 0.1.3

Restart with Ctrl+Q, then `harbor`. Existing running sessions retain their original release. No setup or login reset is needed.

- Attachment previews now accept caption typing and normal left/right/backspace editing. Existing draft text becomes the caption. Shift+Enter inserts a newline; Enter confirms the selected Send/Cancel action. Cancel preserves the draft.
- Word-aware wrapping is shared by messages, composition, caption display, and composer cursor navigation. Oversized tokens still wrap by terminal cell width.
- Message panels have two columns of internal horizontal padding and a blank row above/below their content. The composer starts with two editable rows, grows to five, and shows one prompt.
- Message actions use a compact list. Clicking outside a modal dismisses it and consumes the click; mouse wheel navigation stays inside the modal.
- Legacy Escape+Return is decoded as modified Return, preventing it from selecting a message and opening actions. CSI-u and xterm modified Return mappings remain supported.

Validation: `tests/harbor/native_smoke.py` and `tests/harbor/refinement_smoke.py` run the native executable in isolated pseudoterminals against the offline protocol. They do not send Telegram messages. Refinement captures in `design/harbor-refined-*` are rendered from actual terminal cells, using demo data.
