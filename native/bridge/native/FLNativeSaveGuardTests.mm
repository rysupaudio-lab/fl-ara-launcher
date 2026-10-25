#import <Foundation/Foundation.h>
#include "FLNativeSaveGuard.h"
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
    bool ready = true, precommitReady = false;
    unsigned preflights = 0, precommits = 0, rejections = 0, finishes = 0, freezes = 0, releases = 0;
    int cookie = 0;
    bool callbackContractOK = true;
    NativeSaveTransaction lastFinished;
    static bool preflight(const NativeSaveRequest&, void** cookieOut, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.preflights;
        *cookieOut = nullptr;
        if (!self.ready) return false;
        if (self.freezes != self.releases) self.callbackContractOK = false;
        ++self.freezes;
        *cookieOut = &self.cookie;
        return true;
    }
    static bool precommit(const NativeSaveRequest&, void* cookieIn, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.precommits;
        if (cookieIn != &self.cookie || self.freezes != self.releases + 1)
            self.callbackContractOK = false;
        return self.precommitReady;
    }
    static void rejected(const NativeSaveRequest&, NativeSaveRejection reason,
                         void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.rejections;
        if (reason == NativeSaveRejection::None) self.callbackContractOK = false;
    }
    static void finished(const NativeSaveTransaction& transaction, void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        ++self.finishes;
        self.lastFinished = transaction;
        if (transaction.ownerCookie) {
            if (transaction.ownerCookie != &self.cookie || self.releases >= self.freezes)
                self.callbackContractOK = false;
            ++self.releases;
        }
    }
    NativeSaveGuardCallbacks callbacks() {
        return {preflight, precommit, rejected, finished, this};
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
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count >= 0);
        if (!count) break;
        result.append(buffer, static_cast<std::size_t>(count));
    }
    CHECK(::close(fd) == 0);
    return result;
}
bool exists(const std::string& filename) {
    struct stat metadata{};
    return ::lstat(filename.c_str(), &metadata) == 0;
}
NativeSaveRequest frame(const std::string& target, const std::string& temporary,
                        bool directed = false) {
    static std::uintptr_t sequence = 0x100000;
    NativeSaveRequest result;
    result.actualSaveFrame = (sequence += 0x1000);
    result.targetPath = target;
    result.actualOpenPath = temporary;
    result.nativeFlags = 0;
    result.actualSaveType = 0;
    result.openFlags = O_RDWR | O_CREAT | O_TRUNC;
    result.directedTarget = directed;
    return result;
}
void start(const NativeSaveRequest& request) {
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(testing::setFrame(request));
}
void end(unsigned previousFinishes) {
    // A poll while the exact frame is live must never release the owner freeze.
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == previousFinishes);
    testing::clearFrame();
    CHECK(owner.finishes == previousFinishes);
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == previousFinishes + 1);
    CHECK(currentNativeSaveTransaction() == nullptr);
    CHECK(owner.freezes == owner.releases);
    owner.precommitReady = false;
    CHECK(owner.callbackContractOK);
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == previousFinishes + 1);
}
void refusedOpen(const std::string& filename, int flags) {
    errno = 0;
    CHECK(testing::open(filename.c_str(), flags, 0600) == -1);
    CHECK(errno == EBUSY);
}
void refusedMove(const std::string& source, const std::string& destination) {
    NSError* error = nil;
    CHECK(![[NSFileManager defaultManager] moveItemAtPath:ns(source)
                                                 toPath:ns(destination) error:&error]);
    CHECK(error != nil);
    CHECK([[error domain] isEqualToString:NSPOSIXErrorDomain]);
    CHECK([error code] == EBUSY);
}
void allowedMove(const std::string& source, const std::string& destination) {
    NSError* error = nil;
    CHECK([[NSFileManager defaultManager] moveItemAtPath:ns(source)
                                                toPath:ns(destination) error:&error]);
    CHECK(error == nil);
}

