#include "aer_thread.h"

#ifdef AER_HEAP_REF_TLS
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

unsigned int aer_thread_hardware_workers(void) {
#ifdef AER_HEAP_REF_TLS
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned int n = (unsigned int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 1 ? (unsigned int)n : 1u;
#else
    return 1u;
#endif
}
