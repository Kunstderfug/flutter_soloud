// The engine leaves the app's AVAudioSession alone on iOS: the context is
// created with sessionCategory = none and noAudioSessionActivate /
// noAudioSessionDeactivate (init_platform_context() in soloud_miniaudio.cpp),
// so the category, mode and options the app chose must survive init, play,
// interruptions and deinit, and the engine must never deactivate the session.
// Listing the capture devices, before init and while a sound plays, must
// leave the session alone too and must not stop the output (the engine clock
// keeps advancing).
//
// Runs in the iOS simulator with the whole engine. The AVAudioSession
// setters (setCategory:..., setMode:..., setActive:...) are swizzled to
// record every call the engine makes (the test's own calls are not counted),
// and the category / mode / options are read back after each step, for three
// app configurations (Playback + mixWithOthers + spokenAudio mode, Ambient,
// and GPhil's PlayAndRecord with its options).
//
// Activation is allowed in one place: the engine calls setActive:YES before
// restarting the device from stopped (the MA_APPLE_MOBILE start path in
// soloud_miniaudio.cpp, for remote-command routing after interruptions). The
// test prints how often that happened; it never deactivates.
//
// Build & run with test/ios/audio_session_category_test.sh.

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "../support/engine_ffi.h"

using namespace enginetest;

extern "C" {
void listCaptureDevices(char **devicesName, int **deviceId, int **isDefault, int *n_devices);
void freeListCaptureDevices(char **devicesName, int **deviceId, int **isDefault, int n_devices);
}

static std::atomic<bool> gRecording{false};
static std::atomic<int> gSetCategoryCalls{0}, gSetModeCalls{0}, gActivateCalls{0}, gDeactivateCalls{0};

static void resetCounts() {
    gSetCategoryCalls = gSetModeCalls = gActivateCalls = gDeactivateCalls = 0;
}

// Swizzling: each replacement records the call (while recording) and calls
// the original implementation.
template <typename Fn> struct Orig { static Fn fn; };
template <typename Fn> Fn Orig<Fn>::fn = nullptr;

typedef BOOL (*SetCatFn)(id, SEL, NSString *, NSError **);
typedef BOOL (*SetCatOptsFn)(id, SEL, NSString *, AVAudioSessionCategoryOptions, NSError **);
typedef BOOL (*SetCatModeOptsFn)(id, SEL, NSString *, NSString *, AVAudioSessionCategoryOptions, NSError **);
typedef BOOL (*SetModeFn)(id, SEL, NSString *, NSError **);
typedef BOOL (*SetActiveFn)(id, SEL, BOOL, NSError **);
typedef BOOL (*SetActiveOptsFn)(id, SEL, BOOL, AVAudioSessionSetActiveOptions, NSError **);

static SetCatFn oSetCat;
static SetCatOptsFn oSetCatOpts;
static SetCatModeOptsFn oSetCatModeOpts;
static SetModeFn oSetMode;
static SetActiveFn oSetActive;
static SetActiveOptsFn oSetActiveOpts;

static BOOL rSetCat(id s, SEL c, NSString *a, NSError **e) {
    if (gRecording) gSetCategoryCalls++;
    return oSetCat(s, c, a, e);
}
static BOOL rSetCatOpts(id s, SEL c, NSString *a, AVAudioSessionCategoryOptions o, NSError **e) {
    if (gRecording) gSetCategoryCalls++;
    return oSetCatOpts(s, c, a, o, e);
}
static BOOL rSetCatModeOpts(id s, SEL c, NSString *a, NSString *m, AVAudioSessionCategoryOptions o, NSError **e) {
    if (gRecording) gSetCategoryCalls++;
    return oSetCatModeOpts(s, c, a, m, o, e);
}
static BOOL rSetMode(id s, SEL c, NSString *m, NSError **e) {
    if (gRecording) gSetModeCalls++;
    return oSetMode(s, c, m, e);
}
static BOOL rSetActive(id s, SEL c, BOOL active, NSError **e) {
    if (gRecording) (active ? gActivateCalls : gDeactivateCalls)++;
    return oSetActive(s, c, active, e);
}
static BOOL rSetActiveOpts(id s, SEL c, BOOL active, AVAudioSessionSetActiveOptions o, NSError **e) {
    if (gRecording) (active ? gActivateCalls : gDeactivateCalls)++;
    return oSetActiveOpts(s, c, active, o, e);
}

