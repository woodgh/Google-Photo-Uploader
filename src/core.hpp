#pragma once
// 플랫폼 독립 규칙: 대상 확장자, 새 파일 이름, 100개 단위 묶음. (테스트는 tests/core_tests.cpp)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gpu {

inline constexpr size_t kFilesPerFolder = 100;
inline constexpr size_t kHashChars = 8;

struct Stamp {
  int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
  bool operator<(const Stamp& o) const {
    const int a[] = {year, month, day, hour, minute, second};
    const int b[] = {o.year, o.month, o.day, o.hour, o.minute, o.second};
    return std::lexicographical_compare(a, a + 6, b, b + 6);
  }
};

inline std::wstring Lower(std::wstring s) {
  for (auto& c : s)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return s;
}

// ".JPG" → ".jpg". 확장자가 없으면 빈 문자열.
inline std::wstring ExtensionOf(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  const auto dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
  return Lower(path.substr(dot));
}

// Google 포토가 받는 사진·동영상만 옮긴다. 나머지 파일은 제자리에 둔다.
inline bool IsMediaExtension(const std::wstring& ext) {
  static const wchar_t* const kExts[] = {
      L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif", L".tif", L".tiff",
      L".ico", L".avif", L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".raf",
      L".srw", L".mp4", L".m4v", L".mov", L".avi", L".mkv", L".wmv", L".mpg", L".mpeg", L".3gp",
      L".3g2", L".mts", L".m2ts", L".asf", L".flv", L".webm"};
  const auto e = Lower(ext);
  for (const auto* k : kExts)
    if (e == k) return true;
  return false;
}

inline std::wstring ToHex(const uint8_t* data, size_t len) {
  static const wchar_t kDigits[] = L"0123456789abcdef";
  std::wstring out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 0xF];
  }
  return out;
}

// 년_월_일_시_해쉬.확장자  예) 2024_05_01_13_ab12cd34.jpg
// collision > 0 이면 같은 이름이 이미 있을 때 붙이는 번호: 2024_05_01_13_ab12cd34_2.jpg
inline std::wstring MakeFileName(const Stamp& s, const std::wstring& hashHex, const std::wstring& ext, int collision = 0) {
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d_%02d_%02d_%02d_", s.year, s.month, s.day, s.hour);
  std::wstring name = buf + Lower(hashHex.substr(0, kHashChars));
  if (collision > 0) name += L"_" + std::to_wstring(collision + 1);
  return name + Lower(ext);
}

// 0 → "001", 1 → "002" ...
inline std::wstring ChunkFolderName(size_t chunkIndex) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%03zu", chunkIndex + 1);
  return buf;
}

inline size_t ChunkCount(size_t files) { return (files + kFilesPerFolder - 1) / kFilesPerFolder; }

// path가 dir 안(또는 dir 자체)인지. 대소문자·구분자 무시.
inline bool IsUnder(const std::wstring& path, const std::wstring& dir) {
  auto norm = [](std::wstring s) {
    s = Lower(s);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
  };
  const auto p = norm(path), d = norm(dir);
  if (d.empty() || p.size() < d.size() || p.compare(0, d.size(), d) != 0) return false;
  return p.size() == d.size() || p[d.size()] == L'\\';
}

}  // namespace gpu