void deniedPreflight() {
    const auto target = path("denied.flp"), temporary = path("denied.tmp");
    const char originalBytes[] = "old-project\0preserve-all-bytes\xff";
    const std::string original(originalBytes, sizeof originalBytes - 1);
    seed(target, original);
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    owner.ready = false;
    const auto request = frame(target, temporary);
    start(request);
    refusedOpen(temporary, request.openFlags);
    CHECK(owner.preflights == p + 1);
    CHECK(owner.freezes == z);
    CHECK(!exists(temporary));
    CHECK(read(target) == original);
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::OwnerNotReady);
    end(f);
    owner.ready = true;
    std::puts("PASS denied preflight preserves target and creates no temp");
}
void wrongCreatingRoute() {
    const auto target = path("wrong-route.flp"), temporary = path("wrong-route.tmp");
    seed(target, "wrong-route-original");
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    auto request = frame(target, temporary);
    request.openFlags = O_WRONLY | O_CREAT | O_TRUNC;
    start(request);
    refusedOpen(temporary, request.openFlags);
    CHECK(owner.preflights == p && owner.freezes == z);
    CHECK(!exists(temporary));
    CHECK(read(target) == "wrong-route-original");
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::NativeOpenRouteMismatch);
    end(f);
    std::puts("PASS O_WRONLY creating route refused; admitted native route is O_RDWR");
}
void deniedDirectedTarget() {
    const auto target = path("directed.flp");
    const std::string original = "directed-original";
    seed(target, original);
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    const auto request = frame(target, target, true);
    start(request);
    refusedOpen(target, request.openFlags);
    CHECK(owner.preflights == p); // Route safety is checked before acquiring a freeze.
    CHECK(owner.freezes == z);
    CHECK(read(target) == original);
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::DirectTargetUnsupported);
    end(f);
    std::puts("PASS directed target refused before owner preflight");
}
void lateFailure() {
    const auto target = path("late.flp"), temporary = path("late.tmp"), backup = path("late.bak");
    const std::string original = "late-old-project", prepared = "partial-serialized-project";
    seed(target, original);
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), prepared);
    CHECK(owner.preflights == p + 1);
    CHECK(owner.freezes == z + 1);
    CHECK(owner.releases == z);
    const auto* transaction = currentNativeSaveTransaction();
    CHECK(transaction != nullptr && transaction->openAllowed && !transaction->failed);
    CHECK(transaction->ownerCookie == &owner.cookie);
    CHECK(markCurrentNativeSaveFailed());
    errno = 0;
    CHECK(testing::unlink(target.c_str()) == -1);
    CHECK(errno == EBUSY);
    refusedMove(target, backup);
    refusedMove(temporary, target);
    CHECK(read(target) == original);
    CHECK(read(temporary) == prepared);
    CHECK(!exists(backup));
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(owner.finishes == f);
    end(f);
    CHECK(owner.lastFinished.failed);
    std::puts("PASS late failure blocks unlink and both Foundation commit moves");
}
void deniedPrecommit(bool backupFirst) {
    const auto target = path(backupFirst ? "precommit-move.flp" : "precommit-unlink.flp");
    const auto temporary = path(backupFirst ? "precommit-move.tmp" : "precommit-unlink.tmp");
    const auto backup = path("precommit-move.bak");
    seed(target, "precommit-original");
    const unsigned f = owner.finishes, c = owner.precommits;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "missing-one-component");
    CHECK(!owner.precommitReady);
    if (backupFirst) refusedMove(target, backup);
    else {
        errno = 0;
        CHECK(testing::unlink(target.c_str()) == -1);
        CHECK(errno == EBUSY);
    }
    CHECK(owner.precommits == c + 1);
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(!currentNativeSaveTransaction()->commitBegun);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::OwnerPrecommitRefused);
    CHECK(read(target) == "precommit-original");
    CHECK(read(temporary) == "missing-one-component");
    CHECK(!exists(backup));
    end(f);
    std::puts(backupFirst ? "PASS precommit refusal precedes first backup move" :
                           "PASS precommit refusal precedes first old-target unlink");
}
void backgroundFailureBeforeCommit() {
    const auto target = path("thread-before.flp"), temporary = path("thread-before.tmp");
    seed(target, "thread-before-original");
    const unsigned f = owner.finishes, c = owner.precommits;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "thread-before-prepared");
    owner.precommitReady = true;
    bool marked = false, activeBefore = false, activeAfter = false;
    std::thread worker([&] { activeBefore=hasActiveNativeSaveFromAnyThread();marked = markActiveNativeSaveFailedFromAnyThread();activeAfter=hasActiveNativeSaveFromAnyThread(); });
    worker.join();
    CHECK(marked);
    CHECK(activeBefore);
    CHECK(activeAfter); // A failed save is still active until actual frame exit.
    errno = 0;
    CHECK(testing::unlink(target.c_str()) == -1);
    CHECK(errno == EBUSY);
    CHECK(owner.precommits == c);
    refusedMove(temporary, target);
    CHECK(read(target) == "thread-before-original");
    CHECK(read(temporary) == "thread-before-prepared");
    end(f);
    CHECK(owner.lastFinished.failed);
    CHECK(!hasActiveNativeSaveFromAnyThread());
    std::puts("PASS background invalidation before commit blocks old-target destruction");
}
void backgroundFailureAfterCommit() {
    const auto target = path("thread-after.flp"), temporary = path("thread-after.tmp");
    seed(target, "thread-after-original");
    const unsigned f = owner.finishes, c = owner.precommits;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "thread-after-prepared");
    CHECK(hasActiveNativeSaveFromAnyThread());
    owner.precommitReady = true;
    CHECK(testing::unlink(target.c_str()) == 0);
    CHECK(currentNativeSaveTransaction()->commitBegun);
    CHECK(owner.precommits == c + 1);
    bool marked = true, activeBefore = false, activeAfter = false;
    std::thread worker([&] { activeBefore=hasActiveNativeSaveFromAnyThread();marked = markActiveNativeSaveFailedFromAnyThread();activeAfter=hasActiveNativeSaveFromAnyThread(); });
    worker.join();
    CHECK(!marked);
    CHECK(activeBefore);
    CHECK(activeAfter); // Commit blocks invalidation, not the active query.
    CHECK(!currentNativeSaveTransaction()->failed);
    allowedMove(temporary, target);
    CHECK(owner.precommits == c + 1); // Never revalidate halfway through commit.
    CHECK(read(target) == "thread-after-prepared");
    CHECK(!exists(temporary));
    end(f);
    CHECK(!owner.lastFinished.failed);
    CHECK(!hasActiveNativeSaveFromAnyThread());
    std::puts("PASS post-commit background invalidation refuses and remaining move completes");
}
void preparedSave() {
    const auto target = path("normal.flp"), temporary = path("normal.tmp");
    const std::string original = "normal-old-project", prepared = "normal-new-prepared-archive";
    seed(target, original);
    const unsigned p = owner.preflights, f = owner.finishes;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), prepared);
    CHECK(owner.preflights == p + 1);
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->openAllowed);
    owner.precommitReady = true; // Every expected envelope has serialized.
    CHECK(testing::unlink(target.c_str()) == 0);
    CHECK(currentNativeSaveTransaction()->commitBegun);
    allowedMove(temporary, target);
    CHECK(read(target) == prepared);
    CHECK(!exists(temporary));
    end(f);
    CHECK(!owner.lastFinished.failed);
    std::puts("PASS prepared native temp/unlink/move pattern commits new bytes");
}
void unrelatedOperations() {
    const unsigned p = owner.preflights, f = owner.finishes;
    const auto a = path("outside-a.dat"), b = path("outside-b.dat");
    writeFD(testing::open(a.c_str(), O_WRONLY | O_CREAT | O_EXCL), "outside-save");
    allowedMove(a, b);
    CHECK(read(b) == "outside-save");
    CHECK(testing::unlink(b.c_str()) == 0);
    CHECK(!exists(a) && !exists(b));
    CHECK(owner.preflights == p && owner.finishes == f);
    CHECK(currentNativeSaveTransaction() == nullptr);

    const auto target = path("unrelated.flp"), temporary = path("unrelated.tmp");
    const auto c = path("inside-frame-a.dat"), d = path("inside-frame-b.dat");
    seed(target, "untouched-save-target");
    start(frame(target, temporary));
    writeFD(testing::open(c.c_str(), O_WRONLY | O_CREAT | O_EXCL), "inside-frame-unrelated");
    allowedMove(c, d);
    CHECK(read(d) == "inside-frame-unrelated");
    CHECK(testing::unlink(d.c_str()) == 0);
    CHECK(read(target) == "untouched-save-target");
    CHECK(!exists(temporary));
    CHECK(owner.preflights == p && owner.finishes == f);
    CHECK(currentNativeSaveTransaction() == nullptr);
    testing::clearFrame();
    pollNativeSaveGuardTransactions();
    CHECK(owner.finishes == f);
    std::puts("PASS unrelated file operations forward outside and inside Save frames");
}
void repeatedOpen() {
    const auto target = path("repeated.flp"), temporary = path("repeated.tmp");
    seed(target, "repeated-original");
    const unsigned p = owner.preflights, f = owner.finishes, z = owner.freezes;
    const auto request = frame(target, temporary);
    start(request);
    writeFD(testing::open(temporary.c_str(), request.openFlags), "first-open-contents");
    CHECK(owner.preflights == p + 1 && owner.freezes == z + 1);
    refusedOpen(temporary, request.openFlags);
    CHECK(owner.preflights == p + 1 && owner.freezes == z + 1);
    CHECK(read(temporary) == "first-open-contents");
    CHECK(read(target) == "repeated-original");
    CHECK(currentNativeSaveTransaction() != nullptr);
    CHECK(currentNativeSaveTransaction()->failed);
    CHECK(currentNativeSaveTransaction()->rejection == NativeSaveRejection::NestedOrRepeatedOpen);
    end(f);
    std::puts("PASS same-frame repeated create/truncate refused without stale freeze reuse");
}
} // namespace

