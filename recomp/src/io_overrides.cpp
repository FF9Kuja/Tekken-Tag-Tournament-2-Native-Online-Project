// Host wrappers of the runtime's file-system exports: the one answer TTT2 needs that differs from the
// runtime's (the disc sector query), and a trace of what the game opens and reads.
//
// The sector query. NtQueryInformationFile with FileSectorInformation (class 26) asks for the disc
// sector a file starts at. ReXGlue 0.10.0 answers it with success and a made-up number (a hash of the
// path: some titles only use it to tell files apart). TTT2's loader uses the answer to read its
// archives by raw sector through the disc device when that query works, and ReXGlue mounts no disc
// device (the game folder is \Device\Harddisk0\Partition1, behind game: and d:). TTT2 Web (CC0), which
// runs the game to menus and fights, answers the query as unsupported (STATUS_INVALID_INFO_CLASS) and
// the loader falls back to ordinary file reads; the host does the same. TTT2_SECTOR_QUERY=runtime keeps
// the runtime's answer, for comparing the two on the owner's machine.
//
// The trace. The owner's first boot logged one error, "ResolvePath(\Device) failed - device not
// found": the runtime's message when an open names a bare device path (\Device\<name>), whose parent
// \Device is not a mount. Every open and read now feeds the boot monitor (counts, the recent events
// for the stall dump) and the log: each distinct path that opens, each distinct failure with its
// NTSTATUS, caller and thread (the first 64 of each), the first read failures and device controls.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "boot_monitor.h"
#include "runtime_exports.h"

