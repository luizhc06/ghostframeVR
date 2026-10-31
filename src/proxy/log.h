// Log em arquivo, thread-safe. Arquivo: ghostframe.log na pasta da DLL.
#pragma once

#include <cstdarg>

namespace gf {

void LogInit();
void Log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

} // namespace gf

#define GF_LOG(...) ::gf::Log(__VA_ARGS__)
