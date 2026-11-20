# Harbor jobs

Harbor lets its user hold Telegram conversations in a terminal.

1. Read and write messages with native media, mouse, paste and keyboard behavior.
2. Keep each conversation and topic’s draft, reply/edit context, cursor and reading position while the user temporarily visits a Desk view.
3. Reach actions through the existing Commands menu with Option-/ (also Ctrl+O, Cmd+O or the clickable `⌥/ commands` target) from any draft. A plain `/` and all other printable draft text remain literal, even in an empty draft.
4. When used with Office Desk, offer Desk’s accepted controls without changing Telegram identity or restarting remote seats. Harbor without Desk remains ordinary Harbor.
5. Show messages as their sender formatted them: bold, italic, underline, strikethrough, code, block and expandable quotes, spoilers and named links, with exact text through edits and cache reloads. Spoilers stay hidden until revealed, including in previews and reply excerpts.
6. Copy exactly what was meant: one chosen code block without markup or wrapping, or the whole message as its sender wrote it.
7. Visit the message a reply refers to, even when it is older than loaded history, and return to the former reading position with the draft, cursor and reply/edit context unchanged.
8. Search the current conversation or forum topic on Telegram and visit pinned messages, then return. No match, failure and no answer are distinct, and a local-only search never claims full-history coverage. Opening pins never pins or unpins.
9. React with a reaction the selected message actually allows; opening the chooser sends nothing, and late replies never apply to another conversation or message. Choose which of a message’s real link destinations to open.
10. See which conversation is open: its name heads the screen and titles the terminal. Normal connected state shows no brand or status row; only connecting or authenticating is shown. While reading older messages, new incoming messages show one count-only bar (`↓ N new messages`, Ctrl+G) above the composer; Ctrl+G or a click goes to the latest and keeps the draft, reply/edit and attachments.

11. See incoming Telegram typing as an animated three-dot bubble below the latest message. Activity is specific to the account and conversation or topic, clears on provider stop, provider expiry, reply or disconnect, and stays out of older history without moving the draft. Reduced motion keeps the dots still.

Source: PCL-1467, Teren’s incoming typing request in Telegram 11355.

Source: PCL-1302, Teren’s accepted interaction in Telegram9890 and implementation instruction9944. The shipped hook is `HARBOR_DESK_COMMANDS` (Commands entries run as argv on Harbor's own terminal, Ctrl+O opens the same menu from any draft). It composes the retained offline handoff experiment's design onto current Harbor, without that experiment's build shims. Desk’s lead owns cross-repository delivery; no broader Harbor takeover is commissioned.

Source for 10 and the Option-/ change to 3: PCL-1378, Teren’s request in Telegram 10553 and approved new-message indicator A (PCL-1311).

Source for 5–9: PCL-1312, Teren’s acceptance of all six proposals in Telegram 10014. They live in the existing message actions, More message tools and Commands, with transient choosers; there is no new permanent chrome.
