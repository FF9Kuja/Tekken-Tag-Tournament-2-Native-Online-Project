// ttt2-xexpatch: applies a title-update delta patch (.xexp) to a base executable (.xex) and
// writes the patched executable with its image decrypted and decompressed (no encryption, no
// compression in the output headers), which is what the recompiler consumes.
//
//   ttt2-xexpatch <base.xex> <patch.xexp> <out.xex>
//
// The patcher is XenonUtils' XexPatcher (see CMakeLists.txt for licensing). Both inputs are the
// player's own files; the output stays on the player's disk and is never redistributed.

#include <xex_patcher.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/file_io.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/core/project_version.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

const char* result_name(XexPatcher::Result r) {
  switch (r) {
    case XexPatcher::Result::Success:
      return "success";
    case XexPatcher::Result::FileOpenFailed:
      return "file open failed";
    case XexPatcher::Result::FileWriteFailed:
      return "file write failed";
    case XexPatcher::Result::XexFileUnsupported:
      return "XEX file unsupported";
    case XexPatcher::Result::XexFileInvalid:
      return "XEX file invalid";
    case XexPatcher::Result::PatchFileInvalid:
      return "patch file invalid (not a .xexp?)";
    case XexPatcher::Result::PatchIncompatible:
      return "patch incompatible with this base executable (wrong version?)";
    case XexPatcher::Result::PatchFailed:
      return "patch failed";
    case XexPatcher::Result::PatchUnsupported:
      return "patch unsupported";
  }
  return "unknown";
}

int fail(const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  return 1;
}

void describe(const char* label, const ttt2::xex::Xex2Image& img) {
  if (img.execution_info) {
    const auto& e = *img.execution_info;
    const auto name = ttt2::xex::known_title_name(e.title_id);
    std::printf("%s title %s%s%s, version %s (base %s)\n", label, ttt2::hex_u32(e.title_id).c_str(),
                name ? " " : "", name ? std::string(*name).c_str() : "", e.version.to_string().c_str(),
                e.base_version.to_string().c_str());
  }
  if (img.delta_patch) {
    std::printf("%s delta patch %s -> %s\n", label, img.delta_patch->source_version.to_string().c_str(),
                img.delta_patch->target_version.to_string().c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::printf("ttt2-xexpatch %s (%s)\n", ttt2::kProjectVersion, ttt2::kProjectGitDescribe);
    return 0;
  }
  if (argc != 4) {
    std::fprintf(stderr, "usage: ttt2-xexpatch <base.xex> <patch.xexp> <out.xex>\n");
    return 1;
  }
  auto base = ttt2::read_file(argv[1]);
  if (!base) return fail(base.error().message);
  auto patch = ttt2::read_file(argv[2]);
  if (!patch) return fail(patch.error().message);

  const auto base_info = ttt2::xex::parse(base.value());
  if (!base_info) return fail(std::string("base: ") + base_info.error().message);
  describe("base: ", base_info.value());
  const auto patch_info = ttt2::xex::parse(patch.value());
  if (!patch_info) return fail(std::string("patch: ") + patch_info.error().message);
  describe("patch:", patch_info.value());
  if (!patch_info->is_delta_patch()) return fail("the second file is not a delta patch (.xexp)");
  if (base_info->execution_info && patch_info->execution_info &&
      base_info->execution_info->title_id != patch_info->execution_info->title_id) {
    return fail("title ID mismatch between the base executable and the patch");
  }

  std::vector<std::uint8_t> out;
  const auto result = XexPatcher::apply(base->data(), base->size(), patch->data(), patch->size(), out, false);
  if (result != XexPatcher::Result::Success)
    return fail(std::string("patching failed: ") + result_name(result));

  std::ofstream stream(argv[3], std::ios::binary);
  if (!stream ||
      !stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
    return fail(std::string("cannot write ") + argv[3]);
  }
  stream.close();

  const auto out_info = ttt2::xex::parse(out);
  if (!out_info) return fail(std::string("patched output does not parse: ") + out_info.error().message);
  describe("output:", out_info.value());
  std::printf("output: %zu bytes -> %s\n", out.size(), argv[3]);
  std::printf("output SHA-1: %s\n", ttt2::hash::to_hex(ttt2::hash::Sha1::compute(out)).c_str());
  const auto image = ttt2::xex::load_basefile(out_info.value(), out);
  if (image) {
    std::printf("image: %s, %zu bytes, SHA-1 %s\n",
                ttt2::xex::looks_like_pe_image(*image) ? "PE image" : "NOT a PE image", image->size(),
                ttt2::hash::to_hex(ttt2::hash::Sha1::compute(*image)).c_str());
  } else {
    std::printf("image: not produced: %s\n", image.error().message.c_str());
  }
  return 0;
}
