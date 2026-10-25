# Cache and native process admission

Internal cache authority maintenance closes overlay authority and queues a stop
without declaring a new model edit. A real edit additionally reports its revision
and requests rebuilding. The owner coalesces rebuild requests through its dirty
bit; an edit arriving during a rebuild must remain requested. Authority updates
return the final current ticket after any owner-side retirement invalidation.

Native processing permission, generation and active-call count share one
lock-free atomic word. Rejected entrants never change the active count. A
successful entry owns a move-only lease whose release drains exactly that call.
Closing the gate retains existing admitted calls until their leases release;
permission changes advance the generation. An entrant cannot retry into a later
generation after close/drain/reopen. Audio-thread admission makes a fixed number
of attempts, and exhaustion closes the record permanently.

These compiled policies support cache and process-admission qualification.
Offline controls do not establish actual FL cache stability, editor behavior,
native Save, third-party processing or audio. The signed candidate retains false
runtime and audio qualification flags until exact-binary native tests supply
that evidence. The preceding device-gate and output-sink implementations remain
unchanged in this candidate.
