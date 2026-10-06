#include "core.hpp"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                  \
    }                                                              \
  } while (0)

int main() {
  using namespace gpu;

  CHECK(ExtensionOf(L"C:\\a\\IMG_0001.JPG") == L".jpg");
  CHECK(ExtensionOf(L"C:\\a.b\\noext").empty());
  CHECK(ExtensionOf(L"photo").empty());

  CHECK(IsMediaExtension(L".HEIC"));
  CHECK(IsMediaExtension(L".mov"));
  CHECK(!IsMediaExtension(L".txt"));
  CHECK(!IsMediaExtension(L""));

  const uint8_t bytes[] = {0xab, 0x12, 0xcd, 0x34, 0xff};
  CHECK(ToHex(bytes, 5) == L"ab12cd34ff");

  Stamp s{2024, 5, 1, 13, 7, 9};
  CHECK(MakeFileName(s, L"AB12CD34EF56", L".JPG") == L"2024_05_01_13_ab12cd34.jpg");
  CHECK(MakeFileName(s, L"ab12cd34ef56", L".mp4", 1) == L"2024_05_01_13_ab12cd34_2.mp4");
  CHECK(MakeFileName(s, L"ab12cd34", L"") == L"2024_05_01_13_ab12cd34");

  CHECK((Stamp{2024, 5, 1, 13, 0, 0} < Stamp{2024, 5, 1, 13, 0, 1}));
  CHECK(!(Stamp{2025, 1, 1, 0, 0, 0} < Stamp{2024, 12, 31, 23, 59, 59}));

  CHECK(ChunkFolderName(0) == L"001");
  CHECK(ChunkFolderName(99) == L"100");
  CHECK(ChunkCount(0) == 0);
  CHECK(ChunkCount(100) == 1);
  CHECK(ChunkCount(101) == 2);
  CHECK(ChunkCount(250) == 3);

  CHECK(IsUnder(L"C:\\Out\\001\\a.jpg", L"c:\\out\\"));
  CHECK(IsUnder(L"C:\\Out", L"C:\\Out"));
  CHECK(!IsUnder(L"C:\\Output\\a.jpg", L"C:\\Out"));
  CHECK(!IsUnder(L"C:\\a.jpg", L""));

  if (g_failed) return 1;
  std::printf("core tests passed\n");
  return 0;
}
