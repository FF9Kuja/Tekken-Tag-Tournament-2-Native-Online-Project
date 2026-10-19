// ttt2-stfs: inspects an Xbox 360 STFS package (title updates, "LIVE"/"PIRS"/"CON" files).
//
//   ttt2-stfs info <package>                      title, content type, versions, file list
//   ttt2-stfs extract <package> <name> <out-file> copies one contained file out
//   ttt2-stfs identify <package>                  for a title update: finds the .xexp and reports
//                                                 its source -> target version and hashes
// Exit code 0 on success, 1 on error.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/core/project_version.hpp"
#include "ttt2/stfs/stfs.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: ttt2-stfs info <package>\n"
               "       ttt2-stfs extract <package> <name> <out-file>\n"
               "       ttt2-stfs identify <package>\n"
               "       ttt2-stfs --version\n");
}

int fail(const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  return 1;
}

const char* magic_name(ttt2::stfs::Magic m) {
  switch (m) {
    case ttt2::stfs::Magic::Live:
      return "LIVE (Xbox Live signed)";
    case ttt2::stfs::Magic::Pirs:
      return "PIRS (Microsoft signed)";
    case ttt2::stfs::Magic::Con:
      return "CON (console signed)";
  }
  return "?";
}

int cmd_info(ttt2::stfs::Package& pkg) {
  const auto& i = pkg.info();
  const auto name = ttt2::xex::known_title_name(i.title_id);
  std::printf("Package:            %s\n", magic_name(i.magic));
  std::printf("Content type:       0x%08X%s\n", i.content_type,
              i.is_title_update() ? "  (title update)" : "");
  std::printf("Title ID:           %s%s%s\n", ttt2::hex_u32(i.title_id).c_str(), name ? "  " : "",
              name ? std::string(*name).c_str() : "");
  std::printf("Display name:       %s\n", i.display_name.c_str());
  std::printf("Version:            %s (base %s)\n",
              ttt2::xex::Version::from_u32(i.version).to_string().c_str(),
              ttt2::xex::Version::from_u32(i.base_version).to_string().c_str());
  std::printf("Media ID:           %s\n", ttt2::hex_u32(i.media_id).c_str());
  std::printf("Blocks:             %u allocated, table shift %u, file table at block %u (%u blocks)\n",
              i.total_allocated_blocks, i.table_shift, i.file_table_block, i.file_table_block_count);
  const auto entries = pkg.list();
  if (!entries) return fail(entries.error().message);
  std::printf("Files:              %zu\n", entries->size());
  for (std::size_t k = 0; k < entries->size(); ++k) {
    const auto& e = (*entries)[k];
    std::printf("  %s %10u  block %-6u %s\n", e.is_directory ? "d" : "-", e.size, e.start_block,
                pkg.path_of(*entries, k).c_str());
  }
  return 0;
}

int cmd_extract(ttt2::stfs::Package& pkg, std::string_view name, const std::string& out_path) {
  const auto entry = pkg.find(name);
  if (!entry) return fail(entry.error().message);
  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  if (!out) return fail("cannot create " + out_path);
  ttt2::hash::Sha1 h;
  const auto n = pkg.stream_file(*entry, [&](std::span<const std::uint8_t> chunk) {
    h.update(chunk);
    return std::fwrite(chunk.data(), 1, chunk.size(), out) == chunk.size();
  });
  std::fclose(out);
  if (!n) return fail(n.error().message);
  std::printf("extracted %s (%llu bytes) -> %s\nSHA-1: %s\n", entry->name.c_str(),
              static_cast<unsigned long long>(*n), out_path.c_str(), ttt2::hash::to_hex(h.finish()).c_str());
  return 0;
}

int cmd_identify(ttt2::stfs::Package& pkg) {
  const int rc = cmd_info(pkg);
  if (rc != 0) return rc;
  const auto entries = pkg.list();
  if (!entries) return fail(entries.error().message);
  for (const auto& e : *entries) {
    if (e.is_directory || e.name.size() < 5) continue;
    const std::string ext = e.name.substr(e.name.size() - 5);
    if (ext != ".xexp" && ext != ".XEXP" && ext != ".Xexp") continue;
    const auto data = pkg.read_file(e, 64u * 1024 * 1024);
    if (!data) return fail(data.error().message);
    const auto parsed = ttt2::xex::parse(*data);
    std::printf("Patch file:         %s (%u bytes)\n", e.name.c_str(), e.size);
    if (!parsed) {
      std::printf("                    does not parse as XEX2: %s\n", parsed.error().message.c_str());
      continue;
    }
    if (parsed->delta_patch) {
      std::printf("Patch versions:     %s -> %s\n", parsed->delta_patch->source_version.to_string().c_str(),
                  parsed->delta_patch->target_version.to_string().c_str());
      if (parsed->execution_info && parsed->execution_info->title_id == 0x4E4D0859u &&
          parsed->delta_patch->target_version == ttt2::xex::kTtt2TitleUpdate3Version) {
        std::printf("Identified:         Tekken Tag Tournament 2 Title Update 3 (target 0.0.3.2)\n");
      }
    }
    std::printf("Patch SHA-1:        %s\n", ttt2::hash::to_hex(ttt2::hash::Sha1::compute(*data)).c_str());
    std::printf("Patch SHA-256:      %s\n", ttt2::hash::to_hex(ttt2::hash::Sha256::compute(*data)).c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "-h" || args[0] == "--help") {
    usage();
    return args.empty() ? 1 : 0;
  }
  if (args[0] == "--version") {
    std::printf("ttt2-stfs %s (%s)\n", ttt2::kProjectVersion, ttt2::kProjectGitDescribe);
    return 0;
  }
  if (args.size() < 2) {
    usage();
    return 1;
  }
  auto source = ttt2::FileSource::open(std::string(args[1]));
  if (!source) return fail(source.error().message);
  auto pkg = ttt2::stfs::Package::open(**source);
  if (!pkg) return fail(pkg.error().message);
  if (args[0] == "info") return cmd_info(*pkg);
  if (args[0] == "identify") return cmd_identify(*pkg);
  if (args[0] == "extract") {
    if (args.size() != 4) {
      usage();
      return 1;
    }
    return cmd_extract(*pkg, args[2], std::string(args[3]));
  }
  usage();
  return 1;
}
