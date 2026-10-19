#include <doctest.h>

#include <string>

#include "stub_name.hpp"

TEST_CASE("the runtime's stub log lines yield the export name") {
  CHECK(ttt2::stub_name("__imp__NtClose STUB") == "NtClose");
  CHECK(ttt2::stub_name("__imp__XamShowSigninUI STUB - returning 0x0") == "XamShowSigninUI");
  CHECK(ttt2::stub_name("__imp__KeQueryPerformanceFrequency STUB - not needed") ==
        "KeQueryPerformanceFrequency");
  CHECK(ttt2::stub_name("NtClose STUB") == "NtClose");  // tolerate a missing prefix
  CHECK(ttt2::stub_name("STUB").empty());
  CHECK(ttt2::stub_name(" STUB").empty());
  CHECK(ttt2::stub_name("ttt2: watchdog: STUB count").empty());  // the token is not a lone name
  CHECK(ttt2::stub_name("__imp__NtClose STUBBED").empty());
  CHECK(ttt2::stub_name("Variable import xboxkrnl.exe:0xad (KeTimeStampBundle) not implemented").empty());
  CHECK(ttt2::stub_name("").empty());
}
