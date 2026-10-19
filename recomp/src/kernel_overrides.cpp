// Host-side overrides of kernel/XAM exports that the pinned ReXGlue runtime only stubs.
//
// Generated code calls an import as the C symbol __imp__<Name>, resolved at link time, so a
// definition compiled into the host wins over the runtime library's stub (ELF interposition on
// Linux; on Windows the import library is consulted only for symbols the host does not define).
// The runtime's REX_EXPORT_STUB logs "<name> STUB" and leaves r3 untouched, which a caller reads
// as a random result; TTT2 Web (CC0), which runs the game to menus and fights on its own runtime,
// implements exactly these five with the results below, so they are the first overrides here.
// Every override logs its first call so the owner's boot log shows which ones the game reached.
// The runtime's GetProcAddressByOrdinal path (XexGetProcedureAddress) still finds the runtime's
// registry entry; the game does not use it for these.
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

namespace {

constexpr std::uint64_t kErrorSuccess = 0;        // ERROR_SUCCESS
constexpr std::uint64_t kErrorNoSuchUser = 1317;  // ERROR_NO_SUCH_USER (0x525)

void NoteFirstCall(std::atomic_flag& once, const char* name, const char* result) {
  if (!once.test_and_set()) REXLOG_INFO("ttt2: kernel override {} -> {} (first call)", name, result);
}

}  // namespace

// Each override: the __imp__ symbol the generated code calls, the r3 result, its description.
#define TTT2_KERNEL_OVERRIDE(name, value, text)      \
  REX_HOOK_RAW(__imp__##name) {                      \
    (void)base;                                      \
    static std::atomic_flag once = ATOMIC_FLAG_INIT; \
    NoteFirstCall(once, #name, text);                \
    ctx.r3.u64 = (value);                            \
  }

// System UI the title may open from its menus: reported as shown and dismissed at once.
TTT2_KERNEL_OVERRIDE(XamShowFriendsUI, kErrorSuccess, "ERROR_SUCCESS, dismissed")
TTT2_KERNEL_OVERRIDE(XamShowGamerCardUIForXUID, kErrorSuccess, "ERROR_SUCCESS, dismissed")
TTT2_KERNEL_OVERRIDE(XamShowMarketplaceUI, kErrorSuccess, "ERROR_SUCCESS, dismissed")
TTT2_KERNEL_OVERRIDE(XamShowMarketplaceDownloadItemsUI, kErrorSuccess, "ERROR_SUCCESS, dismissed")
// Leaderboards need Xbox Live: no such user, as TTT2 Web answers.
TTT2_KERNEL_OVERRIDE(XamUserCreateStatsEnumerator, kErrorNoSuchUser, "ERROR_NO_SUCH_USER")

#undef TTT2_KERNEL_OVERRIDE
