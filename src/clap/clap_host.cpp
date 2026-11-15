#ifdef SOLOUD_ENABLE_CLAP

#include "clap_host.h"

#include <clap/clap.h>

static_assert(CLAP_VERSION_MAJOR == 1 && CLAP_VERSION_MINOR >= 2,
              "the CLAP host is written against CLAP 1.2");

namespace soloud_clap
{
    uint32_t hostClapVersionMajor() { return CLAP_VERSION_MAJOR; }
    uint32_t hostClapVersionMinor() { return CLAP_VERSION_MINOR; }
    uint32_t hostClapVersionRevision() { return CLAP_VERSION_REVISION; }
} // namespace soloud_clap

#endif // SOLOUD_ENABLE_CLAP