template <typename Fn> static void swizzle(SEL sel, Fn replacement, Fn *original) {
    Method m = class_getInstanceMethod([AVAudioSession class], sel);
    if (!m) {
        std::printf("  note: no -[AVAudioSession %s]\n", sel_getName(sel));
        return;
    }
    *original = reinterpret_cast<Fn>(method_setImplementation(m, reinterpret_cast<IMP>(replacement)));
}

struct SessionState {
    std::string category, mode;
    unsigned long options = 0;
};

static SessionState current() {
    @autoreleasepool {
        AVAudioSession *s = [AVAudioSession sharedInstance];
        return {[s.category UTF8String] ?: "", [s.mode UTF8String] ?: "", static_cast<unsigned long>(s.categoryOptions)};
    }
}

static void expectUnchanged(const SessionState &want, const char *step) {
    const SessionState now = current();
    ET_CHECK(now.category == want.category && now.mode == want.mode && now.options == want.options,
             "%s: category %s mode %s options 0x%lx (app set %s / %s / 0x%lx)", step, now.category.c_str(),
             now.mode.c_str(), now.options, want.category.c_str(), want.mode.c_str(), want.options);
    ET_CHECK(gSetCategoryCalls == 0 && gSetModeCalls == 0, "%s: engine called setCategory %d / setMode %d times",
             step, gSetCategoryCalls.load(), gSetModeCalls.load());
    ET_CHECK(gDeactivateCalls == 0, "%s: engine deactivated the session %d times", step, gDeactivateCalls.load());
}

// Lists the capture devices the way the Dart side does; returns how many.
// The simulator may have none, which is fine: the session calls happen anyway.
static int listCapture() {
    constexpr int kCap = 255;
    char *names[kCap] = {};
    int *ids[kCap] = {};
    int *defs[kCap] = {};
    int n = 0;
    listCaptureDevices(names, ids, defs, &n);
    freeListCaptureDevices(names, ids, defs, n);
    return n;
}

// Puts the app's session back (not recorded) and clears the counts, so a
// failed step does not spill into the next one.
static void restoreSession(NSString *category, NSString *mode, AVAudioSessionCategoryOptions options) {
    const bool recording = gRecording;
    gRecording = false;
    @autoreleasepool {
        [[AVAudioSession sharedInstance] setCategory:category mode:mode options:options error:nil];
    }
    resetCounts();
    gRecording = recording;
}

static void expectCaptureListLeavesSessionAlone(const SessionState &want, const char *step) {
    resetCounts();
    const int n = listCapture();
    std::printf("  note: %d capture devices %s\n", n, step);
    expectUnchanged(want, step);
    ET_CHECK(gActivateCalls == 0, "%s: engine activated the session %d times", step, gActivateCalls.load());
}

