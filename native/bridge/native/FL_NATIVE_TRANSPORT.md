# Native normal-FL transport helper contract

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


`FLNativeTransport.h/.cpp` implements real native operations as helper APIs.
It does not install or advertise an ARA PlaybackController interface: loop/cycle
control is not source/runtime-qualified, and stopped seek still needs actual
normal-app native-position/audio readback. No success-shaped no-op is supplied.

Root owns serialization and must defer plug-in/ARA requests onto a subsequent
main-thread boundary outside ARA, native processing and native state callbacks.
Never invoke a native request inline from an ARA callback; that can reenter the
native graph or processing lifecycle. Root must first drain its own active SDK,
source-read and render leases where needed.

`requestNativePlayingState(insert, playing)` revalidates the actual native
component/owner/context/Tag chain. It uses the existing genuine public FL host
Dispatcher FHD_Transport79 route with Play10/value2 or Stop11/value1 and checks
actual current native transport state. A request already in the requested state
is distinguished from an actual native request. A delayed/unverified state is
refused, never reported completed. Raw2 is not treated as a stopped boundary.

`requestNativePlaybackPositionWhileStopped(insert, seconds)`:

1. Requires actual own main thread, valid native owner, finite nonnegative
   seconds, actual raw native transport0 and an available outer-lock boundary.
2. Resolves actual global engine1b9fa38 as TCore, verifies exact VMT14cbe90,
   then takes that genuine object's current service receiver from +8. Native
   public `setSongPos` callback81ce50 invokes exactly receiver.vtable+160 as its
   permission check. The helper invokes that same original current native
   method and honors false. It does not patch or substitute vendor permission.
3. Revalidates native identity and repeats the outer-lock probe after permission.
4. Calls actual native milliseconds-to-ticks7d31b0 with seconds*1000, then actual
   native seek325d60 with flags0 and that double tick value. This preserves native
   clock math instead of substituting a constant BPM formula. Nonfinite, negative
   or overflowing native coordinates are refused before mutation.
5. Revalidates the native owner/current transport/permission receiver after the
   original native void seek returns. Receipt is **NativeSeekReturned**, with
   `nativePositionReadbackVerified=false`. That is dispatch/return evidence only;
   the normal-app runtime must establish the actual new native position/audio.

The native permission receiver's class is left unnamed; its provenance is the
actual sealed TCore service slot and the exact original public callback path.
Its table and method must both resolve inside the same admitted engine image.
No approximate source/owner matching or arbitrary request receiver is accepted.
