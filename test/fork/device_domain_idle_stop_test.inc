// Included by device_coordinator_test.cpp after its common fixture helpers.
// These checks exercise actual output-device state, including the empty-bus
// pattern that makes a voice-count-only idle timeout insufficient.

void waitForDeviceState(AudioDeviceState expected)
{
    for (int i = 0; i < 200 && getAudioDeviceState() != expected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(getAudioDeviceState() == expected,
           "device state %d, expected %d", (int)getAudioDeviceState(), (int)expected);
}

void testDomainIdleStopsEmptyBusAndPreservesPausedVoice()
{
    std::printf("domain idle stops empty bus and preserves prepared voice\n");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    const unsigned int bus = createBus();
    unsigned int busHandle = 0;
    EXPECT(busPlayOnEngine(bus, 1.0f, false, &busHandle) == noError,
           "empty effects bus must start");
    const unsigned int hash = loadTestWaveform();
    unsigned int handle = 0;
    EXPECT(play(hash, bus, 0.0f, 0.0f, true, true, 0.0, 0.0,
                -1, -1, 1.0f, &handle) == noError,
           "paused voice must prepare");
    waitForDeviceState(audioDeviceStarted);
    // Force the mixer's active-voice list to observe the persistent bus.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT(stopAudioDevice(0) == noError, "ordinary conditional stop succeeds");
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "conditional stop cannot release the live empty bus");

    soloud_test::armBarrier(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(requestAudioDeviceIdleStop(), "domain idle request must be admitted");
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(getAudioDeviceState() == audioDeviceStopped,
           "domain idle must actually stop hardware despite empty bus");
    EXPECT(getPause(handle) == 1, "prepared voice must remain paused");
    EXPECT(getIsValidVoiceHandle(handle), "prepared voice must remain valid");
    EXPECT(getIsValidVoiceHandle(busHandle), "bus must remain valid");
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopCompleted);

    EXPECT(setPause(handle, false) == noError, "same voice must unpause");
    waitForDeviceState(audioDeviceStarted);
    EXPECT(getIsValidVoiceHandle(handle), "same handle must survive restart");
    stop(handle);
    destroyBus(bus);
    tearDownEngine();
}

void testDomainIdleStopYieldsToNewStart(bool explicitStart)
{
    std::printf("domain idle yields to newer %s\n", explicitStart ? "explicit start" : "play");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    const unsigned int hash = loadTestWaveform();
    EXPECT(startAudioDevice() == noError, "device prewarm must succeed");
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(requestAudioDeviceIdleStop(), "domain idle request must be admitted");
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopDequeued);
    unsigned int handle = 0;
    if (explicitStart)
        EXPECT(startAudioDevice() == noError, "new explicit start must succeed");
    else
    {
        handle = playUnpaused(hash);
        EXPECT(handle != 0, "new play must succeed");
    }
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopCompleted);
    // The scheduler is still parked before any subsequent start can execute:
    // this proves the stale request did not stop/restart the device unnoticed.
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "stale domain idle request must not stop newer startup");
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopCompleted);
    if (handle != 0)
        stop(handle);
    tearDownEngine();
}

void testDomainIdleStopYieldsToIndefinitePolicy()
{
    std::printf("domain idle yields to indefinite policy\n");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    EXPECT(startAudioDevice() == noError, "device prewarm must succeed");
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(requestAudioDeviceIdleStop(), "domain idle request must be admitted");
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopDequeued);
    setAudioDeviceIdleTimeout(-1);
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "new indefinite policy must cancel stale stop");
    EXPECT(!requestAudioDeviceIdleStop(), "indefinite policy must reject domain stop");
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopCompleted);
    tearDownEngine();
}

void testDomainIdleAdmissionDoesNotWaitOnInit()
{
    std::printf("domain idle admission never waits on engine lifecycle\n");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    soloudTestLockInitDeinit();
    const auto before = std::chrono::steady_clock::now();
    const bool accepted = requestAudioDeviceIdleStop();
    const auto elapsed = millisSince(before);
    soloudTestUnlockInitDeinit();
    EXPECT(!accepted, "busy native lifecycle must reject synchronous admission");
    EXPECT(elapsed < kNonBlockingBudgetMs,
           "admission blocked %lldms", elapsed);
    tearDownEngine();
    EXPECT(!requestAudioDeviceIdleStop(), "uninitialized engine must reject admission");
}

void testDomainIdleSupersedesOlderAutomaticStart()
{
    std::printf("domain idle supersedes older automatic start\n");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    const unsigned int hash = loadTestWaveform();
    soloud_test::armBarrier(DeviceBarrier::performAudioDeviceStartEntered);
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopCompleted);
    const unsigned int handle = playUnpaused(hash);
    EXPECT(handle != 0, "older playback must start");
    soloud_test::waitBarrierReached(DeviceBarrier::performAudioDeviceStartEntered);
    EXPECT(requestAudioDeviceIdleStop(), "new domain idle must be admitted");
    soloud_test::releaseBarrier(DeviceBarrier::performAudioDeviceStartEntered);
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(getAudioDeviceState() == audioDeviceStopped,
           "older startup completion must not swallow newer domain idle");
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopCompleted);
    stop(handle);
    tearDownEngine();
}

void testPublishedIndefinitePolicyBlocksDomainStopBeforeApply()
{
    std::printf("published keep-alive blocks domain stop before worker applies\n");
    EXPECT(bringUpEngine(), "engine must initialize");
    if (!isInited())
        return;
    setAudioDeviceIdleTimeout(kQuietIdleTimeoutMs);
    EXPECT(startAudioDevice() == noError, "device prewarm must succeed");
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::armBarrier(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(requestAudioDeviceIdleStop(), "domain idle request must be admitted");
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopDequeued);
    soloudTestLockInitDeinit();
    setAudioDeviceIdleTimeout(-1); // Published now; worker cannot apply yet.
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopDequeued);
    soloud_test::waitBarrierReached(DeviceBarrier::domainIdleStopCompleted);
    EXPECT(getAudioDeviceState() == audioDeviceStarted,
           "latest published policy must protect startup before apply");
    soloud_test::releaseBarrier(DeviceBarrier::domainIdleStopCompleted);
    soloudTestUnlockInitDeinit();
    tearDownEngine();
}
