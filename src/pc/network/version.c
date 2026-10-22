#include <stdio.h>
#include "version.h"
#include "types.h"

static char sVersionString[MAX_VERSION_LENGTH] = { 0 };

const char* get_version(void) {
    snprintf(
        sVersionString, MAX_VERSION_LENGTH,
#if defined(SMNET) && defined(VERSION_US)
        "%s-%s", SM64COOPDX_VERSION, SMNET_FORK_TAG
#elif defined(SMNET)
        "%s-%s %s", SM64COOPDX_VERSION, SMNET_FORK_TAG, VERSION_REGION
#elif defined(VERSION_US)
        "%s", SM64COOPDX_VERSION
#else
        "%s %s", SM64COOPDX_VERSION, VERSION_REGION
#endif // VERSION_US
    );
    return sVersionString;
}

#ifdef COMPILE_TIME
const char* get_version_with_build_date(void) {
    snprintf(
        sVersionString, MAX_VERSION_LENGTH,
#if defined(SMNET)
        "%s-%s, %s", SM64COOPDX_VERSION, SMNET_FORK_TAG, COMPILE_TIME
#elif defined(VERSION_US)
        "%s, %s", SM64COOPDX_VERSION, COMPILE_TIME
#else
        "%s %s, %s", SM64COOPDX_VERSION, VERSION_REGION, COMPILE_TIME
#endif // VERSION_US
    );
    return sVersionString;
}
#endif