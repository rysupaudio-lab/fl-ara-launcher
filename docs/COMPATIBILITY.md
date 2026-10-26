# Compatibility and current scope

The public beta contains a working native FL/ARA audio path. Compatibility reports are welcome for additional plug-ins; the observed cases below are kept separate from broader compatibility goals.

| Requirement | Current release |
|---|---|
| Mac | Apple Silicon / arm64 |
| FL Studio | Original Image-Line-signed 26.1.7.5421; other builds are refused |
| macOS | Deployment minimum 15.4; native evidence from 27.0 (26A428) |
| Source audio | Resident PCM16, 48 kHz, mono/stereo |
| Placement | One ARA effect per insert, first in its effect chain |
| Timing | No source stretching/rate conversion or reported plug-in latency/tail in this route |

## Plug-ins

Autochoir 1.0.0 VST3 produced processed audio and meaningful gain/Detune edits in native FL exports. Its asynchronous-generation adapter applies to executable SHA256 `390c549fa3b3e546d3ee4c9a7041dab0149e9f0944eddce403c0abce6783e43d`. The shared host remains available to other ARA VST3 plug-ins; that exact adapter is not applied to unknown binaries.

Melodyne 5.4.2 VST3 previously bound, exported a default project and saved. Its editing and cold recall remain unconfirmed. Every editor gesture, automation operation, plug-in build and DAW version is not covered by the current examples. There is no universal ARA/vendor certification claim.

[Full measurements and hashes](NATIVE_QUALIFICATION.json) · [Report compatibility](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=plugin-compatibility.yml)

## Projects and playback

Reopen/save projects containing this integration's ARA state through the launcher. Unpatched FL may discard extended plug-in state on save. Keep an untouched backup. Caches prepare while stopped; a playback-time edit can stop playback and require an explicit restart when preparation finishes.

## Private-copy storage

Same-volume APFS cloning is the normal route, with a 5 GiB free-space reserve. The existing full-copy fallback is restricted to an explicitly selected mounted Razer SSD; arbitrary external-volume support is a contribution opportunity. Long private-copy paths are refused because of this FL build's template reader limit. Use a short real cache path. A launcher-only `privateCacheRoot` preference remembers an owner-selected location while its external volume is mounted; `RYSUP_FLARA_CACHE_ROOT` and `--cache-root` provide advanced overrides.

The standard internal diagnostics location is `~/Library/Caches/FLARALauncher/runs`; a selected cache location stores its own runs. Existing DAW sessions and installed applications remain separate.
