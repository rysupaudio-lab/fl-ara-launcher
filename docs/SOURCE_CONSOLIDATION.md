# Source consolidation after integration freeze

The integration owner must freeze the normal-app helper before consolidation.
No helper files have been copied out of the ongoing mutable research tree.

The durable layout will be `native/bridge/host`, `native/bridge/native`, and
`native/bridge/legacy/FLNativeModel.{h,cpp}`. Only directly enumerated C++/ObjC++
source/header files and the helper builder are eligible. Build outputs, binaries,
SDK trees, projects, audio, credentials and registry files are excluded.

`tools/stage_bridge_source.py` defaults to planning only. After freeze, create and
review the exact plan, then apply that unchanged plan. All source hashes are
rechecked before writes and again before publishing the staged directory. Any
changed input refuses; existing durable sources cannot be overwritten by this
bootstrap tool. The only transformations are the reviewed native-model include
and the builder's repository-relative legacy default. Both resulting hashes are
included in the plan. Source provenance accompanies the consolidated source.

```sh
python3 tools/stage_bridge_source.py --source-root /frozen/fl --legacy-host /frozen/legacy-host > /reviewed/plan.json
python3 tools/stage_bridge_source.py --source-root /frozen/fl --legacy-host /frozen/legacy-host --apply /reviewed/plan.json
./build.sh bridge --ara-sdk /external/ARA_SDK --vst3-sdk /external/VST3_SDK --output /external/helper-build
```

The helper front door refuses any mutable research-source fallback. SDK roots are
external build inputs, with no SDK tree redistribution. Public ARA API/Library
Apache2.0 and actual VST3 pluginterfaces MIT texts are included separately in the
launcher source and app resources. Helper build receipts remain unqualified until
the integration owner records exact binary runtime evidence.
