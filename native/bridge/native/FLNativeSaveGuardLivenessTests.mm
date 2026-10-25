#import <Foundation/Foundation.h>
#include "FLNativeSaveGuard.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
#error This own-file test requires RYSUP_FL_SAVE_GUARD_TESTING.
#endif

using namespace rysup::fl_ara;
namespace testing = rysup::fl_ara::save_guard_testing;
namespace {
std::string root;
unsigned assertions = 0;
[[noreturn]] void fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL line %d: %s (errno=%d: %s)\nEvidence: %s\n",
                 line, expression, errno, std::strerror(errno), root.c_str());
    std::exit(1);
}
#define CHECK(expression) do { ++assertions; if (!(expression)) fail(#expression, __LINE__); } while (false)

struct Owner {
    std::atomic<bool> live{false};
    bool ready = false, callbacksOK = true;
    unsigned preflights = 0, precommits = 0, rejections = 0, finishes = 0;
    unsigned freezes = 0, releases = 0;
    int cookie = 0;
    // Deliberately atomic-only: no SDK, file, allocation, locks, or live owner map.
    static bool hasLiveARA(void* context) noexcept {
        return static_cast<Owner*>(context)->live.load(std::memory_order_acquire);
    }
    static bool preflight(const NativeSaveRequest&, void** cookieOut, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.preflights;
        *cookieOut = nullptr;
        if (!self.ready) return false;
        if (self.freezes != self.releases) self.callbacksOK = false;
        ++self.freezes;
        *cookieOut = &self.cookie;
        return true;
    }
    static bool precommit(const NativeSaveRequest&, void* cookieIn, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.precommits;
        if (cookieIn != &self.cookie || self.freezes != self.releases + 1)
            self.callbacksOK = false;
        return self.ready;
    }
    static void rejected(const NativeSaveRequest&, NativeSaveRejection reason, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.rejections;
        if (reason == NativeSaveRejection::None) self.callbacksOK = false;
    }
    static void finished(const NativeSaveTransaction& transaction, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.finishes;
        if (transaction.ownerCookie) {
            if (transaction.ownerCookie != &self.cookie || self.freezes != self.releases + 1)
                self.callbacksOK = false;
            ++self.releases;
        }
    }
    NativeSaveGuardCallbacks callbacks() {
        NativeSaveGuardCallbacks result{};
        result.preflight = preflight;
        result.precommit = precommit;
        result.rejected = rejected;
        result.finished = finished;
        result.context = this;
        result.hasLiveARA = hasLiveARA;
        return result;
    }
};
Owner owner;
std::string path(const char* name) { return root + "/" + name; }
NSString* ns(const std::string& value) { return [NSString stringWithUTF8String:value.c_str()]; }
void writeFD(int fd, const std::string& bytes) {
    CHECK(fd >= 0);
    std::size_t position = 0;
    while (position < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + position, bytes.size() - position);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        position += static_cast<std::size_t>(count);
    }
    CHECK(::close(fd) == 0);
}
void seed(const std::string& filename, const std::string& bytes) {
    CHECK(currentNativeSaveTransaction() == nullptr);
    writeFD(::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600), bytes);
}
std::string read(const std::string& filename) {
    const int fd = ::open(filename.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    std::string result;
    char bytes[1024];
    while (true) {
        const ssize_t count = ::read(fd, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count >= 0);
        if (count == 0) break;
        result.append(bytes, static_cast<std::size_t>(count));
    }
    CHECK(::close(fd) == 0);
    return result;
}
bool exists(const std::string& filename) {
    struct stat metadata{};
    return ::lstat(filename.c_str(), &metadata) == 0;
}
NativeSaveRequest frame(const std::string& target, const std::string& temporary) {
    static std::uintptr_t sequence = 0x200000;
    NativeSaveRequest request;
    request.actualSaveFrame = (sequence += 0x1000);
    request.targetPath = target;
    request.actualOpenPath = temporary;
    request.openFlags = O_RDWR | O_CREAT | O_TRUNC;
    request.actualSaveType = 0;
    return request;
}
void liveFromBackground(bool live) {
    std::thread worker([live] { owner.live.store(live, std::memory_order_release); });
    worker.join();
    CHECK(owner.live.load(std::memory_order_acquire) == live);
}
void start(const NativeSaveRequest& request) {
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(testing::setFrame(request));
}
void endWithoutTransaction(unsigned finishes) {
    CHECK(currentNativeSaveTransaction() == nullptr);
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == finishes);
    testing::clearFrame();
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == finishes);
    CHECK(owner.freezes == owner.releases && owner.callbacksOK);
}
void endTransaction(unsigned finishes) {
    CHECK(currentNativeSaveTransaction() != nullptr);
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == finishes);
    testing::clearFrame();
    CHECK(owner.finishes == finishes);
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == finishes + 1);
    CHECK(owner.freezes == owner.releases && owner.callbacksOK);
    CHECK(currentNativeSaveTransaction() == nullptr);
}
void refusedMove(const std::string& source, const std::string& destination) {
    NSError* error = nil;
    CHECK(![[NSFileManager defaultManager] moveItemAtPath:ns(source)
                                                 toPath:ns(destination) error:&error]);
    CHECK(error != nil && [[error domain] isEqualToString:NSPOSIXErrorDomain]);
    CHECK([error code] == EBUSY);
}
void allowedMove(const std::string& source, const std::string& destination) {
    NSError* error = nil;
    CHECK([[NSFileManager defaultManager] moveItemAtPath:ns(source)
                                                toPath:ns(destination) error:&error]);
    CHECK(error == nil);
}
void staleFalseCannotOverrideFreshTrue() {
    const auto target = path("stale-false.flp"), temporary = path("stale-false.tmp");
    seed(target, "stale-false-original");
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    owner.ready = false;
    liveFromBackground(true);
    setNativeSaveGuardHasLiveARA(false); // A stale main-thread cached observation.
    const auto request = frame(target, temporary);
    start(request);
    errno = 0;
    CHECK(testing::open(temporary.c_str(), request.openFlags) == -1);
    CHECK(errno == EBUSY);
    CHECK(owner.preflights == p + 1 && owner.freezes == z);
    CHECK(!exists(temporary));
    CHECK(read(target) == "stale-false-original");
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::OwnerNotReady);
    endTransaction(f);
    owner.live.store(false, std::memory_order_release);
    std::puts("PASS fresh background live=true overrides stale cached false before creation");
}
void liveAppearsBeforeCommit(bool foundationFirst) {
    const auto target = path(foundationFirst ? "appears-move.flp" : "appears-unlink.flp");
    const auto temporary = path(foundationFirst ? "appears-move.tmp" : "appears-unlink.tmp");
    const auto backup = path("appears-move.bak");
    seed(target, "appears-original");
    const unsigned p = owner.preflights, f = owner.finishes, r = owner.rejections;
    owner.live.store(false, std::memory_order_release);
    setNativeSaveGuardHasLiveARA(false);
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "unprepared-temporary");
    CHECK(owner.preflights == p);
    CHECK(currentNativeSaveTransaction() == nullptr);
    liveFromBackground(true); // Deterministically after creation, before destructive effect.
    if (foundationFirst) refusedMove(target, backup);
    else {
        errno = 0;
        CHECK(testing::unlink(target.c_str()) == -1);
        CHECK(errno == EBUSY);
    }
    CHECK(owner.rejections > r);
    CHECK(owner.preflights == p && owner.precommits == 0);
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(read(target) == "appears-original");
    CHECK(read(temporary) == "unprepared-temporary");
    CHECK(!exists(backup));
    refusedMove(temporary, target);
    endWithoutTransaction(f);
    owner.live.store(false, std::memory_order_release);
    std::puts(foundationFirst ? "PASS fresh live at first Foundation backup move vetoes without transaction" :
                               "PASS fresh live at first unlink vetoes without transaction");
}
void pendingSurvivesFreshFalse() {
    const auto target = path("pending-false.flp"), temporary = path("pending-false.tmp");
    const auto backup = path("pending-false.bak");
    seed(target, "pending-original");
    owner.ready = true;
    owner.live.store(true, std::memory_order_release);
    setNativeSaveGuardHasLiveARA(false);
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "pending-prepared");
    CHECK(owner.preflights == p + 1 && owner.freezes == z + 1);
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->openAllowed);
    liveFromBackground(false);
    CHECK(markCurrentNativeSaveFailed());
    errno = 0;
    CHECK(testing::unlink(target.c_str()) == -1);
    CHECK(errno == EBUSY);
    refusedMove(target, backup);
    refusedMove(temporary, target);
    CHECK(read(target) == "pending-original");
    CHECK(read(temporary) == "pending-prepared");
    CHECK(!exists(backup));
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(!currentNativeSaveTransaction()->commitBegun);
    CHECK(owner.precommits == 0);
    endTransaction(f);
    owner.ready = false;
    std::puts("PASS pending transaction remains guarded after fresh owner predicate becomes false");
}
void freshFalseIgnoresStaleTrue() {
    const auto target = path("stale-true.flp"), temporary = path("stale-true.tmp");
    seed(target, "stale-true-original");
    owner.ready = false; // Would refuse if an obsolete cached bit were consulted.
    owner.live.store(false, std::memory_order_release);
    setNativeSaveGuardHasLiveARA(true);
    const unsigned p = owner.preflights, f = owner.finishes, r = owner.rejections;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "ordinary-new-project");
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(testing::unlink(target.c_str()) == 0);
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->ordinaryCommit);
    CHECK(currentNativeSaveTransaction()->commitBegun);
    CHECK(currentNativeSaveTransaction()->ownerCookie == nullptr);
    allowedMove(temporary, target);
    CHECK(read(target) == "ordinary-new-project");
    CHECK(!exists(temporary));
    CHECK(owner.preflights == p && owner.rejections == r && owner.precommits == 0);
    endTransaction(f);
    std::puts("PASS fresh live=false ignores stale cached true and permits ordinary native save");
}
void liveAppearsAfterOrdinaryCommit(bool foundationFirst) {
    const auto target = path(foundationFirst ? "ordinary-commit-move.flp" : "ordinary-commit-unlink.flp");
    const auto temporary = path(foundationFirst ? "ordinary-commit-move.tmp" : "ordinary-commit-unlink.tmp");
    const auto backup = path("ordinary-commit-move.bak");
    seed(target, "ordinary-commit-original");
    owner.ready = false;
    owner.live.store(false, std::memory_order_release);
    setNativeSaveGuardHasLiveARA(false);
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes, r = owner.rejections;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "ordinary-commit-new");
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(!hasActiveNativeSaveFromAnyThread());
    if (foundationFirst) allowedMove(target, backup);
    else CHECK(testing::unlink(target.c_str()) == 0);
    CHECK(!exists(target));
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->ordinaryCommit);
    CHECK(currentNativeSaveTransaction()->commitBegun);
    CHECK(!currentNativeSaveTransaction()->failed);
    CHECK(currentNativeSaveTransaction()->ownerCookie == nullptr);
    CHECK(owner.preflights == p && owner.freezes == z && owner.precommits == 0);
    liveFromBackground(true);
    bool marked = true, active = false;
    std::thread worker([&] { active=hasActiveNativeSaveFromAnyThread();marked = markActiveNativeSaveFailedFromAnyThread(); });
    worker.join();
    CHECK(!marked); // Commit has already removed/moved the old target.
    CHECK(active);
    CHECK(!currentNativeSaveTransaction()->failed);
    allowedMove(temporary, target);
    CHECK(read(target) == "ordinary-commit-new");
    CHECK(!exists(temporary));
    if (foundationFirst) CHECK(read(backup) == "ordinary-commit-original");
    else CHECK(!exists(backup));
    CHECK(owner.preflights == p && owner.freezes == z && owner.precommits == 0);
    CHECK(owner.rejections == r);
    endTransaction(f);
    CHECK(!hasActiveNativeSaveFromAnyThread());
    owner.live.store(false, std::memory_order_release);
    std::puts(foundationFirst ? "PASS ordinary backup commit completes despite newly live owner and late invalidation" :
                               "PASS ordinary unlink commit completes despite newly live owner and late invalidation");
}
} // namespace

