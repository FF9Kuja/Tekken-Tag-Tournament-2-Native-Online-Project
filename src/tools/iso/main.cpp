// ttt2-iso: inspects an Xbox 360 disc image (XDVDFS) without extracting it.
//
//   ttt2-iso info <image.iso>                      partition offset, root directory, top-level entries
//   ttt2-iso list <image.iso> [directory]          entries of a directory
//   ttt2-iso identify <image.iso> [--image-hash]   finds default.xex, reports title/version/hashes
//   ttt2-iso extract <image.iso> <path> <out-file> copies one file out (to the player's own disk)
//
// Exit code 0 on success, 1 on error.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ttt2/core/byte_source.hpp"
#include "ttt2/core/format.hpp"
#include "ttt2/core/hash.hpp"
#include "ttt2/core/project_version.hpp"
#include "ttt2/xdvdfs/xdvdfs.hpp"
#include "ttt2/xex/basefile.hpp"
#include "ttt2/xex/known_titles.hpp"
#include "ttt2/xex/xex2.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: ttt2-iso info <image.iso>\n"
               "       ttt2-iso list <image.iso> [directory]\n"
               "       ttt2-iso identify <image.iso> [--image-hash]\n"
               "       ttt2-iso extract <image.iso> <path-in-image> <out-file>\n"
               "       ttt2-iso --version\n");
}

int fail(const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  return 1;
}

int cmd_info(ttt2::xdvdfs::Image& disc) {
  const auto& v = disc.volume();
  std::printf("Partition offset:   0x%llX (%s)\n", static_cast<unsigned long long>(v.partition_offset),
              v.partition_offset == 0x2080000ull   ? "XGD3 image"
              : v.partition_offset == 0xFD90000ull ? "XGD2 image"
              : v.partition_offset == 0            ? "extracted game partition"
                                                   : "XGD1 image");
  std::printf("Root directory:     sector %u, %u bytes\n", v.root_sector, v.root_size);
  const auto root = disc.list("/");
  if (!root) return fail(root.error().message);
  std::size_t files = 0, dirs = 0;
  std::uint64_t bytes = 0;
  for (const auto& e : *root) {
    if (e.is_directory())
      ++dirs;
    else {
      ++files;
      bytes += e.size;
    }
  }
  std::printf("Top level:          %zu files (%llu bytes), %zu directories\n", files,
              static_cast<unsigned long long>(bytes), dirs);
  return 0;
}

int cmd_list(ttt2::xdvdfs::Image& disc, std::string_view dir) {
  const auto entries = disc.list(dir);
  if (!entries) return fail(entries.error().message);
  for (const auto& e : *entries) {
    std::printf("%s %12u  sector %-8u %s\n", e.is_directory() ? "d" : "-", e.size, e.start_sector,
                e.name.c_str());
  }
  return 0;
}

int cmd_identify(ttt2::xdvdfs::Image& disc, bool image_hash) {
  const auto entry = disc.find("default.xex");
  if (!entry) return fail("no default.xex in the image root: " + entry.error().message);
  const auto data = disc.read_file(*entry, 64u * 1024 * 1024);
  if (!data) return fail(data.error().message);
  const auto parsed = ttt2::xex::parse(*data);
  if (!parsed) return fail("default.xex: " + parsed.error().message);
  using namespace ttt2::xex;
  std::printf("default.xex:        %u bytes at sector %u\n", entry->size, entry->start_sector);
  if (parsed->execution_info) {
    const auto& e = *parsed->execution_info;
    const auto name = known_title_name(e.title_id);
    std::printf("Title ID:           %s%s%s\n", ttt2::hex_u32(e.title_id).c_str(), name ? "  " : "",
                name ? std::string(*name).c_str() : "");
    std::printf("Version:            %s (base %s)\n", e.version.to_string().c_str(),
                e.base_version.to_string().c_str());
    std::printf("Media ID:           %s\n", ttt2::hex_u32(e.media_id).c_str());
  }
  if (parsed->file_format) {
    std::printf("Encryption:         %s\n",
                std::string(encryption_name(parsed->file_format->encryption)).c_str());
    std::printf("Compression:        %s\n",
                std::string(compression_name(parsed->file_format->compression)).c_str());
  }
  const std::string sha1 = ttt2::hash::to_hex(ttt2::hash::Sha1::compute(*data));
  const std::string sha256 = ttt2::hash::to_hex(ttt2::hash::Sha256::compute(*data));
  std::printf("SHA-1:              %s\n", sha1.c_str());
  std::printf("SHA-256:            %s\n", sha256.c_str());
  if (const auto* known = match_known_executable(sha1, sha256)) {
    std::printf("Matches:            %s\n", std::string(known->description).c_str());
    std::printf("                    (%s)\n", std::string(known->provenance).c_str());
  } else {
    std::printf("Matches:            no known executable (send these hashes with your report)\n");
  }
  if (image_hash) {
    const auto image = load_basefile(*parsed, *data);
    if (!image) {
      std::printf("Image:              not produced: %s\n", image.error().message.c_str());
    } else {
      std::printf("Image:              %s, %zu bytes\n",
                  looks_like_pe_image(*image) ? "PE image" : "NOT a PE image", image->size());
      std::printf("Image SHA-1:        %s\n", ttt2::hash::to_hex(ttt2::hash::Sha1::compute(*image)).c_str());
      std::printf("Image SHA-256:      %s\n",
                  ttt2::hash::to_hex(ttt2::hash::Sha256::compute(*image)).c_str());
    }
  }
  for (const auto& w : parsed->warnings) std::printf("Warning:            %s\n", w.c_str());
  return 0;
}

int cmd_extract(ttt2::xdvdfs::Image& disc, std::string_view path, const std::string& out_path) {
  const auto entry = disc.find(path);
  if (!entry) return fail(entry.error().message);
  if (entry->is_directory()) return fail("cannot extract a directory");
  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  if (!out) return fail("cannot create " + out_path);
  ttt2::hash::Sha1 h;
  const auto n = disc.stream_file(*entry, [&](std::span<const std::uint8_t> chunk) {
    h.update(chunk);
    return std::fwrite(chunk.data(), 1, chunk.size(), out) == chunk.size();
  });
  std::fclose(out);
  if (!n) return fail(n.error().message);
  std::printf("extracted %s (%llu bytes) -> %s\nSHA-1: %s\n", entry->name.c_str(),
              static_cast<unsigned long long>(*n), out_path.c_str(), ttt2::hash::to_hex(h.finish()).c_str());
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
    std::printf("ttt2-iso %s (%s)\n", ttt2::kProjectVersion, ttt2::kProjectGitDescribe);
    return 0;
  }
  if (args.size() < 2) {
    usage();
    return 1;
  }
  const std::string_view command = args[0];
  auto source = ttt2::FileSource::open(std::string(args[1]));
  if (!source) return fail(source.error().message);
  auto disc = ttt2::xdvdfs::Image::open(**source);
  if (!disc) return fail(disc.error().message);

  if (command == "info") return cmd_info(*disc);
  if (command == "list") return cmd_list(*disc, args.size() > 2 ? args[2] : "/");
  if (command == "identify") {
    bool image_hash = false;
    for (std::size_t i = 2; i < args.size(); ++i) image_hash |= args[i] == "--image-hash";
    return cmd_identify(*disc, image_hash);
  }
  if (command == "extract") {
    if (args.size() != 4) {
      usage();
      return 1;
    }
    return cmd_extract(*disc, args[2], std::string(args[3]));
  }
  usage();
  return 1;
}
