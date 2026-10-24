# Third-party notices

The native helper uses public ARA API and ARA Library interfaces and links the
ARAHostDispatch and ARADebug library implementations:

- ARA SDK API and ARA SDK Library, copyright Celemony Software GmbH.
- ARAHostDispatch was originally written and contributed by PreSonus Software Ltd.
  and developed in cooperation with PreSonus Software Ltd.
- These ARA components are licensed under Apache License2.0. Exact license texts
  from the supplied API and Library distributions are included in
  `third_party_licenses/ARA_API-Apache-2.0.txt` and
  `third_party_licenses/ARA_Library-Apache-2.0.txt`.

The helper uses public VST3 pluginterfaces headers and links the `funknown.cpp`
and `coreiids.cpp` interface implementations, copyright2025 Steinberg Media
Technologies GmbH, under the MIT license present in the actual supplied
`pluginterfaces/LICENSE.txt`. Its exact text is included in
`third_party_licenses/VST3-pluginterfaces-MIT.txt`.

Our helper implementation is separate from these SDK sources. No SDK source tree,
SDK documentation bundle, example plugin, vendor DAW binary, third-party plugin,
license key, activation record, customer audio or test-project audio is included
in the application distribution. SDK roots are explicit build-time inputs.
Third-party notices and the applicable license texts accompany the linked helper
in `Contents/Resources/ThirdPartyLicenses` and `THIRD_PARTY_NOTICES.md`.

ARA, FL Studio, Image-Line, VST3, Steinberg and third-party plugin names belong to
their respective owners. This independent experimental launcher does not imply
affiliation or endorsement, and requires separately obtained legitimate DAW and
plugin licenses.
