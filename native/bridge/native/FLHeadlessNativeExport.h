#pragma once
namespace rysup::fl_ara {
// Main tick only, after current model/archive/cache preparation. Readiness is
// supplied by Bridge for ALL expected live ARA owners; never inferred from file.
// One fixed launch-owned request, no commands, arbitrary paths, save or quit.
void pollHeadlessNativeExport(bool currentARAReady) noexcept;
// Explicit process-wide guard for the complete genuine modal export call.
// Bridge must refuse nested model/cache/state/save mutations while true.
bool headlessNativeExportInFlight() noexcept;
}