namespace {

using ttt2::monitor::counters;
using ttt2::monitor::FormatStatus;

constexpr std::uint32_t kFileSectorInformation = 26;     // X_FILE_INFORMATION_CLASS::XFileSectorInformation
constexpr std::uint32_t kStatusInvalidInfoClass = 0xC0000003;
constexpr std::uint32_t kStatusEndOfFile = 0xC0000011;
constexpr std::uint32_t kStatusPending = 0x00000103;
constexpr std::uint32_t kObDosDevices = 0xFFFFFFFD;  // OBJECT_ATTRIBUTES.RootDirectory: the DOS device namespace
constexpr std::size_t kMaxLoggedPaths = 64;
constexpr std::uint64_t kMaxLoggedReadFailures = 16;
constexpr std::uint64_t kMaxLoggedDeviceControls = 16;

bool Succeeded(std::uint32_t status) {
  return (status & 0x80000000u) == 0;
}

bool SectorQueryUnsupported() {
  static const bool unsupported = [] {
    const char* value = std::getenv("TTT2_SECTOR_QUERY");
    return !(value && std::string_view(value) == "runtime");
  }();
  return unsupported;
}

// --- guest arguments (pointers the game passed in; valid for the duration of the call) -------------

rex::memory::Memory* GuestMemory() {
  return rex::system::kernel_memory();
}

std::uint32_t LoadU32(std::uint32_t address) {
  const auto* p = GuestMemory()->TranslateVirtual<const std::uint8_t*>(address);
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

std::uint16_t LoadU16(std::uint32_t address) {
  const auto* p = GuestMemory()->TranslateVirtual<const std::uint8_t*>(address);
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

/// An ANSI_STRING { u16 length, u16 maximum, u32 buffer } as text (printable characters only).
std::string ReadAnsiString(std::uint32_t ansi) {
  if (!ansi) return "(null name)";
  const std::uint16_t length = LoadU16(ansi);
  const std::uint32_t buffer = LoadU32(ansi + 4);
  if (!buffer || !length) return "";
  const auto* text = GuestMemory()->TranslateVirtual<const char*>(buffer);
  std::string out;
  out.reserve(length < 512 ? length : 512);
  for (std::uint16_t i = 0; i < length && i < 512; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  return out;
}

/// The path an open file handle refers to, as the runtime resolved it.
std::string HandlePath(std::uint32_t handle) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return "?";
  auto file = kernel->object_table()->LookupObject<rex::system::XFile>(handle);
  if (!file) return fmt::format("handle 0x{:08X} (not a file)", handle);
  return file->entry() ? file->entry()->absolute_path() : file->path();
}

/// The path an OBJECT_ATTRIBUTES names, with the directory it is relative to (if any) in front.
std::string ObjectPath(std::uint32_t attributes) {
  if (!attributes) return "(no object attributes)";
  const std::uint32_t root = LoadU32(attributes);
  std::string name = ReadAnsiString(LoadU32(attributes + 4));
  if (root && root != kObDosDevices) name = HandlePath(root) + "\\" + name;
  return name;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

/// \Device\<name> with nothing after the device name: the device itself, not a file on it.
bool IsBareDevicePath(std::string_view path) {
  constexpr std::string_view kDevice = "\\Device\\";
  if (!StartsWithNoCase(path, kDevice)) return false;
  std::string_view rest = path.substr(kDevice.size());
  if (!rest.empty() && rest.back() == '\\') rest.remove_suffix(1);
  return !rest.empty() && rest.find('\\') == std::string_view::npos;
}

// --- logging each distinct path once -------------------------------------------------------------

struct PathLog {
  std::mutex mutex;
  std::set<std::string> seen;
  bool full_noted = false;
};

/// True when `key` has not been logged before and the log still has room (kMaxLoggedPaths keys).
bool FirstTime(PathLog& log, const std::string& key, const char* what) {
  std::lock_guard<std::mutex> lock(log.mutex);
  if (log.seen.count(key)) return false;
  if (log.seen.size() >= kMaxLoggedPaths) {
    if (!log.full_noted) {
      log.full_noted = true;
      REXLOG_INFO("ttt2: {}: {} distinct paths logged; further ones are only counted (watchdog activity line)",
                  what, kMaxLoggedPaths);
    }
    return false;
  }
  log.seen.insert(key);
  return true;
}

PathLog& OpenedPaths() {
  static PathLog log;
  return log;
}

PathLog& FailedPaths() {
  static PathLog log;
  return log;
}

PathLog& MissingAttributePaths() {
  static PathLog log;
  return log;
}

void NoteOpen(const char* api, const std::string& path, std::uint32_t status, std::uint32_t lr) {
  ttt2::monitor::NoteFileEvent(api, path, status, lr);
  auto& c = counters();
  if (Succeeded(status)) {
    c.files_opened.fetch_add(1, std::memory_order_relaxed);
    if (FirstTime(OpenedPaths(), path, "file opened")) {
      REXLOG_INFO("ttt2: file opened: '{}' ({}, {})", path, api, ttt2::monitor::CurrentThreadLabel());
    }
    return;
  }
  c.open_failures.fetch_add(1, std::memory_order_relaxed);
  const bool device = IsBareDevicePath(path);
  if (device) {
    c.device_open_failures.fetch_add(1, std::memory_order_relaxed);
    static std::atomic_flag explained = ATOMIC_FLAG_INIT;
    if (!explained.test_and_set()) {
      REXLOG_WARN(
          "ttt2: the game opened '{}', a device itself rather than a file on it; ReXGlue mounts the game folder "
          "at \\Device\\Harddisk0\\Partition1 (game:, d:) and has no device at this path, so the open fails and "
          "the runtime logs \"ResolvePath(\\Device) failed - device not found\" (it resolves the parent, \\Device). "
          "A game opens its disc device to read raw sectors.",
          path);
    }
  }
  if (FirstTime(FailedPaths(), path + "|" + FormatStatus(status), "file open failed")) {
    REXLOG_INFO("ttt2: file open failed: '{}' -> {} ({}, {}, from {})", path, FormatStatus(status), api,
                ttt2::monitor::CurrentThreadLabel(), ttt2::monitor::DescribeGuestAddress(lr));
  }
}

}  // namespace

// NtQueryInformationFile(handle r3, io_status_block r4, info r5, length r6, class r7).
TTT2_WRAPPED_EXPORT(NtQueryInformationFile) {
  const std::uint32_t handle = ctx.r3.u32;
  const std::uint32_t info_class = ctx.r7.u32;
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtQueryInformationFile), lr);
  if (info_class == kFileSectorInformation && SectorQueryUnsupported()) {
    auto* kernel = rex::system::kernel_state();
    if (kernel && kernel->object_table()->LookupObject<rex::system::XFile>(handle)) {
      // As TTT2 Web: the status only (the I/O status block is left alone, as for any rejected class).
      if (counters().sector_queries.fetch_add(1, std::memory_order_relaxed) == 0) {
        REXLOG_INFO(
            "ttt2: sector query: FileSectorInformation for '{}' answered as unsupported ({}), as TTT2 Web answers "
            "it, so the game reads its files by name rather than by disc sector (first query, from {}; later ones "
            "are counted; TTT2_SECTOR_QUERY=runtime keeps the runtime's made-up sector number)",
            HandlePath(handle), FormatStatus(kStatusInvalidInfoClass), ttt2::monitor::DescribeGuestAddress(lr));
      }
      ctx.r3.u64 = kStatusInvalidInfoClass;
      return;
    }
  }
  TTT2_RUNTIME_ORIGINAL(NtQueryInformationFile)(ctx, base);
}

// NtCreateFile(handle out r3, access r4, object attributes r5, io_status_block r6, allocation size r7,
//              attributes r8, share r9, disposition r10, options on the stack).
TTT2_WRAPPED_EXPORT(NtCreateFile) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtCreateFile), lr);
  const std::string path = ObjectPath(ctx.r5.u32);
  TTT2_RUNTIME_ORIGINAL(NtCreateFile)(ctx, base);
  NoteOpen("NtCreateFile", path, ctx.r3.u32, lr);
}

