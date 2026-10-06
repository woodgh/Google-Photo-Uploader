#include "organizer.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propvarutil.h>
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "core.hpp"

namespace gpu {
namespace {

struct Item {
  std::wstring path;
  FILETIME lastWrite{};
  Stamp stamp;
  std::wstring hash;
  std::wstring target;  // 옮길 경로 (이동 단계에서 정함)
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

  // 파일 내용의 SHA-256 (hex). 실패하면 빈 문자열 + error. 여러 스레드에서 동시에 불러도 된다.
  std::wstring File(const std::wstring& path, DWORD& error) const {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring hex;
    if (alg_ && BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash, nullptr, 0, nullptr, 0, 0))) {
      std::vector<uint8_t> buffer(1 << 20);
      DWORD read = 0;
      bool ok = true;
      while ((ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE) && read > 0)
        BCryptHashData(hash, buffer.data(), read, 0);
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

// 동시에 실행할 수 있는 스레드 수 (논리 프로세서 수). 읽기·이동 워커 수와 IOCP 동시 실행 수에 쓴다.
size_t ConcurrentThreads() {
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n > 0 ? n : (std::max)(1u, std::thread::hardware_concurrency());
}

std::wstring NewRunDir(const std::wstring& destRoot) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d-%02d-%02d_%02d-%02d-%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  auto dir = JoinPath(destRoot, buf);
  for (int i = 2; Exists(dir); ++i) dir = JoinPath(destRoot, std::wstring(buf) + L"_" + std::to_wstring(i));
  return dir;
}

// IOCP를 작업 큐로 쓰는 스레드 풀: 0..count-1 작업을 threads개 워커가 나눠 처리한다.
// 워커마다 COM(MTA)을 초기화한다. 포트를 만들 수 없으면 호출 스레드에서 차례로 처리한다.
template <class Fn>
void RunOnIocp(size_t count, size_t threads, const Fn& fn) {
  if (count == 0) return;
  threads = (std::max)(size_t{1}, (std::min)(threads, count));
  HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, static_cast<DWORD>(threads));
  if (!port) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }
  auto worker = [&] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    // key = 작업 번호 + 1, 0은 종료 신호
    while (GetQueuedCompletionStatus(port, &bytes, &key, &ov, INFINITE) && key != 0) fn(static_cast<size_t>(key - 1));
    if (SUCCEEDED(com)) CoUninitialize();
  };
  std::vector<std::thread> pool;
  for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);
  for (size_t i = 0; i < count; ++i) PostQueuedCompletionStatus(port, 0, static_cast<ULONG_PTR>(i + 1), nullptr);
  for (size_t t = 0; t < threads; ++t) PostQueuedCompletionStatus(port, 0, 0, nullptr);  // 작업 뒤에 들어가므로 모두 끝난 뒤 종료
  for (auto& t : pool) t.join();
  CloseHandle(port);
}

bool GetBasicInfo(const std::wstring& path, FILE_BASIC_INFO& info) {
  HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const BOOL ok = GetFileInformationByHandleEx(h, FileBasicInfo, &info, sizeof(info));
  CloseHandle(h);
  return ok != FALSE;
}

// 다른 드라이브로 옮기면 OS가 복사 후 원본을 지우므로 만든 날짜 등이 바뀔 수 있다 → 원래 값으로 되돌린다.
// 같은 드라이브 이동(이름 바꾸기)은 이미 그대로라 아무것도 쓰지 않는다.
bool RestoreBasicInfo(const std::wstring& path, const FILE_BASIC_INFO& original) {
  FILE_BASIC_INFO now{};
  if (!GetBasicInfo(path, now)) return false;
  if (now.CreationTime.QuadPart == original.CreationTime.QuadPart && now.LastWriteTime.QuadPart == original.LastWriteTime.QuadPart &&
      now.LastAccessTime.QuadPart == original.LastAccessTime.QuadPart && now.FileAttributes == original.FileAttributes)
    return true;
  HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  FILE_BASIC_INFO info = original;
  info.ChangeTime.QuadPart = 0;  // 0 = 바꾸지 않음
  const BOOL ok = SetFileInformationByHandle(h, FileBasicInfo, &info, sizeof(info));
  CloseHandle(h);
  return ok != FALSE;
}

}  // namespace

std::wstring VolumeOf(const std::wstring& path) {
  wchar_t volume[MAX_PATH];
  if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH)) return {};
  return volume;
}

