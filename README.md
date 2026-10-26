# ARA 2 for FL Studio

**Give ARA plug-ins direct access to your FL Studio audio clips.**

FL ARA Launcher is a free, open-source Mac app that runs your licensed FL Studio in a private copy. Your installed FL and plug-ins stay intact.

**[Download for Mac →](https://github.com/rysupaudio-lab/fl-ara-launcher/releases/download/v0.1.0-candidate.14/FL-ARA-Launcher-0.1.0-candidate.14-arm64.dmg)** · [Release notes](https://github.com/rysupaudio-lab/fl-ara-launcher/releases/tag/v0.1.0-candidate.14) · [Report a plug-in](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=plugin-compatibility.yml)

**Public beta · Signed and notarized · MIT license**

Current installer: **Apple Silicon · FL Studio 26.1.7.5421 · macOS 15.4+**

## Get started

1. Download and open the DMG. Put **FL ARA Launcher** in Applications.
2. Open it and choose **Open a backed-up project…**. Select your licensed FL application if prompted.
3. Work in the private FL window. Keep the launcher open, save normally, and quit that FL window when finished.

No Terminal, Python, developer tools or Rys Up account is required. Install and license your ARA plug-ins separately.

Start with **48 kHz, 16-bit mono/stereo audio** and place **one ARA effect first in its mixer insert**. Preparation happens while stopped; after an edit finishes preparing, restart playback if it was stopped.

**Reopen and save projects containing these ARA edits through FL ARA Launcher.** Keep an untouched backup: ordinary FL does not understand the extended ARA state and can discard it on save.

## Hear an actual edit

Autochoir 1.0.0 VST3 processed audio inside normal FL Studio. Its gain edit changed the exported level by about **10 dB**. Its Detune edit changed **360,000 sample values**, with a substantial audio difference.

**[Listen before the Detune edit](https://github.com/rysupaudio-lab/fl-ara-launcher/blob/main/docs/demo/detune-before.mp3)** · **[Listen after](https://github.com/rysupaudio-lab/fl-ara-launcher/blob/main/docs/demo/detune-after.mp3)**

These short examples use a **synthetic test clip**. They are compressed copies of actual native FL exports, with the same output gain and no normalization. [Audio provenance](docs/demo/README.md) · [Full results](docs/NATIVE_QUALIFICATION.json)

## Bring your ARA plug-in

The project contains a shared ARA host layer and a native FL integration. Wider plug-in compatibility is a central goal, and community reports help decide what to improve next.

| Plug-in | Current evidence |
|---|---|
| autochoir 1.0.0 VST3 | Processed audio, audible gain/Detune edits and saved-state exports demonstrated |
| Melodyne 5.4.2 VST3 | Loading, default export and saving observed; edited audio and cold recall still need confirmation |
| Other ARA VST3 plug-ins | Try your plug-in and [share what worked or broke](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=plugin-compatibility.yml) |

We publish the working integration now and expand compatibility in the open. The current evidence does not establish a percentage of all ARA plug-ins supported. See [compatibility details](docs/COMPATIBILITY.md) for the exact host, audio and plug-in scope.

## Want ARA in another DAW?

**Logic Pro:** [Logic ARA Launcher](https://github.com/rysupaudio-lab/logic-ara-launcher) has its own download and requirements.

**Developers:** use [the porting guide](docs/PORTING.md) to find the shared ARA host, source readers, state handling and native integration boundaries. Each DAW needs its own adapter. Contributions that add another host or improve plug-in compatibility are welcome.

[Contribute](CONTRIBUTING.md) · [Request a DAW](https://github.com/rysupaudio-lab/fl-ara-launcher/issues/new?template=host-request.yml)

## Your original install stays intact

The launcher checks your original FL signature/build, creates a private app and preferences, and protects installed DAW/plug-in files and normal FL data from writes. It reads legitimate authorization without distributing license data. Other installed DAW sessions are left alone.

If a session is interrupted, its private copy is preserved so you can save your work. After it exits, **Clean finished session** removes the finished copy. To uninstall, quit the private session and remove only **FL ARA Launcher.app**.

## Build from source

Build tools require Apple Silicon, Python and Xcode command-line tools; the installed launcher does not.

```sh
./build.sh --compile-only
'dist/FL-ARA-Launcher-compilecheck' --self-test
'dist/FL-ARA-Launcher-compilecheck' --check
```

Build the app with the exact helper and its matching manifest:

```sh
./build.sh --helper /path/FLAppBridge.dylib --helper-manifest /path/helper.json --output /path/build
/opt/homebrew/bin/python3.13 tools/package_app.py \
  --app '/path/build/FL ARA Launcher.app' --output /path/packages \
  --identity 'Developer ID Application: …' \
  --version 0.1.0-candidate.14 --stem FL-ARA-Launcher-0.1.0-candidate.14-arm64
```

The helper must be signed and hash-bound to a ready build manifest. Signing, notarization, runtime evidence and publication are separately recorded. See [candidate release preparation](docs/CANDIDATE_14_RELEASE.md).

The internal CLI accepts `--headless-export BACKED_UP.flp` for a disposable, read-only-source batch run. It uses FL’s real `-R` batch renderer, prepares ARA after the genuine project loader returns, and reuses the sealed converter RateHold and export driver from the control pipeline. Output and private diagnostics stay in the owned run. The existing driver waits for the output and quits the private FL copy normally; it can answer discard prompts for that disposable batch only. No computer-use gestures are needed. The normal launcher window/project-saving workflow above is unchanged.

## License

MIT for the launcher/integration code. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled SDK notices. Independent of Image-Line and plugin vendors.
