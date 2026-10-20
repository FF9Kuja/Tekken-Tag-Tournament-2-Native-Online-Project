#include "runtime_exports.h"

#include <rex/logging.h>

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: tlhelp32.h needs its types.
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#endif

namespace ttt2 {
namespace {

struct Wrapped {
  const char* symbol;  // __imp__<Name>
  GuestExportFn* host;
};

std::vector<Wrapped>& Registry() {
  static std::vector<Wrapped> registry;
  return registry;
}

}  // namespace

GuestExportFn* FindRuntimeExport(const char* symbol) {
#if defined(_WIN32)
  // The runtime DLL exports every symbol (WINDOWS_EXPORT_ALL_SYMBOLS); the executable's own definition
  // is the host wrapper, so it is skipped.
  const HMODULE exe = GetModuleHandleW(nullptr);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  GuestExportFn* found = nullptr;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Module32FirstW(snapshot, &entry); more && !found; more = Module32NextW(snapshot, &entry)) {
    if (entry.hModule == exe) continue;
    if (FARPROC proc = GetProcAddress(entry.hModule, symbol)) found = reinterpret_cast<GuestExportFn*>(proc);
  }
  CloseHandle(snapshot);
  return found;
#else
  // RTLD_NEXT: the next definition in the search order after the object this code is in (the
  // executable), which is the runtime library's.
  return reinterpret_cast<GuestExportFn*>(dlsym(RTLD_NEXT, symbol));
#endif
}

GuestExportFn* RequireRuntimeExport(const char* symbol) {
  GuestExportFn* original = FindRuntimeExport(symbol);
  if (!original) {
    REXLOG_CRITICAL("ttt2: the runtime library has no {}: the host's wrapper of it cannot call the original", symbol);
    rex::FlushLogging();
    std::abort();
  }
  return original;
}

std::uint32_t RegisterWrappedExport(const char* symbol, GuestExportFn* host) {
  Registry().push_back({symbol, host});
  return static_cast<std::uint32_t>(Registry().size() - 1);
}

std::size_t WrappedExportCount() {
  return Registry().size();
}

const char* WrappedExportName(std::uint32_t id) {
  if (id >= Registry().size()) return "?";
  const char* symbol = Registry()[id].symbol;
  constexpr char kPrefix[] = "__imp__";
  for (std::size_t i = 0; i + 1 < sizeof(kPrefix); ++i) {
    if (symbol[i] != kPrefix[i]) return symbol;
  }
  return symbol + sizeof(kPrefix) - 1;
}

std::vector<std::string> MissingRuntimeExports() {
  std::vector<std::string> missing;
  for (const auto& wrapped : Registry()) {
    GuestExportFn* original = FindRuntimeExport(wrapped.symbol);
    if (!original || original == wrapped.host) missing.emplace_back(wrapped.symbol);
  }
  return missing;
}

}  // namespace ttt2
