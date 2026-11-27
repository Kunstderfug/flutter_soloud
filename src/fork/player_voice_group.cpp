// Player::scheduleVoiceGroupStartAt, the fork's one-deadline voice-group
// start. Kept out of the upstream src/player.cpp; declared in
// player_fork_methods.inc. The engine side is
// src/soloud/src/core/soloud_core_voicegroup.cpp.
#include "../player.h"

// The FFI enum must mirror SoLoud's result codes value for value.
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_SUCCESS) ==
    voiceGroupStartSuccess);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_BACKEND_NOT_INITIALIZED) ==
    voiceGroupStartBackendNotInitialized);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_INVALID_INPUT) ==
    voiceGroupStartInvalidInput);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_INVALID_GROUP) ==
    voiceGroupStartInvalidGroup);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_MEMBER_COUNT_MISMATCH) ==
    voiceGroupStartMemberCountMismatch);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_INVALID_MAIN) ==
    voiceGroupStartInvalidMain);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_INVALID_MEMBER) ==
    voiceGroupStartInvalidMember);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_MEMBER_NOT_PAUSED) ==
    voiceGroupStartMemberNotPaused);
static_assert(
    static_cast<int>(SoLoud::VOICE_GROUP_START_DEADLINE_REACHED) ==
    voiceGroupStartDeadlineReached);

VoiceGroupStartResult Player::scheduleVoiceGroupStartAt(
    SoLoud::handle voiceGroupHandle,
    SoLoud::handle requiredMainHandle,
    int expectedMemberCount,
    double engineDeadline)
{
    return static_cast<VoiceGroupStartResult>(
        soloud.scheduleVoiceGroupStartAt(
            voiceGroupHandle,
            requiredMainHandle,
            expectedMemberCount,
            engineDeadline));
}