int main() {
    CHECK(!hasActiveNativeSaveFromAnyThread());
    @autoreleasepool {
        const std::string base = "/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/2026-10-24/build/save-guard";
        root = base + "/tests-" + [[[NSUUID UUID] UUIDString] UTF8String];
        CHECK(::mkdir(root.c_str(), 0700) == 0);
        struct stat metadata{};
        CHECK(::lstat(root.c_str(), &metadata) == 0);
        CHECK(S_ISDIR(metadata.st_mode) && (metadata.st_mode & 0777) == 0700);
        CHECK(testing::initialize(owner.callbacks(), root));
        setNativeSaveGuardHasLiveARA(true);
        CHECK(nativeSaveGuardReady());
        deniedPreflight();
        wrongCreatingRoute();
        deniedDirectedTarget();
        lateFailure();
        deniedPrecommit(false);
        deniedPrecommit(true);
        backgroundFailureBeforeCommit();
        backgroundFailureAfterCommit();
        preparedSave();
        unrelatedOperations();
        repeatedOpen();
        CHECK(owner.callbackContractOK && owner.freezes == owner.releases);
        CHECK(!markCurrentNativeSaveFailed());
        CHECK(!hasActiveNativeSaveFromAnyThread());
        std::printf("PASS %u assertions; preflights=%u rejections=%u finishes=%u freezes=%u releases=%u\nEvidence retained: %s\n",
                    assertions, owner.preflights, owner.rejections, owner.finishes,
                    owner.freezes, owner.releases, root.c_str());
    }
    return 0;
}