// NtOpenFile(handle out r3, access r4, object attributes r5, io_status_block r6, options r7).
TTT2_WRAPPED_EXPORT(NtOpenFile) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtOpenFile), lr);
  const std::string path = ObjectPath(ctx.r5.u32);
  TTT2_RUNTIME_ORIGINAL(NtOpenFile)(ctx, base);
  NoteOpen("NtOpenFile", path, ctx.r3.u32, lr);
}

// NtReadFile(handle r3, event r4, apc routine r5, apc context r6, io_status_block r7, buffer r8,
//            length r9, byte offset pointer r10).
TTT2_WRAPPED_EXPORT(NtReadFile) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtReadFile), lr);
  const std::uint32_t handle = ctx.r3.u32;
  const std::uint32_t io_status_block = ctx.r7.u32;
  const std::uint32_t length = ctx.r9.u32;
  const std::uint32_t offset_ptr = ctx.r10.u32;
  TTT2_RUNTIME_ORIGINAL(NtReadFile)(ctx, base);
  const std::uint32_t status = ctx.r3.u32;
  auto& c = counters();
  if (Succeeded(status) || status == kStatusEndOfFile) {
    c.reads.fetch_add(1, std::memory_order_relaxed);
    if (io_status_block && (status == 0 || status == kStatusPending)) {
      c.bytes_read.fetch_add(LoadU32(io_status_block + 4), std::memory_order_relaxed);
    }
    return;
  }
  const std::uint64_t failures = c.read_failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (failures <= kMaxLoggedReadFailures) {
    const std::string offset =
        offset_ptr ? fmt::format("0x{:X}", (std::uint64_t(LoadU32(offset_ptr)) << 32) | LoadU32(offset_ptr + 4))
                   : std::string("current position");
    REXLOG_INFO("ttt2: file read failed: '{}' at {} length {} -> {} ({}, from {}){}", HandlePath(handle), offset,
                length, FormatStatus(status), ttt2::monitor::CurrentThreadLabel(),
                ttt2::monitor::DescribeGuestAddress(lr),
                failures == kMaxLoggedReadFailures ? "; further read failures are only counted" : "");
  }
}

// NtQueryFullAttributesFile(object attributes r3, network open information r4): "does it exist?".
TTT2_WRAPPED_EXPORT(NtQueryFullAttributesFile) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtQueryFullAttributesFile), lr);
  const std::string path = ObjectPath(ctx.r3.u32);
  TTT2_RUNTIME_ORIGINAL(NtQueryFullAttributesFile)(ctx, base);
  const std::uint32_t status = ctx.r3.u32;
  if (Succeeded(status)) return;
  counters().attribute_failures.fetch_add(1, std::memory_order_relaxed);
  ttt2::monitor::NoteFileEvent("NtQueryFullAttributesFile", path, status, lr);
  if (FirstTime(MissingAttributePaths(), path, "file attributes missing")) {
    REXLOG_INFO("ttt2: file attributes missing: '{}' -> {} ({}, from {})", path, FormatStatus(status),
                ttt2::monitor::CurrentThreadLabel(), ttt2::monitor::DescribeGuestAddress(lr));
  }
}

// NtDeviceIoControlFile(handle r3, event r4, apc routine r5, apc context r6, io_status_block r7,
//                       control code r8, input r9, input length r10, output and its length on the stack).
TTT2_WRAPPED_EXPORT(NtDeviceIoControlFile) {
  const auto lr = static_cast<std::uint32_t>(ctx.lr);
  ttt2::monitor::NoteCall(TTT2_EXPORT_ID(NtDeviceIoControlFile), lr);
  const std::uint32_t handle = ctx.r3.u32;
  const std::uint32_t code = ctx.r8.u32;
  TTT2_RUNTIME_ORIGINAL(NtDeviceIoControlFile)(ctx, base);
  static std::atomic<std::uint64_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedDeviceControls) {
    REXLOG_INFO("ttt2: device control 0x{:08X} on '{}' -> {} ({}, from {})", code, HandlePath(handle),
                FormatStatus(ctx.r3.u32), ttt2::monitor::CurrentThreadLabel(), ttt2::monitor::DescribeGuestAddress(lr));
  }
}
