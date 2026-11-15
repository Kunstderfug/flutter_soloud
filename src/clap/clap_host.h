// CLAP plugin host (macOS). Built only with SOLOUD_ENABLE_CLAP; see
// docs/clap_hosting_plan.md. The FFI surface is in src/clap_bindings.cpp.
#pragma once

#ifdef SOLOUD_ENABLE_CLAP

#include <cstdint>

namespace soloud_clap
{
    /// The CLAP version this host was built against (vendored headers).
    uint32_t hostClapVersionMajor();
    uint32_t hostClapVersionMinor();
    uint32_t hostClapVersionRevision();
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
