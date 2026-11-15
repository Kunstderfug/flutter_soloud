// FFI entry points of the CLAP plugin host, declared in bindings.h. Kept out
// of bindings.cpp so the CLAP host stays one seam. Always compiled: without
// SOLOUD_ENABLE_CLAP every function is an inert stub.
#include "bindings.h"

#ifdef SOLOUD_ENABLE_CLAP
#include "clap/clap_host.h"
#endif

extern "C"
{
    FFI_PLUGIN_EXPORT int soloud_clapIsSupported(void)
    {
#ifdef SOLOUD_ENABLE_CLAP
        return 1;
#else
        return 0;
#endif
    }

    FFI_PLUGIN_EXPORT void soloud_clapGetHostClapVersion(
        unsigned int *outMajor,
        unsigned int *outMinor,
        unsigned int *outRevision)
    {
        unsigned int major = 0, minor = 0, revision = 0;
#ifdef SOLOUD_ENABLE_CLAP
        major = soloud_clap::hostClapVersionMajor();
        minor = soloud_clap::hostClapVersionMinor();
        revision = soloud_clap::hostClapVersionRevision();
#endif
        if (outMajor != nullptr)
            *outMajor = major;
        if (outMinor != nullptr)
            *outMinor = minor;
        if (outRevision != nullptr)
            *outRevision = revision;
    }

    FFI_PLUGIN_EXPORT void soloud_clapHostUnloadAll(void)
    {
        // Nothing is loaded until the host core lands (plan Phase 2).
    }
}
