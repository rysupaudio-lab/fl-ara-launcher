# FL ARA Launcher contributor instructions

- Read README.md and docs/PORTING.md. Authoritative runtime source is native/bridge; old external research/build copies are not the source of truth.
- Headless/API tools are the default. Reuse the existing native export path and host code; avoid another test framework or a broad matrix for each fix.
- Docs/metadata-only changes need link/format review, not DAW launches or binary rebuilds.
- Native audio changes need focused evidence for the changed behavior. Use a short owned project and a meaningful edit; add checks only for relevant failures or uncertainty.
- Keep real DAW/plugin installations, registration, user preferences, projects and active sessions untouched. Use private copies/profiles and close owned test processes normally.
- Preserve genuine ARA lifecycle, source ownership, cache invalidation and saved-state/teardown boundaries. Keep vendor-specific readiness handling scoped to its actual supported image.
- Present the working project clearly. Keep demonstrated support and community reports distinguishable; do not invent compatibility percentages.
- No third-party applications, plug-in/model binaries, licenses, private logs or user audio in commits. Intentional public synthetic demo assets are documented in docs/demo/README.md.
