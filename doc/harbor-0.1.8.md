# Harbor 0.1.8

Right-click a message to open its context menu. It contains exactly three actions: Reply, Forward, Copy text. The keyboard message menu (Esc, arrows, Enter) uses the same three actions.

Left-click no longer opens the message menu. Left-drag still selects text and copies it automatically on release. Ordinary buttons and picker results continue to use left-click.

Additional tools such as editing a sent message or opening an attachment are available under `/ commands` → More message tools. The commands button is now available while browsing messages as well as composing.

Validation: native PTY checks cover left-click, right-click, selection copying, reply, forward, clipboard copying through a private named pasteboard, unchanged drafts, and existing media/edit/recovery behavior. All checks use offline demo accounts.
