// make_test_iso: writes an XDVDFS disc image (the format ttt2-iso reads) that holds the given files. No game
// data is involved: this exists so the SDK workflow can run the owner's whole pipeline (scripts/owner-pipeline.ps1:
// identify, extract, prepare, codegen, host build, extraction, boot repair) on a license-free test program
// packed the way the owner's disc carries the game.
//
//   make_test_iso <out.iso> <path on the disc>=<file> [<path on the disc>=<file> ...]
//
// Paths on the disc use '/' (default.xex, data/table.bin). Exit code 0 when the image was written, 1 otherwise.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xdvdfs_builder.hpp"

#include "ttt2/core/file_io.hpp"

namespace {

int fail(const std::string& message) {
  std::fprintf(stderr, "make_test_iso: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: make_test_iso <out.iso> <path on the disc>=<file> [...]\n");
    return 1;
  }
  ttt2::test::XdvdfsBuilder disc;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == arg.size()) return fail("expected <path on the disc>=<file>: " + arg);
    auto data = ttt2::read_file(arg.substr(eq + 1));
    if (!data) return fail(arg.substr(eq + 1) + ": " + data.error().message);
    disc.add_file(arg.substr(0, eq), std::move(*data));
  }
  const auto image = disc.build();
  std::ofstream out(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
  if (!out || !out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()))) {
    return fail(std::string("cannot write ") + argv[1]);
  }
  std::printf("%s: %zu bytes, %d files\n", argv[1], image.size(), argc - 2);
  return 0;
}
