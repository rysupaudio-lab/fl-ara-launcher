#pragma once
namespace rysup::fl_ara {
// Compatibility entry for the existing main tick. In genuine batch mode this
// only observes the launch-owned request; it never calls native export or waits
// for readiness. Normal native CLI arguments drive all loading/rendering.
void pollHeadlessNativeExport(bool unusedCurrentARAReady) noexcept;
// Genuine native batch never enters an own export scope. Setup-time synchronous
// priming remains available during the native batch's real setup callbacks.
bool headlessNativeExportInFlight() noexcept;
// Existing NativeApplication creationReturned boundary, after the actual normal
// main loop returns. Output verification is the launcher's job after child exit.
void observeHeadlessNativeBatchReturn(int nativeResult) noexcept;
}
