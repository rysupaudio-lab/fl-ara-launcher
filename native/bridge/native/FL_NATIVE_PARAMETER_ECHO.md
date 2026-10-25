# Exact current-cache parameter echoes

Each owned playback child receives the original-only native component-state bytes used to prepare its ARA source cache. Before activation, an independent controller from the same factory is initialized and given those exact component bytes through setComponentState. A bounded128-parameter normalized readback is optional; failure leaves strict parameter invalidation unchanged.

Before cache publication, the bridge requires all rendered source modifications to report the same parameter identities, flags and values, and verifies those against the current genuine native controller. An immutable snapshot is bound to the resulting authorized cache ticket and exact component/controller association. Snapshot storage is replaced only while this component's strong process admission is closed and drained; every realtime snapshot reader holds that admission lease.

The realtime callback accepts an echo only for a current authorized snapshot, a bounded set of unique known parameter IDs, and exactly one finite equal-value point at offset0 per queue. Read-only, program-change and bypass parameters cannot qualify. Unknown values, multi-point/sample-offset automation, malformed queues, changed ownership or stale cache epochs retain normal invalidation and STOP/rebuild behavior. The original native process receives its untouched queue regardless. No controller call, allocation or lock occurs on the realtime path.

A headless native export is polled only after the stopped nonempty owned ARA graph is fully coherent. Nested native-export run-loop calls cannot enter model capture, state or project-save preparation. Valid native processing setup transitions keep their existing cache-continuity policy; the export does not suppress genuine parameter changes.

Focused source validation passed495 existing/targeted UBSan assertions and strict ARM64 production Bridge syntax. This is source evidence only; no updated complete helper/app was built or launched, and native readback/export remains unqualified.
