# Harbor 0.1.19

1. Several Harbor windows can be open on the same Telegram account at once. Each window keeps its own conversation, reading position and drafts, and restores them when it is reopened.
2. All windows share one hidden session process, which starts with the first window and ends with the last. Sign-in prompts use one open window’s terminal, and Harbor returns to that window afterwards. If that window closes during sign-in, the session ends with drafts saved.
3. If the session ends unexpectedly, every window saves its drafts and exits. Messages whose sending was never confirmed come back as drafts, and Harbor does not resend them.
