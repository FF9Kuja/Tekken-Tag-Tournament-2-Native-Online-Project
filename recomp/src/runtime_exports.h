// Host wrappers around kernel/XAM exports of the ReXGlue runtime.
//
// The recompiled code calls every import as the C symbol __imp__<Name>, bound at link time, so a
// definition compiled into the host replaces the runtime library's for the game (see
// kernel_overrides.cpp). A wrapper that only adds diagnostics, or changes one case of an export, still
// needs the runtime's own function for everything else: FindRuntimeExport looks it up in the runtime
// library by name, which works because that library exports every __imp__ symbol (the generated code
// links against them). TTT2_RUNTIME_ORIGINAL(Name) is the cached lookup a wrapper calls.
//
// TTT2_WRAPPED_EXPORT(Name) { ... } defines a wrapper and registers it under a small id (the boot
// monitor counts calls per thread and per export with it). Ttt2App checks at start-up that the
// originals of all registered wrappers are found, so a platform where the lookup does not work fails
// at once with a clear log line instead of on the game's first file access.
#pragma once

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ttt2 {

/// An export as the recompiled code calls it: extern "C" void __imp__<Name>(PPCContext&, uint8_t*).
using GuestExportFn = void(PPCContext& ctx, std::uint8_t* base);

/// The runtime library's own definition of `symbol` (an `__imp__<Name>` export), or null when no
/// module other than the host executable defines it. Linux and macOS: dlsym(RTLD_NEXT), the next
/// definition after the executable's own. Windows: the first loaded module other than the executable
/// that exports the name.
GuestExportFn* FindRuntimeExport(const char* symbol);

/// FindRuntimeExport for a symbol a host wrapper cannot work without: when it is missing, logs a
/// critical line and aborts (Ttt2App turns that into exit code 4 with the log flushed).
GuestExportFn* RequireRuntimeExport(const char* symbol);

/// Records a host wrapper of `symbol` (static registration, before logging starts) and returns its id.
std::uint32_t RegisterWrappedExport(const char* symbol, GuestExportFn* host);

/// The number of host wrappers registered (ids are 0 .. count - 1).
std::size_t WrappedExportCount();

/// The export name of wrapper `id` without the __imp__ prefix ("?" for an unknown id).
const char* WrappedExportName(std::uint32_t id);

/// The registered wrappers whose runtime original cannot be found (empty when all are found).
std::vector<std::string> MissingRuntimeExports();

}  // namespace ttt2

/// The runtime's original of export `name` (an identifier, without the __imp__ prefix), looked up once.
#define TTT2_RUNTIME_ORIGINAL(name)                                                               \
  ([]() -> ::ttt2::GuestExportFn* {                                                               \
    static ::ttt2::GuestExportFn* const original = ::ttt2::RequireRuntimeExport("__imp__" #name); \
    return original;                                                                              \
  }())

/// Defines the host's __imp__<name> (the body follows the macro) and registers it; the body can use
/// TTT2_EXPORT_ID(name), the wrapper's id.
#define TTT2_WRAPPED_EXPORT(name)                                                     \
  extern "C" REX_FUNC(__imp__##name);                                                 \
  static const std::uint32_t ttt2_export_id_##name =                                  \
      ::ttt2::RegisterWrappedExport("__imp__" #name, &__imp__##name);                 \
  extern "C" REX_FUNC(__imp__##name)

#define TTT2_EXPORT_ID(name) ttt2_export_id_##name
