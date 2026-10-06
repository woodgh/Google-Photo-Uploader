#include "organizer.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propvarutil.h>
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <set>

#include "core.hpp"

namespace gpu {
namespace {

struct Item {
  std::wstring path;
  FILETIME lastWrite{};
  Stamp stamp;
  std::wstring hash;
};

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty() || dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

std::wstring ErrorText(DWORD code) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                 reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::wstring text = msg ? msg : L"오류 " + std::to_wstring(code);
  if (msg) LocalFree(msg);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
  return text;
}

bool ToLocalStamp(const FILETIME& utc, Stamp& out) {
  SYSTEMTIME st{}, local{};
  if (!FileTimeToSystemTime(&utc, &st) || !SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) return false;
  if (local.wYear < 1980) return false;  // 비어 있거나 잘못된 메타데이터
  out = {local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond};
  return true;
}

// 사진은 EXIF 촬영 시각, 동영상은 인코딩 시각. 없으면 파일 수정 시각.
Stamp ReadStamp(const Item& item) {
  Stamp stamp;
  IPropertyStore* store = nullptr;
  if (SUCCEEDED(SHGetPropertyStoreFromParsingName(item.path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
    bool found = false;
    for (const PROPERTYKEY* key : {&PKEY_Photo_DateTaken, &PKEY_Media_DateEncoded}) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(store->GetValue(*key, &v)) && v.vt == VT_FILETIME) found = ToLocalStamp(v.filetime, stamp);
      PropVariantClear(&v);
      if (found) break;
    }
    store->Release();
    if (found) return stamp;
  }
  ToLocalStamp(item.lastWrite, stamp);
  return stamp;
}

class Sha256 {
 public:
  Sha256() { BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
  ~Sha256() {
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }

  // 파일 내용의 SHA-256 (hex). 실패하면 빈 문자열 + error.
  std::wstring File(const std::wstring& path, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring hex;
    if (alg_ && BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash, nullptr, 0, nullptr, 0, 0))) {
      buffer_.resize(1 << 20);
      DWORD read = 0;
      bool ok = true;
      while ((ok = ReadFile(file, buffer_.data(), static_cast<DWORD>(buffer_.size()), &read, nullptr) != FALSE) && read > 0)
        BCryptHashData(hash, buffer_.data(), read, 0);
      uint8_t digest[32];
      if (!ok)
        error = GetLastError();
      else if (BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)))
        hex = ToHex(digest, sizeof(digest));
      else
        error = ERROR_INVALID_FUNCTION;
      BCryptDestroyHash(hash);
    } else {
      error = ERROR_INVALID_FUNCTION;
    }
    CloseHandle(file);
    return hex;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  std::vector<uint8_t> buffer_;
};

struct Collector {
  std::wstring destRoot;
  std::vector<Item> items;
  std::set<std::wstring> seen;
  size_t skipped = 0;

  void AddFile(const std::wstring& path, const WIN32_FIND_DATAW& fd) {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) return;
    if (wcsncmp(fd.cFileName, L"._", 2) == 0) return;  // macOS가 남기는 메타데이터 파일
    if (IsUnder(path, destRoot)) return;               // 저장 폴더 안의 이미 정리된 파일
    if (!IsMediaExtension(ExtensionOf(path))) {
      ++skipped;
      return;
    }
    if (!seen.insert(Lower(path)).second) return;
    items.push_back({path, fd.ftLastWriteTime, {}, {}});
  }

  void AddDir(const std::wstring& dir) {
    if (IsUnder(dir, destRoot)) return;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
      const auto path = JoinPath(dir, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) AddDir(path);  // 정션 순환 방지
      } else {
        AddFile(path, fd);
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }

  void Add(const std::wstring& source) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(source.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
      // 드라이브 루트(C:\)는 FindFirstFile로 열리지 않는다.
      const DWORD attrs = GetFileAttributesW(source.c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) AddDir(source);
      return;
    }
    FindClose(find);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      AddDir(source);
    else
      AddFile(source, fd);
  }
};

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring NewRunDir(const std::wstring& destRoot) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d-%02d-%02d_%02d-%02d-%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  auto dir = JoinPath(destRoot, buf);
  for (int i = 2; Exists(dir); ++i) dir = JoinPath(destRoot, std::wstring(buf) + L"_" + std::to_wstring(i));
  return dir;
}

}  // namespace

OrganizeResult Organize(const std::vector<std::wstring>& sources, const std::wstring& destRoot, const ProgressFn& progress) {
  OrganizeResult result;

  Collector collector;
  collector.destRoot = destRoot;
  for (size_t i = 0; i < sources.size(); ++i) {
    progress(Phase::Collect, i, sources.size());
    collector.Add(sources[i]);
  }
  result.skipped = collector.skipped;
  auto& items = collector.items;

  // 촬영 시각 + 내용 해시
  Sha256 sha;
  std::vector<Item> ready;
  ready.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    progress(Phase::Scan, i, items.size());
    auto& item = items[i];
    DWORD error = 0;
    item.hash = sha.File(item.path, error);
    if (item.hash.empty()) {
      result.failed.push_back(item.path + L": " + ErrorText(error));
      continue;
    }
    item.stamp = ReadStamp(item);
    ready.push_back(std::move(item));
  }
  progress(Phase::Scan, items.size(), items.size());
  if (ready.empty()) return result;

  // 오래된 것부터 001, 002 ... 폴더에 100개씩
  std::stable_sort(ready.begin(), ready.end(), [](const Item& a, const Item& b) {
    if (a.stamp < b.stamp) return true;
    if (b.stamp < a.stamp) return false;
    return Lower(a.path) < Lower(b.path);
  });

  result.outputDir = NewRunDir(destRoot);
  size_t chunk = 0, inChunk = 0;
  std::wstring chunkDir;
  for (size_t i = 0; i < ready.size(); ++i) {
    progress(Phase::Move, i, ready.size());
    if (chunkDir.empty()) {
      chunkDir = JoinPath(result.outputDir, ChunkFolderName(chunk));
      const int rc = SHCreateDirectoryExW(nullptr, chunkDir.c_str(), nullptr);
      if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
        result.failed.push_back(chunkDir + L": " + ErrorText(static_cast<DWORD>(rc)));
        break;
      }
    }
    const auto& item = ready[i];
    const auto ext = ExtensionOf(item.path);
    std::wstring target;
    for (int n = 0;; ++n) {
      target = JoinPath(chunkDir, MakeFileName(item.stamp, item.hash, ext, n));
      if (!Exists(target)) break;
    }
    if (!MoveFileExW(item.path.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
      result.failed.push_back(item.path + L": " + ErrorText(GetLastError()));
      continue;
    }
    ++result.moved;
    if (++inChunk == kFilesPerFolder) {
      ++chunk;
      inChunk = 0;
      chunkDir.clear();
    }
  }
  progress(Phase::Move, ready.size(), ready.size());
  result.chunks = chunk + (inChunk > 0 ? 1 : 0);
  if (result.moved == 0) {
    RemoveDirectoryW(chunkDir.c_str());
    RemoveDirectoryW(result.outputDir.c_str());
    result.outputDir.clear();
  }
  return result;
}

}  // namespace gpu
