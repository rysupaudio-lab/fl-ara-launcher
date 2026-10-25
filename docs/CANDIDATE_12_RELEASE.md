# Candidate 12 release preparation

Prepared source version: `0.1.0-candidate.12`. This document records the packaging path; it does not claim that unpublished artifacts or pending runtime behavior are qualified.

## Download description

> FL ARA Launcher is an experimental ARA 2 integration for the original Image-Line-signed FL Studio 2026 26.1.7.5421 on Apple Silicon Macs. Download the DMG, drag the launcher to Applications and open a backed-up project through it. Your installed FL application is unchanged. Requires your own licensed FL Studio and separately installed licensed ARA plugins. Reopen projects containing this integration’s ARA state through the launcher.

The app targets macOS 15.4+, but native evidence currently comes from macOS 27.0 (26A428). Do not describe every macOS version as tested. Initial audio scope is 48 kHz resident PCM16 mono/stereo, one ARA effect first in each mixer insert, no source stretching/rate conversion or plugin latency/tail. No Intel, Windows, other FL build, universal plugin or ARA conformance claim.

## Exact runtime result to attach before publication

The candidate 12 release body must identify the actual saved-project/headless-export result and any remaining limitation. C11 native Save produced two RYSFLAPP envelopes with saved +5 Formant state, but its normal export found queued parameter echoes invalidating prepared caches; candidate 12 addresses that behavior. Do not substitute C11’s failed export or the FL AU/Symbiosis renderer for a completed candidate 12 normal-app result. Reuse the converter code gate and existing envelope/PCM readers; no extra test campaign is required here.

## Packaging and notarization

Existing builders: `tools/build_app.py`, `tools/package_app.py`, `tools/bundle_manifest.py`; wrapper `build.sh`. Pass explicit candidate version/stem because the packager defaults to candidate 1. The Swift version and helper source revision must match the committed candidate 12 source.

```sh
./build.sh --helper HELPER --helper-manifest MANIFEST --output BUILD
/opt/homebrew/bin/python3.13 tools/package_app.py \
  --app 'BUILD/FL ARA Launcher.app' --output PACKAGES \
  --identity 'Developer ID Application: …' \
  --version 0.1.0-candidate.12 --stem FL-ARA-Launcher-0.1.0-candidate.12-arm64
xcrun notarytool submit PACKAGES/FL-ARA-Launcher-0.1.0-candidate.12-arm64.dmg \
  --keychain-profile RysUpNotary --wait --output-format json
xcrun stapler staple PACKAGES/FL-ARA-Launcher-0.1.0-candidate.12-arm64.dmg
xcrun stapler validate PACKAGES/FL-ARA-Launcher-0.1.0-candidate.12-arm64.dmg
```

`RysUpNotary` availability was confirmed read-only on 2026-10-25. Do not copy credentials into the repo or release. Preserve the accepted notary receipt and recompute final asset hashes after stapling. ZIP distribution requires its own accepted/stapled app path; do not label an earlier unstapled ZIP as identical to the final DMG contents.

Use a public GitHub prerelease for this bounded experimental profile. Publish the DMG and final SHA256 checksums with source, MIT/SDK notices and exact compatibility notes. Read back the uploaded asset size/hash. Do not ship vendor apps, installed plugin binaries, license/registry data, personal paths, local native logs or test audio. No Shopify/commercial-plugin launch is implied.

## Current publication availability

At preparation time the local repository had no remote and `rysupaudio-lab/fl-ara-launcher` did not resolve through the authenticated GitHub CLI. Repository creation, source push, tagging, asset upload and public readback remain owner-run release steps after the runtime result. No build, signing, publication or native operation was performed by this documentation pass.
