# M1 normal-app preparation — 2026-10-24

No FL runtime has been launched by this preparation.

The staged reference is:
`/Users/jordanrys/Documents/Rys Up Audio/Development Resources/Research/ARA App Integration/2026-10-24/references/FL Studio 2026.app`.

Read-only checks passed on M1: both exact executable/engine hashes, deep strict
vendor signature verification, and the exact Image-Line signing requirement.
The source and `~/Library/Caches` report filesystem device16777229, so the private
cache can use same-filesystem APFS cloning. M1 had14GiB free during preflight.
macOS26.2 satisfies the launcher minimum15.4. Active user Logic PID29117 was
observed read-only and was never attached to, hidden, signaled, or modified.
No OsxFL process was listed in that bounded inventory.

A native compile-check executable was transferred to:
`.../2026-10-24/tools/fl-launcher/FL-ARA-Launcher-compilecheck`.
Its `--check --fl <staged-reference>` passed on M1. It has no bundled runtime
helper and must not be used for an actual FL launch.

The owned private cache has been prepared with0700 permissions at:
`.../2026-10-24/runtime/fl-launcher-cache`.

After the integration owner builds/transfers the actual sealed app with the R2
helper, this is the exact blank-project test command on M1 (no `open`, `--args`
or `--` routing):

```sh
'/Users/jordanrys/Documents/Rys Up Audio/Development Resources/Research/ARA App Integration/2026-10-24/bare-startup-candidate/FL ARA Launcher.app/Contents/MacOS/FL ARA Launcher' \
  --fl '/Users/jordanrys/Documents/Rys Up Audio/Development Resources/Research/ARA App Integration/2026-10-24/references/FL Studio 2026.app' \
  --cache-root '/Users/jordanrys/Documents/Rys Up Audio/Development Resources/Research/ARA App Integration/2026-10-24/runtime/fl-launcher-cache' \
  --test-device 'Pro Tools Audio Bridge 2-A_UID'
```

Append exactly one absolute backed-up `.flp` filename to request a project. The
sealed current-engine static parser accepts that argument; actual opened project
and graph must still be verified at runtime. The launcher executable stays alive
until the spawned child exits; leave its SSH/terminal process running.

## Authorization continuity

Only branch presence/counts were checked in original registry files. The original
M1 registry contains a Registrations branch with26 values; local has28. Neither
has an exact FL Studio26 subbranch. No registration values or hashes were printed,
stored, copied, or sent. Presence alone does not establish the vendor's entitlement
decision; older registration records may remain legitimate under lifetime updates.

The original registry remains readable under the child sandbox, but the private
home intentionally has no license data. A normal vendor read-through route for
registration-only queries remains under static investigation. Do not infer
licensed mode or saving support from the reference/copy signature checks. Any
future route must return existing authorized registration reads in memory,
leave the vendor's validation unchanged, prevent original writes, and keep secret
values out of the private registry, files, environment and logs.

## Actual R2 bare-startup candidate staging

The owner-local app was built and transferred to
`.../2026-10-24/bare-startup-candidate/FL ARA Launcher.app`. M1 deep strict
signature, version and packaged-helper hash checks passed. This preparation did
not launch it; the root integration owner controls the runtime test. The packaged
helper SHA256 is `1fc01ae32ca4ecd2ec3d3c4aa7b1cc541151b7c181c958ee75229ad4406f9c37`
(ad-hoc signature metadata differs from the unchanged research helper c3b404…).
The test admission is enforced: exact virtualUID, blank startup only, no project.
Every runtime and third-party qualification flag remains false.

Local DMG/ZIP/receipt:
`/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/fl-launcher-build/r2-bare-packages/`.
The original research helper manifest's `runtime_ready:false` remains unchanged.
