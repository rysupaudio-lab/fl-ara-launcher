// RysUpRateHold: keep a hidden DAW from changing any audio device's sample rate.
//
// Loaded next to RysUpExportDriver.dylib (DYLD_INSERT_LIBRARIES) when a recipe names "hold_device_rate".
// Live 11 starts its engine at the device's own rate, then, while the set loads, sets the device to the
// rate saved in Jordan's Live preferences (44.1 kHz), which flips the shared Multi-Output Device for the
// whole Mac and makes the Gotaway export come out with its vocal chains silent. This library interposes
// the CoreAudio calls that change a device's nominal rate and answers them with success without applying
// them, so the engine stays at whatever rate the device already runs. It writes no setting anywhere: it
// only stops this one process from writing one. Every call it sees is logged with the requested rate and
// a backtrace to $RYSUP_RATE_HOLD_LOG (default /tmp/rysup-rate-hold.log).
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_new, _old) \
    __attribute__((used)) static struct { const void *n; const void *o; } _interpose_##_old \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_new, (const void *)(unsigned long)&_old };

static int log_fd(void) {
    static int fd = -2;
    if (fd == -2) {
        const char *path = getenv("RYSUP_RATE_HOLD_LOG");
        fd = open(path && *path ? path : "/tmp/rysup-rate-hold.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    return fd;
}

static void note(const char *what, AudioObjectID obj, double rate, int held) {
    int fd = log_fd();
    if (fd < 0) return;
    char line[256];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int n = snprintf(line, sizeof line, "%ld.%03ld pid=%d %s object=%u rate=%.1f %s\n", (long)ts.tv_sec,
                     ts.tv_nsec / 1000000, getpid(), what, (unsigned)obj, rate, held ? "HELD" : "passed");
    write(fd, line, (size_t)n);
    if (!held) return;
    void *frames[24];
    int depth = backtrace(frames, 24);
    backtrace_symbols_fd(frames, depth, fd);
}

static double current_rate(AudioObjectID obj) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 r = 0;
    UInt32 sz = sizeof r;
    return AudioObjectGetPropertyData(obj, &a, 0, NULL, &sz, &r) == noErr ? r : 0;
}

static OSStatus hold_AudioObjectSetPropertyData(AudioObjectID obj, const AudioObjectPropertyAddress *addr,
                                                UInt32 qsz, const void *q, UInt32 sz, const void *data) {
    if (addr && addr->mSelector == kAudioDevicePropertyNominalSampleRate && data && sz >= sizeof(Float64)) {
        double want = *(const Float64 *)data;
        note("AudioObjectSetPropertyData(nominal rate)", obj, want, 1);
        char now[64];
        snprintf(now, sizeof now, "device-rate-now=%.1f", current_rate(obj));
        note(now, obj, want, 1);
        return noErr;
    }
    return AudioObjectSetPropertyData(obj, addr, qsz, q, sz, data);
}
DYLD_INTERPOSE(hold_AudioObjectSetPropertyData, AudioObjectSetPropertyData)

// With $RYSUP_ENGINE_RATE set (export.py passes the recipe's engine_rate), a device that runs at another rate
// is reported to this process as running at that rate, so Live prepares its engine and every plug-in there.
// An export is rendered offline and never goes through the device, so its real rate does not matter; the
// device itself is not touched. Live plays its IOProc at the device's real rate (a hidden export never plays).
static double engine_rate(void) {
    const char *v = getenv("RYSUP_ENGINE_RATE");
    return v && *v ? atof(v) : 0;
}

static void fake_rate(AudioObjectID obj, const AudioObjectPropertyAddress *addr, UInt32 *sz, void *data) {
    double want = engine_rate();
    if (want <= 0 || !addr || !data || !sz) return;
    switch (addr->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        if (*sz >= sizeof(Float64) && *(Float64 *)data != want) {
            note("reported nominal rate", obj, *(Float64 *)data, 0);
            *(Float64 *)data = want;
        }
        break;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        if (*sz >= sizeof(AudioValueRange)) {
            AudioValueRange *r = data;
            r->mMinimum = r->mMaximum = want;
            *sz = sizeof(AudioValueRange);
        }
        break;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        if (*sz >= sizeof(AudioStreamBasicDescription) && ((AudioStreamBasicDescription *)data)->mSampleRate != want)
            ((AudioStreamBasicDescription *)data)->mSampleRate = want;
        break;
    default:
        break;
    }
}

static OSStatus hold_AudioObjectGetPropertyData(AudioObjectID obj, const AudioObjectPropertyAddress *addr,
                                                UInt32 qsz, const void *q, UInt32 *sz, void *data) {
    OSStatus st = AudioObjectGetPropertyData(obj, addr, qsz, q, sz, data);
    if (st == noErr) fake_rate(obj, addr, sz, data);
    return st;
}
DYLD_INTERPOSE(hold_AudioObjectGetPropertyData, AudioObjectGetPropertyData)

static OSStatus hold_AudioUnitSetProperty(AudioUnit unit, AudioUnitPropertyID id, AudioUnitScope scope,
                                          AudioUnitElement el, const void *data, UInt32 sz) {
    // Log only: a HAL output unit's stream format is its client side and does not set the device's rate.
    if ((id == kAudioUnitProperty_SampleRate && data && sz >= sizeof(Float64)) ||
        (id == kAudioUnitProperty_StreamFormat && data && sz >= sizeof(AudioStreamBasicDescription) &&
         getenv("RYSUP_RATE_HOLD_VERBOSE"))) {
        double r = id == kAudioUnitProperty_SampleRate ? *(const Float64 *)data
                                                       : ((const AudioStreamBasicDescription *)data)->mSampleRate;
        note(id == kAudioUnitProperty_SampleRate ? "AudioUnitSetProperty(SampleRate)" : "AudioUnitSetProperty(StreamFormat)",
             (AudioObjectID)scope, r, 0);
    }
    return AudioUnitSetProperty(unit, id, scope, el, data, sz);
}
DYLD_INTERPOSE(hold_AudioUnitSetProperty, AudioUnitSetProperty)

__attribute__((constructor)) static void hold_loaded(void) {
    note("loaded", 0, 0, 0);
}
