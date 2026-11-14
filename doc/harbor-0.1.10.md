# Harbor 0.1.10

Forum topics appear in Option+K as ordinary conversations named **Group > Topic**. Search matches the group and topic names; opening a topic goes straight to its own history and composer. Drafts, reading positions, attachments, replies, and sends use the topic's conversation identity. Telegram history and send requests retain the real parent chat ID and forum topic ID separately.

Pin ordering is retained. Each picker result shows its actual last-message time on the right, independent of the pin-order value used for sorting. A short last-message preview is shown inline only in wider pickers when the name leaves enough space. Narrow pickers keep the name and time without another row.

Up/Down accepts both standard CSI and application SS3 arrow sequences, plus enhanced CSI-u arrow encodings. Selection stays on the same conversation if an incoming message reorders the list while the picker is open. Changing the search starts from the first match.

Forum discovery is enabled by Harbor; stock nchat keeps its original chat-list mode. Topic metadata is paginated and cached using distinct conversation IDs. Read status, reactions, attachments, edits, and deletion notifications are routed back to the loaded topic.

Validation: offline native PTY tests cover topic search, ordinary picker entries, independent parent/topic/sibling histories and drafts, sending within a topic, pin-first sorting, time/preview layout, arrow variants, and incoming-message reordering. A C++ test constructs the actual TDLib forum-history and send-topic objects and verifies both IDs. The stock nchat target also builds. Automated checks do not send live Telegram messages.

Apply the installed update from Harbor's update banner. Then search for either the group or the topic name in Option+K.
