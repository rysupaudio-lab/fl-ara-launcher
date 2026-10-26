# Candidate 14 release notes

FL ARA Launcher `0.1.0-candidate.14` fixes source-only and stale processed caches in the tested autochoir1.0.0 VST3 route. FL remains an owned private copy; installed FL/plugin binaries, registration and normal FL preferences stay protected.

## Processing fix

Autochoir generates stems asynchronously. Its renderer can return raw source while they are unavailable, and initial generation can retain defaults that preceded restored component state. This build observes current generation readiness for the exact known executable, renews the same source through public ARA samples-access calls when necessary, and checks the completed generation before and after cache rendering. It uses no private plugin calls, writes or patched plugin instructions. Pending preparation stays distinct from fatal startup errors and does not raise an initial-wait dialog during batch export.

The internal batch route retains FL's genuine `-R -Ewav` export, the established converter RateHold/export driver and normal Quit. Its private profile selects built-in devices and renders at48kHz without requiring or changing the hardware's current sample rate.

## Focused native evidence

Three parameter states of the same saved six-clip fixture were exported through normal FL26.1.7.5421 on arm64 macOS27.0(26A428): baseline, Output Gain−20dB, and maximum Detune. Every run produced276000 frames, Float32 stereo48kHz, applied180000 source frames per insert with zero held frames, and exited normally. The final Developer ID signed bridge was used for both edited states; all materialized Mach-O sections match the baseline bridge before signing.

Output Gain−10→−20dB changed360000 sample values and measured−9.9685dB. Detune.03→.05 changed360000 values, maximum difference0.5842 and RMS difference0.1043. No alignment or fitted gain was used. This establishes meaningful audible parameter changes, without claiming independent processed-reference parity or bit-identical gain-normalized renders.

All11 recorded original app/plugin/registration/preferences pins match, and the original FL deep signature remains valid. Complete hashes, runtime scope and remaining limits are in NATIVE_QUALIFICATION.json. No broad version matrix or GUI test campaign was run.

## Installation and scope

Download the arm64 DMG, open it and drag FL ARA Launcher to Applications. Open a backed-up project through the launcher. Requires licensed original FL26.1.7.5421 on Apple Silicon, macOS15.4+ and separately installed/licensed plugins. This candidate's native evidence is from macOS27.0. The initial source profile remains resident PCM16 mono/stereo48kHz, one ARA effect first per insert, no source stretching/rate conversion or reported plugin latency/tail.

Always reopen/save projects with this integration's state through the launcher. Keep an untouched backup. Other plugin/build/editor operations are not covered by the autochoir gain/Detune checks. Melodyne editing/cold recall remain unqualified. This is a prerelease, not universal ARA conformance or vendor certification.

Package with the preserved, exact tested nested runtime signatures; sign/notarize/staple the outer app and DMG. Publish only our app, checksums and qualification/packaging metadata. No vendor applications, plugins, models, license data, private logs or test audio are distributed.
