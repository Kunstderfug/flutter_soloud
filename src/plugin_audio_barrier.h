// The audio-thread barrier for the CLAP host, defined in fork/fork_bindings.cpp
// when either plugin host is compiled in.
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
