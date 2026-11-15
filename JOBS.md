# Harbor jobs

Harbor lets its user hold Telegram conversations in a terminal.

1. Read and write messages with native media, mouse, paste and keyboard behavior.
2. Keep each conversation and topic’s draft, reply/edit context, cursor and reading position while the user temporarily visits a Desk view.
3. Reach actions through the existing slash Commands menu, including when a nonempty draft requires the same menu through a separate chord or clickable target. Printable draft text remains literal.
4. When used with Office Desk, offer Desk’s accepted controls without changing Telegram identity or restarting remote seats. Harbor without Desk remains ordinary Harbor.

Source: PCL-1302, Teren’s accepted interaction in Telegram9890 and implementation instruction9944. The shipped hook is `HARBOR_DESK_COMMANDS` (Commands entries run as argv on Harbor's own terminal, Ctrl+O opens the same menu from any draft). It composes the retained offline handoff experiment's design onto current Harbor, without that experiment's build shims. Desk’s lead owns cross-repository delivery; no broader Harbor takeover is commissioned.