int main() {
    @autoreleasepool {
        const std::string base = "/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/2026-10-24/build/save-guard";
        root = base + "/liveness-tests-" + [[[NSUUID UUID] UUIDString] UTF8String];
        CHECK(::mkdir(root.c_str(), 0700) == 0);
        struct stat metadata{};
        CHECK(::lstat(root.c_str(), &metadata) == 0);
        CHECK(S_ISDIR(metadata.st_mode) && (metadata.st_mode & 0777) == 0700);
        CHECK(testing::initialize(owner.callbacks(), root));
        CHECK(nativeSaveGuardReady());
        staleFalseCannotOverrideFreshTrue();
        liveAppearsBeforeCommit(false);
        liveAppearsBeforeCommit(true);
        pendingSurvivesFreshFalse();
        freshFalseIgnoresStaleTrue();
        liveAppearsAfterOrdinaryCommit(false);
        liveAppearsAfterOrdinaryCommit(true);
        CHECK(owner.callbacksOK && owner.freezes == owner.releases);
        std::printf("PASS %u assertions; preflights=%u precommits=%u rejections=%u finishes=%u freezes=%u releases=%u\nEvidence retained: %s\n",
                    assertions, owner.preflights, owner.precommits, owner.rejections,
                    owner.finishes, owner.freezes, owner.releases, root.c_str());
    }
    return 0;
}
