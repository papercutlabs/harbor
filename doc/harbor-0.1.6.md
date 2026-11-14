# Harbor 0.1.6

- Initial history loading now automatically requests older batches until the visible conversation area is filled or history ends. TDLib can return fewer messages than requested. Previously Harbor accepted that short first batch and required a manual scroll to paginate. Repeated non-advancing responses use bounded delayed retries; an empty older batch ends pagination. Reading positions remain anchored when older rows are prepended.
- Conversation search now has at most eight single-line results, a separate search field, a clear selection arrow, and inline unread dots. The list scrolls as keyboard selection moves.
- Harbor checks its managed local launcher every three seconds. When a newer version is already installed, a persistent banner offers Update; the slash menu also has Update Harbor. The confirmation offers Restart or Later. Restart saves drafts and reading positions, logs out and cleans up, releases account locks, and replaces the process with the new executable using the same configuration directory. It never reruns account setup. It does not download or build releases from the internet.

One manual restart is needed to load this release: Ctrl+Q, then `harbor`. After that, installed updates can be applied from inside Harbor.

Validation: `loading_update_smoke.py` reproduces a short initial history batch, checks automatic history loading without scrolling, verifies picker bounds/search separation, simulates installing a newer release while Harbor is running, chooses Later, then performs a real process replacement and confirms draft restoration without account lock errors. All data is offline demo data in temporary directories.
