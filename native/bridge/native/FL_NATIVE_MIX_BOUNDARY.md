# Actual FL outer mix-lock boundary

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


Call `observeActualNativeMixBoundary()` on the owning normal-app main thread,
after our bounded native copy hold has been released and our own render/source
leases have drained, immediately before ARA analysis/model/archive/cache work.
Proceed only if `result.available()`; otherwise defer and retry a later native
model boundary. Log its reason and return codes; contention is not permission
to disable protection or call the SDK under the native lock.

The helper validates admitted current engine/instance and stopped raw transport0,
actual TCriticalSection VMT17187e0/size48, its actual enter/leave methods and
current FPC thread manager functions. It then starts one owned worker, which
uses a **nonblocking** pthread_mutex_trylock on the actual native mutex, unlocks
immediately if successful, and exits. The main thread joins this owned worker.
No native DAW method, SDK callback, media, file, hardware or GUI call occurs on
the worker. Unknown join lifetime retains its tiny work record and fails closed.

Why this addresses the recursive outer-lock problem: a trylock issued by the
same main thread could succeed recursively. A distinct worker gets EBUSY when
the main thread holds even one outer level. The meaningful regression test
checks main depth1, main depth2, releasing only the inner level, and releasing
the final level. Free cases also verify the worker released the lock.

There is no assumption about private Darwin pthread owner/count fields. Exact
sealed native code proves TCriticalSection+8 is passed unchanged through FPC
enter38330/try38350/leave38380 to pthread_mutex_lock/trylock/unlock. Native
initializer382a0 sets recursive mutex type2. Runtime admission verifies actual
FPC dispatch pointers at engine21ab608+88/+90/+98 before using the mutex.

Success proves main did not hold this recursive FL lock **at the probe instant**.
It does not reserve the native mutex across later SDK work, and does not claim
that another native thread cannot subsequently acquire it. Keep main-thread
native mutation callbacks excluded; revalidate graph generations independently.
All actual model/archive entrypoints, including synchronous setup priming, must
use this guard. A successful stopped timer from an earlier turn is not a lease.
