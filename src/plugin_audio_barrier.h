// The audio-thread barrier shared by the VST3 and CLAP hosts, defined in
// bindings.cpp when either host is compiled in.
#pragma once

#if defined(SOLOUD_ENABLE_VST3) || defined(SOLOUD_ENABLE_CLAP)
extern "C"
{
    /// Returns once no mix that may have loaded a plugin pointer from a host's
    /// lookup table is still running. A no-op when the engine is not inited.
    /// Never call it with the engine's lifetime lock held.
    void soloud_pluginAudioThreadBarrier(void *userData);
}
#endif
