# Bring the ARA host to another DAW

This repository ships one FL adapter and reusable ARA hosting code. A new DAW needs its own integration with that DAW's plug-in lifecycle, clip audio, transport and project state. Changing a DAW name or binary hash is not a port.

## Start with the existing layers

| Responsibility | Source |
|---|---|
| ARA factory/document, source readers, graph and archive operations | [GenericARAHost](../native/bridge/host/GenericARAHost.h) |
| Bind the actual loaded VST3 component to ARA | [VST3ARAEntry](../native/bridge/host/VST3ARAEntry.h) |
| Render through an additional same-factory ARA playback component | [OwnedVST3PlaybackCompanion](../native/bridge/host/OwnedVST3PlaybackCompanion.h) |
| Native-owner hold, model snapshots, rebuilds and saved state | [StandaloneARACoordinator](../native/bridge/host/StandaloneARACoordinator.h) |
| FL-specific lifecycle hooks, native data access and source overlay | [FL bridge directory](../native/bridge/native/) |
| Private-copy launcher and normal/background export lifecycle | [Launcher.swift](../native/Launcher.swift) |

The current implementation is macOS/arm64. Another platform also needs platform-specific launcher, memory-access and lifecycle work. The source's FL-specific class names and model/state containers must be adapted to the new host.

## A practical port

1. **Find the DAW's existing headless/API route.** Reuse its native project load/export/save tools. Keep a small owned fixture and the real application/project separate.
2. **Integrate the genuine plug-in lifecycle.** Capture the actual loaded factory/component and module lifetime; bind after original initialization and before state, activation or processing. Preserve the DAW's normal ownership and teardown.
3. **Supply actual clip data.** Map tracks, sources, modifications, regions, offsets and transport into the existing graph/source interfaces. `StandaloneOwnerAdapter` supplies `verifyActualNativeHold`, `copyActualCurrentRAMAndClock`, invalidation and queued rebuilds.
4. **Connect processed audio and project state.** Use the existing playback companion/cache path and the new DAW's audio boundary. Preserve its ordinary plug-in state alongside genuine ARA archives, with consistent persistent identities.
5. **Handle readiness.** Use public ARA analysis/content APIs where provided. The current host polls advertised analysis status; add explicit analysis requests when the target requires them. `WholeSourceCacheOptions::requireCurrentGeneration` supports an owner-specific asynchronous-generation adapter. The autochoir adapter is an exact-image workaround in the FL bridge, not a general rule for other vendors.
6. **Demonstrate one meaningful edit.** Reuse the native headless exporter for a short before/after render with a control whose behavior is understood. Compare the actual output. Expand checks only when a changed feature or failure calls for them.

Source saves, callback admission and teardown need the real owner boundary; a UI pause indicator alone cannot supply it. Existing [implementation contracts](../native/bridge/host/OWNER_NEUTRAL_HOST_CONTRACT.md) describe those invariants.

## Build the existing FL bridge

External ARA/VST3 SDK roots and Xcode command-line tools are required for source builds. The downloadable app includes its runtime and needs no developer tools.

```sh
./build.sh bridge --ara-sdk /path/to/ARA_SDK --vst3-sdk /path/to/VST3_SDK --output /path/to/helper-build
```

The builder emits an unqualified helper receipt. Follow the existing signing/admission and packaging flow for your actual candidate; a successful compile alone does not demonstrate audio integration. See [the current release notes](CANDIDATE_14_RELEASE.md) and [source layout](SOURCE_CONSOLIDATION.md).

To contribute a host, open a [host proposal](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=host-request.yml) with the DAW/build/platform and the native interfaces or export route you plan to use. Small, focused contributions are welcome.
