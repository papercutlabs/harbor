# Shared Harbor session

An ordinary Harbor process is a window. It reserves the lowest free view-state slot and attaches to the same-binary backend for its configuration directory. Only the backend owns native profile/configuration locks, logging, cache and temporary-directory initialization/cleanup. Connect-or-spawn uses `session.lock`; startup includes an inherited socket connection, so there is no empty-session startup timer. Shutdown keeps that lock until native ownership has been released. A window creates a missing directory without deleting anything; only the locked backend initializes a missing version file in an otherwise empty directory.

Slot 0 reads and writes the existing `harbor-state` file. Other slots use `harbor-state.N`. State loads after slot selection. The existing draft, reply, edit and reading-position rows remain compatible. Additional `recover` rows retain complete serialized messages for failed or unknown sends, including rich text, quotes, reactions and attachments. On orderly close, signal shutdown or update restart, the window freezes its connection and consumes queued final results and unknown-send recoveries before its final state save. Restart never replays them. A native locally accepted send remains unknown until TDLib reports a final success/failure. Each pending native message is correlated by `(chat, temporary message id)` within its profile.

Every connection has a never-reused high 32-bit identity. The client supplies a monotonically increasing low 32-bit request sequence; either counter stops before wrap. Request-scoped replies echo that identity through native callbacks, cache queues and the codec. Tagged replies reach only their connection; untagged state updates reach all windows. Per-view current chats produce a union of native `openChat` subscriptions. Disconnect clears only that view's membership.

The socket is mode 0600, and both connectors and accepted peers validate the kernel peer UID. Frames consume their exact tagged shape and reject unsupported types, truncation, non-canonical booleans and trailing bytes. The 64 MiB frame budget accommodates the tested 50-message, 10 MiB rich-history page. Larger encoded values fail; they are not truncated. Each peer has at most 128 MiB queued encoded output, with frame headers included in that budget. Overflow closes that peer with a retained-session-ended outcome. Incremental input is parsed after each read, and the loop yields after 256 KiB per peer. No reconnect, replay, owner promotion, registry, deadline or per-window native profile is involved.

For native sign-in, one connected window stops its existing Go input reader, leaves ncurses and restores its captured original termios before passing terminal descriptors 0/1 through SCM_RIGHTS. The backend validates exactly two descriptors for the same terminal, then binds fd 0/1 for the existing `std::cin`/`std::cout` prompts. A small `std::cin` stream buffer polls the terminal together with a cancellation pipe. This lets lender socket death end an already waiting native read without relying on `dup2` to cancel it. Detaching fd 0/1 waits until the existing prompt returns control, so its own password-mode restoration finishes first. Shared-session native auth readers are joined during profile shutdown after cancellation; classic setup keeps its existing behavior. Normal return cancels input, restores the lent terminal mode and restores detached `/dev/null` descriptors before the window resumes. If the sign-in terminal dies while borrowed, that sign-in session ends and surviving windows receive a saved-drafts explanation; an ordinary window's death leaves the others usable. No auth input is recorded.

The actual wire contains 13 request types and 13 notify types. `ProtocolUiControlNotify` is a fourteenth handled control operation, represented by dedicated lend/lent/return frames rather than an encoded notify. The earlier source-only report's claim of 14 encoded notifies was a counting error.

| ServiceMessage subclass | Shared-session disposition |
| --- | --- |
| NewContactsNotify | Encoded; requested contacts/forum catalog replies tagged, unsolicited contact changes shared. |
| NewChatsNotify | Encoded; requested chats/forum pages tagged, unsolicited chat changes shared. |
| NewMessagesNotify | Encoded; cache/native/demo history, exact previews and search/visit fetches tagged; live messages/edits shared. |
| SendMessageNotify | Encoded; initiating view only. `pending` acceptance retains unknown-send recovery until final outcome. Native old pending sends without a current view owner do not restore drafts in every view. |
| ConnectNotify | Encoded shared profile state; latest state replayed on attach. |
| MarkMessageReadNotify | Not consumed by Harbor before or after this change; remains native/cache behavior. |
| DeleteMessageNotify | Encoded; requested deletion result tagged; unsolicited deletion/replacement shared. |
| SendTypingNotify | Not consumed by Harbor before or after this change. |
| SetStatusNotify | Not consumed by Harbor before or after this change. |
| CreateChatNotify | Not consumed by Harbor before or after this change. |
| ReceiveTypingNotify | Encoded shared update. |
| ReceiveStatusNotify | Not consumed by Harbor before or after this change. |
| NewMessageStatusNotify | Encoded shared update. |
| NewMessageIsPinnedNotify | Not consumed by Harbor before or after this change. |
| NewMessageFileNotify | Encoded; explicit download/open result tagged; unsolicited file state shared. |
| DeleteChatNotify | Not consumed by Harbor before or after this change. |
| MoveChatNotify | Not consumed by Harbor before or after this change. |
| UpdateMuteNotify | Not consumed by Harbor before or after this change. |
| ProtocolUiControlNotify | Local backend control; lend/lent/return handshake suspends/resumes one window. |
| RequestAppExitNotify | Encoded; native request to exit the session's windows. Local EOF also creates it after recovery events. |
| NewMessageReactionsNotify | Encoded shared message-state update, including the resulting state of a reaction action. |
| AvailableReactionsNotify | Encoded; initiating chooser only. |
| FindMessageNotify | Not consumed by Harbor before or after this change. |
| SearchMessagesNotify | Encoded; initiating search only. |
| UpdatePinNotify | Not consumed by Harbor before or after this change. |
| NewGroupMembersNotify | Not consumed by Harbor before or after this change. |
| UpdateArchivedNotify | Not consumed by Harbor before or after this change. |

The codec request allowlist is GetContacts, GetChats, GetMessage, GetMessages, SendMessage, EditMessage, DeleteMessage, SetStatus, DownloadFile, SetCurrentChat, GetAvailableReactions, SendReaction and SearchMessages. Reinit, deferred internal requests, setup, keydump, export and profile administration do not cross this boundary. Classic administration keeps its existing native locks.

Native checks use disposable demo homes/configurations and real Unix sockets/macOS PTYs. Their prompt fixture uses the fixed nonsecret value `12345`; it does not sign in to Telegram. The offline build, cache queue execution and TgChat syntax compilation do not establish live Telegram behavior or native multi-view read marks. Office context and per-window child-environment integration require the composed parent build's acceptance checks.