bool MovePreservingMetadata(const std::wstring& src, const std::wstring& dst, std::wstring& error, bool& metadataRestored) {
  FILE_BASIC_INFO original{};
  const bool haveInfo = GetBasicInfo(src, original);
  if (!MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
    error = ErrorText(GetLastError());
    return false;
  }
  metadataRestored = !haveInfo || RestoreBasicInfo(dst, original);
  if (!metadataRestored) error = ErrorText(GetLastError());
  return true;
}

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

  // 촬영 시각 + 내용 해시. 논리 프로세서 수만큼의 스레드로 읽는다.
  // 결과는 아래에서 정렬하므로 읽는 순서는 결과에 영향이 없다.
  std::vector<std::wstring> errors(items.size());
  std::atomic<size_t> done{0};
  const Sha256 sha;
  result.threads = ConcurrentThreads();
  progress(Phase::Scan, 0, items.size());
  RunOnIocp(items.size(), result.threads, [&](size_t i) {
    auto& item = items[i];
    DWORD error = 0;
    item.hash = sha.File(item.path, error);
    if (item.hash.empty())
      errors[i] = item.path + L": " + ErrorText(error);
    else
      item.stamp = ReadStamp(item);
    progress(Phase::Scan, done.fetch_add(1) + 1, items.size());
  });

  std::vector<Item> ready;
  ready.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    if (errors[i].empty())
      ready.push_back(std::move(items[i]));
    else
      result.failed.push_back(std::move(errors[i]));
  }
  progress(Phase::Scan, items.size(), items.size());
  if (ready.empty()) return result;

  // 오래된 것부터 001, 002 ... 폴더에 100개씩
  std::stable_sort(ready.begin(), ready.end(), [](const Item& a, const Item& b) {
    if (a.stamp < b.stamp) return true;
    if (b.stamp < a.stamp) return false;
    return Lower(a.path) < Lower(b.path);
  });

  // 옮길 자리를 정렬 순서대로 미리 정한다 (100개씩 001, 002 ...). 같은 이름은 _2, _3.
  result.outputDir = NewRunDir(destRoot);
  const size_t chunkCount = ChunkCount(ready.size());
  std::vector<std::wstring> chunkDirs(chunkCount);
  for (size_t c = 0; c < chunkCount; ++c) {
    chunkDirs[c] = JoinPath(result.outputDir, ChunkFolderName(c));
    const int rc = SHCreateDirectoryExW(nullptr, chunkDirs[c].c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
      result.failed.push_back(chunkDirs[c] + L": " + ErrorText(static_cast<DWORD>(rc)));
      for (auto& d : chunkDirs) RemoveDirectoryW(d.c_str());
      RemoveDirectoryW(result.outputDir.c_str());
      result.outputDir.clear();
      return result;
    }
  }
  std::set<std::wstring> taken;
  for (size_t i = 0; i < ready.size(); ++i) {
    auto& item = ready[i];
    const auto ext = ExtensionOf(item.path);
    for (int n = 0;; ++n) {
      item.target = JoinPath(chunkDirs[i / kFilesPerFolder], MakeFileName(item.stamp, item.hash, ext, n));
      if (!taken.count(Lower(item.target)) && !Exists(item.target)) break;
    }
    taken.insert(Lower(item.target));
  }

  // 이동: IOCP 작업 큐 + 워커 스레드. 복사본을 남기지 않는다(다른 드라이브면 OS가 복사 후 원본 삭제).
  // 파일 내용(EXIF 등)과 대체 데이터 스트림은 OS가 그대로 옮기고, 날짜·속성은 원래 값으로 맞춘다.
  std::vector<std::wstring> moveErrors(ready.size()), metaErrors(ready.size());
  std::vector<char> movedFlags(ready.size(), 0);
  std::atomic<size_t> moved{0};
  progress(Phase::Move, 0, ready.size());
  RunOnIocp(ready.size(), result.threads, [&](size_t i) {
    const auto& item = ready[i];
    std::wstring error;
    bool restored = true;
    if (!MovePreservingMetadata(item.path, item.target, error, restored)) {
      moveErrors[i] = item.path + L": " + error;
    } else {
      movedFlags[i] = 1;
      if (!restored) metaErrors[i] = item.target + L": 날짜 복원 실패 - " + error;
    }
    progress(Phase::Move, moved.fetch_add(1) + 1, ready.size());
  });

  std::vector<size_t> perChunk(chunkCount, 0);
  for (size_t i = 0; i < ready.size(); ++i) {
    if (movedFlags[i]) {
      ++result.moved;
      ++perChunk[i / kFilesPerFolder];
    } else {
      result.failed.push_back(std::move(moveErrors[i]));
    }
    if (!metaErrors[i].empty()) result.warnings.push_back(std::move(metaErrors[i]));
  }
  for (size_t c = 0; c < chunkCount; ++c) {
    if (perChunk[c])
      ++result.chunks;
    else
      RemoveDirectoryW(chunkDirs[c].c_str());
  }
  if (result.moved == 0) {
    RemoveDirectoryW(result.outputDir.c_str());
    result.outputDir.clear();
  }
  return result;
}

}  // namespace gpu
