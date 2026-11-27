// Player's fork-only public methods, kept out of the upstream src/player.h,
// which includes this file inside `class Player`'s public section. Declarations
// only. Bodies: src/capture/capture_session.cpp (capture),
// src/fork/player_voice_group.cpp (voice-group start) and src/player.cpp
// (requestAudioDeviceIdleStop, which is part of the device-lifecycle
// scheduler there).
//
// No include guard: it is meant to be included exactly once, by player.h.

  // Native capture.

  /// @brief Enumerate OS capture/input devices. Static for the same reason as
  /// [listPlaybackDevices]: it must not depend on engine lifetime.
  static std::vector<CaptureDevice> listCaptureDevices();

  /// @brief Start recording the native miniaudio capture device to a WAV file.
  PlayerErrors startCapture(const std::string &filePath,
                            unsigned int sampleRate,
                            unsigned int channels,
                            unsigned int bufferSizeFrames,
                            float inputGainDb,
                            int captureDeviceID,
                            const std::string &mirrorFilePath,
                            unsigned int mirrorFormat,
                            unsigned int mirrorBitsPerSample,
                            CaptureStartInfo *info);

  /// @brief Start native capture and a SoLoud voice from one native command.
  PlayerErrors startCaptureAndPlay(const std::string &filePath,
                                   unsigned int soundHash,
                                   unsigned int busId,
                                   unsigned int sampleRate,
                                   unsigned int channels,
                                   unsigned int bufferSizeFrames,
                                   float volume,
                                   float pan,
                                   double startAtSeconds,
                                   bool looping,
                                   double loopingStartAt,
                                   float inputGainDb,
                                   int captureDeviceID,
                                   const std::string &mirrorFilePath,
                                   unsigned int mirrorFormat,
                                   unsigned int mirrorBitsPerSample,
                                   CapturePlaybackStartInfo *info);

  /// @brief Stop recording the native miniaudio capture device.
  PlayerErrors stopCapture(CaptureStopInfo *info);

  /// @brief Cancel recording and delete the partially written WAV file.
  PlayerErrors cancelCapture();

  /// @brief Return true if miniaudio capture is active.
  bool isCaptureRecording() const;

  /// @brief Capture a same-clock snapshot from the active capture session.
  PlayerErrors getCaptureClockSnapshot(CaptureClockInfo *info) const;

  /// @brief Capture the current input level from the active capture session.
  PlayerErrors getCaptureLevelSnapshot(CaptureLevelInfo *info);

  // Engine power.

  /// Queue a one-shot output stop when the application has proved domain idle,
  /// even if empty buses or paused transport resources remain alive. This does
  /// not mutate voices or suppress future starts. Requires a finite idle policy.
  bool requestAudioDeviceIdleStop();

  // Voice groups.

  /// @brief Atomically schedule every prepared member of a voice group at one
  /// absolute engine deadline.
  ///
  /// All validation and the successful delay/unpause commit happen under one
  /// audio mutex. Any failure leaves the voices and group unchanged.
  VoiceGroupStartResult scheduleVoiceGroupStartAt(
      SoLoud::handle voiceGroupHandle,
      SoLoud::handle requiredMainHandle,
      int expectedMemberCount,
      double engineDeadline);
