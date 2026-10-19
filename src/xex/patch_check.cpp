#include "ttt2/xex/patch_check.hpp"

#include <cstdio>

#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"

namespace ttt2::xex {

namespace {

std::string hex32(std::uint32_t v) {
  return "0x" + hex_u32(v);
}

}  // namespace

PatchCompatibility check_patch_compatibility(const Xex2Image& base, std::span<const std::uint8_t> base_file,
                                             const Xex2Image& patch) {
  PatchCompatibility result;
  const auto fail = [&](std::string message) {
    result.errors.push_back(std::move(message));
    result.compatible = false;
  };
  const auto note = [&](std::string message) { result.notes.push_back(std::move(message)); };

  // --- the patch file ---
  if (!patch.is_patch() && !patch.is_delta_patch()) {
    fail("the second file is not a title update: its module flags lack MODULE_PATCH / PATCH_DELTA (" +
         describe_module_flags(patch.module_flags) + ")");
  }
  if (!patch.delta_patch) fail("the patch has no delta patch descriptor");
  if (!patch.file_format) {
    fail("the patch has no file format info");
  } else if (patch.file_format->compression != CompressionType::Delta) {
    fail("the patch's compression type is " + std::string(compression_name(patch.file_format->compression)) +
         ", not delta");
  }
  if (!patch.execution_info) fail("the patch has no execution info (title ID / version)");

  // --- the base file ---
  if (base.is_patch() || base.is_delta_patch())
    fail("the first file is itself a patch, not a base executable");
  if (!base.security) fail("the base executable has no readable security info");
  if (!base.execution_info) fail("the base executable has no execution info (title ID / version)");
  if (!base.file_format) {
    fail("the base executable has no file format info");
  } else {
    if (base.file_format->encryption != EncryptionType::None &&
        base.file_format->encryption != EncryptionType::Normal) {
      fail("the base executable uses an unsupported encryption type");
    }
    if (base.file_format->compression == CompressionType::Delta) {
      fail("the base executable is delta-compressed (is it a patch?)");
    }
  }

  // --- the pair ---
  if (base.execution_info && patch.execution_info) {
    const auto& b = *base.execution_info;
    const auto& p = *patch.execution_info;
    if (b.title_id != p.title_id) {
      fail("title ID mismatch: base " + hex_u32(b.title_id) + ", patch " + hex_u32(p.title_id));
    }
    if (patch.delta_patch) {
      const auto& d = *patch.delta_patch;
      if (d.source_version != b.version) {
        fail("version mismatch: the patch applies to version " + d.source_version.to_string() +
             " but the base executable is version " + b.version.to_string());
      }
      note("patch " + d.source_version.to_string() + " -> " + d.target_version.to_string() + " (base " +
           b.version.to_string() + ", patch module " + p.version.to_string() + ")");
    }
    if (b.media_id != p.media_id) {
      note("media ID differs (base " + hex_u32(b.media_id) + ", patch " + hex_u32(p.media_id) +
           "); title updates are not bound to a media ID, so this is informational");
    }
  }

  // --- descriptor windows against the base header ---
  if (patch.delta_patch && patch.delta_patch->has_delta_windows) {
    const auto& d = *patch.delta_patch;
    if (d.delta_headers_source_offset > base.header_size) {
      fail("the patch reads base headers at " + hex32(d.delta_headers_source_offset) +
           ", beyond the base header size " + hex32(base.header_size));
    } else if (d.delta_headers_source_size > base.header_size - d.delta_headers_source_offset) {
      fail("the patch reads " + hex32(d.delta_headers_source_size) + " bytes of base headers at " +
           hex32(d.delta_headers_source_offset) + ", past the base header size " + hex32(base.header_size));
    }
    if (d.size_of_target_headers != 0) {
      if (d.delta_headers_target_offset > d.size_of_target_headers) {
        fail("the patch's header target offset " + hex32(d.delta_headers_target_offset) +
             " lies beyond its target header size " + hex32(d.size_of_target_headers));
      } else if (d.delta_headers_source_size > d.size_of_target_headers - d.delta_headers_target_offset) {
        fail("the patch's header window does not fit its target header size " +
             hex32(d.size_of_target_headers));
      }
      note("patched header size " + hex32(d.size_of_target_headers) + " (base " + hex32(base.header_size) +
           ")");
    }
  } else if (patch.delta_patch) {
    note("short delta descriptor (" + std::to_string(patch.delta_patch->descriptor_size) +
         " bytes): header windows not present, bounds not checked");
  }

  // --- what the descriptor's source digest matches (informational; the format does not say) ---
  if (patch.delta_patch && base.security) {
    const auto& digest = patch.delta_patch->source_digest;
    const auto& s = *base.security;
    std::string match;
    if (digest == s.header_digest) {
      match = "the base's security header digest";
    } else if (digest == s.section_digest) {
      match = "the base's section digest";
    } else if (digest == s.import_table_digest) {
      match = "the base's import table digest";
    } else if (base.header_size <= base_file.size()) {
      if (digest == hash::Sha1::compute(base_file.subspan(0, base.header_size))) {
        match = "SHA-1 of the base's headers [0, header_size)";
      } else if (digest == hash::Sha1::compute(base_file)) {
        match = "SHA-1 of the whole base file";
      } else if (base.security_offset < base.header_size &&
                 digest == hash::Sha1::compute(base_file.subspan(base.security_offset,
                                                                 base.header_size - base.security_offset))) {
        match = "SHA-1 of the base's security info and page descriptors";
      }
    }
    note("patch source digest " + hash::to_hex(digest) +
         (match.empty() ? " matches none of the base digests tried (informational)" : " matches " + match));
  }
  return result;
}

}  // namespace ttt2::xex
