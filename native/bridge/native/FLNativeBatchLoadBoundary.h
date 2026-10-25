#pragma once
namespace rysup::fl_ara {
using NativeBatchPrime=bool(*)(void*) noexcept;
// Main thread, after owned engine admission, before original Create. Inactive
// outside the launch-owned real CLI batch mode. Never patches native code.
bool configureNativeBatchLoadBoundary(NativeBatchPrime,void*) noexcept;
// Existing native initialize/state callbacks only. Finds the exact active
// batch-loader frame and replaces its raw saved LR DATA once. False can simply
// mean this callback is not inside that batch loader; no GUI path is armed.
bool armNativeBatchLoadBoundaryAtCurrentCallback() noexcept;
// After original Create returns. Clear own metadata ONLY; never restore a dead,
// abandoned or reused stack slot. This does not quit or manipulate the app.
void finishNativeBatchLoadBoundary() noexcept;
}
// Scope: exact arm64 raw-LR loader's ordinary RET only. No claim of C++/DWARF
// unwinding through the redirected return edge. FPC nonlocal error exits may
// abandon the slot; finish/next arm only forget metadata, never repair that RAM.
