/*
SoLoud audio engine
Copyright (c) 2013-2015 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

// flutter_soloud fork addition (not in SoLoud or upstream flutter_soloud):
// Soloud::scheduleVoiceGroupStartAt, declared in soloud.h. Kept out of
// soloud_core_voicegroup.cpp so that vendored file stays as upstream ships it.

#include "soloud.h"

#include <cmath>
#include <limits>

namespace SoLoud
{
	VoiceGroupStartResult Soloud::scheduleVoiceGroupStartAt(
		handle aVoiceGroupHandle,
		handle aRequiredMainHandle,
		int aExpectedMemberCount,
		time aEngineDeadline)
	{
		lockAudioMutex_internal();

		if (mSamplerate == 0)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_BACKEND_NOT_INITIALIZED;
		}
		if (aExpectedMemberCount <= 0 ||
			aExpectedMemberCount > static_cast<int>(VOICE_COUNT) ||
			!std::isfinite(aEngineDeadline) ||
			aEngineDeadline < 0)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_INPUT;
		}

		if ((aVoiceGroupHandle & 0xfffff000) != 0xfffff000)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_GROUP;
		}
		const unsigned int groupIndex = aVoiceGroupHandle & 0xfff;
		if (groupIndex >= mVoiceGroupCount ||
			mVoiceGroup[groupIndex] == NULL ||
			mVoiceGroup[groupIndex][1] == 0)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_GROUP;
		}

		handle *members = mVoiceGroup[groupIndex] + 1;
		const unsigned int capacity = mVoiceGroup[groupIndex][0] - 1;
		unsigned int memberCount = 0;
		while (memberCount < capacity && members[memberCount] != 0)
		{
			memberCount++;
		}
		if (memberCount != static_cast<unsigned int>(aExpectedMemberCount))
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_MEMBER_COUNT_MISMATCH;
		}

		if (aRequiredMainHandle == 0 ||
			(aRequiredMainHandle & 0xfffff000) == 0xfffff000 ||
			(aRequiredMainHandle & 0xfff) == 0 ||
			(aRequiredMainHandle & 0xfff) > VOICE_COUNT)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_MAIN;
		}
		const int mainVoice =
			getVoiceFromHandle_internal(aRequiredMainHandle);
		bool mainPresent = false;
		for (unsigned int i = 0; i < memberCount; i++)
		{
			if (members[i] == aRequiredMainHandle)
			{
				mainPresent = true;
				break;
			}
		}
		if (mainVoice == -1 || !mainPresent)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_MAIN;
		}

		int memberVoices[VOICE_COUNT];
		for (unsigned int i = 0; i < memberCount; i++)
		{
			const handle member = members[i];
			if (member == 0 ||
				(member & 0xfffff000) == 0xfffff000 ||
				(member & 0xfff) == 0 ||
				(member & 0xfff) > VOICE_COUNT)
			{
				unlockAudioMutex_internal();
				return VOICE_GROUP_START_INVALID_MEMBER;
			}
			const int voice = getVoiceFromHandle_internal(member);
			if (voice == -1)
			{
				unlockAudioMutex_internal();
				return VOICE_GROUP_START_INVALID_MEMBER;
			}
			if (!(mVoice[voice]->mFlags & AudioSourceInstance::PAUSED))
			{
				unlockAudioMutex_internal();
				return VOICE_GROUP_START_MEMBER_NOT_PAUSED;
			}
			memberVoices[i] = voice;
		}

		if (aEngineDeadline <= mStreamTime)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_DEADLINE_REACHED;
		}
		const double delayValue =
			(aEngineDeadline - mStreamTime) * mSamplerate;
		if (!std::isfinite(delayValue) ||
			delayValue >
				static_cast<double>(std::numeric_limits<unsigned int>::max()) -
					0.5)
		{
			unlockAudioMutex_internal();
			return VOICE_GROUP_START_INVALID_INPUT;
		}
		const unsigned int delaySamples =
			static_cast<unsigned int>(std::floor(delayValue + 0.5));

		for (unsigned int i = 0; i < memberCount; i++)
		{
			mVoice[memberVoices[i]]->mDelaySamples = delaySamples;
			setVoicePause_internal(memberVoices[i], false);
		}

		unlockAudioMutex_internal();
		return VOICE_GROUP_START_SUCCESS;
	}
}
