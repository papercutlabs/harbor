# Harbor view switching correction

Teren reports that Everything feels like loading a second script. Harbor currently leaves its screen and creates an Office process on every visit. Preserve Harbor's process and all conversation state, and keep one directly owned Office child between visits. The first visit starts it lazily; subsequent visits reuse its imports and controller.

Harbor starts only a fixed Office view argv from its existing command file. A private inherited socket carries bounded prepare/ready/yield/released messages with monotonically increasing request identities. Office validates the exact declared argv and current owner before readiness. No keyboard reader is created until Harbor stops and joins its reader and explicitly yields. Office closes its dedicated tty reader and drains actions before release. Harbor resumes only on matching release or after killing and reaping its exact child. Unknown actions are never replayed. Both renderers share Harbor's alternate screen; one-shot commands retain their existing lifecycle.

The owner strip remains the sole instance owner, Focus runner and updater. The child retains its controller only while owner process identity and surface match. Each mounted view gets a new cancellation scope. Last inventory and quota observations retain their original freshness. Existing action checks remain authoritative.

Definition of done:
1. Repeated declared view visits use one child, preserve Harbor state and avoid alternate-screen leave/enter.
2. Preparation rejection never acquires tty; malformed, stale, EOF and exact-child failure recover without two readers or effect replay.
3. Each release follows input close and action drain; nested child lifetime cannot outlive terminal custody.
4. Resize, terminal flags, extended keys and typeahead are restored across transfers.
5. Tests exercise real protocol/decoder/render paths, retained inventory and freshness, owner replacement and failure cases. Hosted current-source Harbor qualification remains required; no local full build or laptop proof is claimed.

Build only in the parent-provided isolated checkouts. No push, release, live cmux or profile changes. Parent commissions the single independent review and owns integration, hosted checks and consumer acceptance.
