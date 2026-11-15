# Harbor 0.1.12

Harbor remembers the active conversation across restart, including forum topics. Reading position is saved as a message and line offset, so loading a different history batch does not move it to an unrelated row. A conversation following latest continues to follow latest.

Telegram history no longer admits pending sending IDs. When TDLib confirms a send, the old temporary ID is removed from the UI and message cache before the permanent message is delivered. This closes a path that could display the same sent message twice. Messages with identical text remain separate when they have distinct permanent IDs.

Native offline tests cover active conversation restoration, reading position after partial history reload, latest-follow mode, pending-ID replacement, and legitimate repeated sends. These reproduce the faulty transitions without sending live Telegram messages; the specific duplicate in the reported screenshot was not inspected in the live account.
