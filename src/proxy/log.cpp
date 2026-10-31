#include "log.h"

#include "platform.h"

#include <cstdio>
#ifdef _WIN32
#include <process.h>
#define GF_GETPID _getpid
#else
#include <unistd.h>
#define GF_GETPID getpid
#endif
#include <mutex>
#include <string>

namespace gf {

static std::mutex g_mutex;
static FILE *g_file = nullptr;
static double g_t0 = 0.0;

void LogInit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::string self = OwnModulePath();
    size_t slash = self.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "." : self.substr(0, slash);
    std::string path = dir + "/ghostframe.log";
    // Modo append: a DLL é carregada por mais de um processo do SteamVR
    // (vrserver, watchdog...). Cada linha leva o PID.
    g_file = std::fopen(path.c_str(), "a");
    // Pasta do driver em Program Files não é gravável pelo vrserver (sem admin).
    if (!g_file) {
        path = TempDir() + "ghostframe.log";
        g_file = std::fopen(path.c_str(), "a");
    }
    g_t0 = NowSeconds();
    if (g_file) std::fprintf(g_file, "\n===== nova sessao (pid %d) log=%s =====\n", (int)GF_GETPID(), path.c_str());
}

void Log(const char *fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "[%6d %9.3f] ", (int)GF_GETPID(), NowSeconds() - g_t0);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

} // namespace gf
