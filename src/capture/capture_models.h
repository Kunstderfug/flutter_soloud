#pragma once

#include "soloud/src/backend/miniaudio/miniaudio.h"

#include <cstdint>

enum CaptureMirrorFormat
{
  captureMirrorNone = 0,
  captureMirrorFlac = 1,
  captureMirrorWavPack = 2,
};

struct CaptureDevice
{
  char *name;
  unsigned int isDefault;
  unsigned int id;
  ma_device_id deviceId;
};

struct CaptureStartInfo
{
  unsigned int sampleRate;
  unsigned int channels;
  uint64_t sessionStartHostTimeNanos;
  uint64_t captureStartHostTimeNanos;
  unsigned int mirrorFormat;
  bool mirrorActive;
};

struct CaptureStopInfo
{
  unsigned int sampleRate;
  unsigned int channels;
  uint64_t frameCount;
  uint64_t sessionStartHostTimeNanos;
  uint64_t captureStartHostTimeNanos;
  uint64_t firstInputBufferHostTimeNanos;
  uint64_t firstInputBufferFrameIndex;
  uint64_t captureStopHostTimeNanos;
  unsigned int mirrorFormat;
  bool mirrorSucceeded;
  uint64_t mirrorFrameCount;
  uint64_t writerOverflowFrames;
  uint64_t writerSilenceFrames;
  bool writerFailed;
};

struct CaptureClockInfo
{
  uint64_t hostTimeNanos;
  uint64_t sessionStartHostTimeNanos;
  unsigned int sampleRate;
  uint64_t inputDeviceFrame;
};

struct CaptureLevelInfo
{
  float currentPeak;
  float currentRms;
  float peakSinceLastRead;
  float heldPeak;
  uint64_t frameCount;
};

struct CapturePlaybackStartInfo
{
  unsigned int handle;
  unsigned int sampleRate;
  unsigned int channels;
  uint64_t sessionStartHostTimeNanos;
  uint64_t captureStartHostTimeNanos;
  uint64_t playbackStartHostTimeNanos;
  unsigned int mirrorFormat;
  bool mirrorActive;
};
