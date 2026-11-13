// coop_mod_plugins.h Guarded: on its own, because a function with __try may
// hold no C++ object that needs unwinding.
#include "coop_mod_plugins.h"

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CoopModPlugins {

#ifdef _MSC_VER
namespace {
// A stack overflow is not caught: the thread has no stack left to go on with.
int Filter(unsigned long code) {
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

bool Guarded(void (*fn)(void*), void* arg) {
    __try {
        fn(arg);
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
    return true;
}
#else
bool Guarded(void (*fn)(void*), void* arg) {
    try {
        fn(arg);
    } catch (...) {
        return false;
    }
    return true;
}
#endif

}  // namespace CoopModPlugins