static void runConfig(NSString *category, NSString *mode, AVAudioSessionCategoryOptions options) {
    std::printf("[case] app session: %s, %s, options 0x%lx\n", [category UTF8String], [mode UTF8String],
                static_cast<unsigned long>(options));
    @autoreleasepool {
        NSError *error = nil;
        BOOL ok = [[AVAudioSession sharedInstance] setCategory:category mode:mode options:options error:&error];
        ET_CHECK(ok, "the app sets its category (%s)", error ? [[error description] UTF8String] : "ok");
    }
    const SessionState want = current();
    resetCounts();
    gRecording = true;

    expectCaptureListLeavesSessionAlone(want, "after listing capture devices before init");
    restoreSession(category, mode, options);

    gState.clear();
    ET_CHECK(initLikeDart() == noError, "init");
    expectUnchanged(want, "after init");
    ET_CHECK(gActivateCalls == 0, "init does not activate the session (%d)", gActivateCalls.load());

    const unsigned int h = playSine();
    ET_CHECK(h != 0 && waitDeviceState(audioDeviceStarted) && mixing(h), "play");
    expectUnchanged(want, "after play");
    ET_CHECK(gActivateCalls == 0, "the first play after init does not activate the session (%d)",
             gActivateCalls.load());

    // While a sound plays: the output must keep running, so the engine clock
    // (which advances only while the device renders) keeps advancing.
    expectCaptureListLeavesSessionAlone(want, "after listing capture devices while playing");
    {
        const double before = getEngineTime();
        sleepMs(500);
        const double advanced = getEngineTime() - before;
        ET_CHECK(advanced > 0.25 && mixing(h), "the engine clock keeps advancing after the list (%.3f s in 0.5 s)",
                 advanced);
    }
    restoreSession(category, mode, options);

    // An idle stop and the next play restart the device from stopped.
    setAudioDeviceIdleTimeout(100);
    stop(h);
    ET_CHECK(waitDeviceState(audioDeviceStopped), "idle stop");
    const unsigned int h1 = playSine();
    ET_CHECK(h1 != 0 && waitDeviceState(audioDeviceStarted), "play restarts the device");
    std::printf("  note: setActive:YES calls so far (idle restart): %d\n", gActivateCalls.load());
    expectUnchanged(want, "after an idle stop and restart");
    setAudioDeviceIdleTimeout(-1);

    // An interruption and its end: re-activating then is what the comment
    // in the source describes, so it is not counted against L2.
    debugTriggerAudioInterruption(1);
    waitDeviceState(audioDeviceStopped);
    debugTriggerAudioInterruption(0);
    ET_CHECK(waitDeviceState(audioDeviceStarted), "restarted after an interruption");
    std::printf("  note: setActive:YES calls so far (with the interruption): %d\n", gActivateCalls.load());
    expectUnchanged(want, "after an interruption");

    stop(h1);
    ET_CHECK(stopAudioDevice(1) == noError, "stopAudioDevice");
    expectUnchanged(want, "after stopAudioDevice");

    dispose();
    expectUnchanged(want, "after deinit");
    gRecording = false;
}

static int runAll() {
    swizzle(@selector(setCategory:error:), rSetCat, &oSetCat);
    swizzle(@selector(setCategory:withOptions:error:), rSetCatOpts, &oSetCatOpts);
    swizzle(@selector(setCategory:mode:options:error:), rSetCatModeOpts, &oSetCatModeOpts);
    swizzle(@selector(setMode:error:), rSetMode, &oSetMode);
    swizzle(@selector(setActive:error:), rSetActive, &oSetActive);
    swizzle(@selector(setActive:withOptions:error:), rSetActiveOpts, &oSetActiveOpts);

    runConfig(AVAudioSessionCategoryPlayback, AVAudioSessionModeSpokenAudio,
              AVAudioSessionCategoryOptionMixWithOthers);
    runConfig(AVAudioSessionCategoryAmbient, AVAudioSessionModeDefault, 0);
    // GPhil's session (ios/Runner/AudioPluginHandler.swift in the app).
    runConfig(AVAudioSessionCategoryPlayAndRecord, AVAudioSessionModeDefault,
              AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetooth |
                  AVAudioSessionCategoryOptionAllowBluetoothA2DP | AVAudioSessionCategoryOptionAllowAirPlay |
                  AVAudioSessionCategoryOptionMixWithOthers);
    return finish("audio session category");
}

int main(int argc, char **argv) {
    parseArgs(argc, argv);
    std::thread worker([] { std::exit(runAll()); });
    worker.detach();
    CFRunLoopRun();
    return 0;
}
