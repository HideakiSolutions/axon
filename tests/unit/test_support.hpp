#pragma once
// Portability shims for the test suite only. Production code is already
// platform-guarded; the tests use a handful of POSIX calls that MSVC either
// spells differently (getpid) or does not provide at all (setenv/unsetenv).
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace testsupport {

inline int pid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

inline void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    ::_putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

// _putenv_s with an empty value removes the variable from the CRT
// environment, matching POSIX unsetenv for everything getenv sees.
inline void unset_env(const char* name) {
#ifdef _WIN32
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

} // namespace testsupport
