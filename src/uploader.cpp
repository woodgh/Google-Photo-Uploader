#include "uploader.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#include "adb.hpp"
#include "phone_model.hpp"

namespace gpu {
namespace {

struct PcFile {
  std::wstring path;   // <runDir>\003\2024_05_01_13_ab12cd34.jpg
  std::wstring chunk;  // 003
  std::string name;    // 2024_05_01_13_ab12cd34.jpg (폰에서의 이름)
};

bool IsChunkName(const wchar_t* n) {
  return wcslen(n) == 3 && iswdigit(n[0]) && iswdigit(n[1]) && iswdigit(n[2]);
}

template <class Fn>
void ForEachEntry(const std::wstring& dir, Fn&& fn) {
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) fn(fd);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}

// 아직 올리지 않은 파일: 001, 002 ... 순서, 폴더 안은 이름순(= 촬영 시각순)
std::vector<PcFile> ListPending(const std::wstring& runDir) {
  std::vector<std::wstring> chunks;
  ForEachEntry(runDir, [&](const WIN32_FIND_DATAW& fd) {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsChunkName(fd.cFileName)) chunks.emplace_back(fd.cFileName);
  });
  std::sort(chunks.begin(), chunks.end());
  std::vector<PcFile> files;
  for (const auto& c : chunks) {
    std::vector<PcFile> inChunk;
    ForEachEntry(runDir + L"\\" + c, [&](const WIN32_FIND_DATAW& fd) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) inChunk.push_back({runDir + L"\\" + c + L"\\" + fd.cFileName, c, WideToUtf8(fd.cFileName)});
    });
    std::sort(inChunk.begin(), inChunk.end(), [](const PcFile& a, const PcFile& b) { return a.name < b.name; });
    files.insert(files.end(), inChunk.begin(), inChunk.end());
  }
  return files;
}

size_t CountUploaded(const std::wstring& runDir) {
  size_t n = 0;
  const auto done = runDir + L"\\" + kUploadedFolder;
  ForEachEntry(done, [&](const WIN32_FIND_DATAW& chunk) {
    if (chunk.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      ForEachEntry(done + L"\\" + chunk.cFileName, [&](const WIN32_FIND_DATAW& f) {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
      });
  });
  return n;
}

int64_t NowSeconds() { return static_cast<int64_t>(GetTickCount64() / 1000); }

std::string PhonePath(const std::string& name) { return std::string(kPhoneDir) + "/" + name; }

// 연결된 폰: 사용할 수 있는 첫 기기의 serial. 없으면 빈 문자열 + 상태 문구.
std::string FindPhone(const Adb& adb, std::wstring& state) {
  if (!adb.Found()) {
    state = L"adb.exe를 찾을 수 없습니다 (앱과 같은 폴더에 있어야 합니다)";
    return {};
  }
  const auto r = adb.Run({}, {L"devices"}, 20000);
  if (r.exitCode != 0) {
    state = L"adb 실행 실패";
    return {};
  }
  bool unauthorized = false;
  for (const auto& d : ParseDevices(r.output)) {
    if (d.state == "device") {
      const auto model = Trim(adb.Shell(d.serial, "getprop ro.product.model", 10000).output);
      state = L"연결됨: " + Utf8ToWide(model.empty() ? d.serial : model) + L" (" + Utf8ToWide(d.serial) + L")";
      return d.serial;
    }
    if (d.state == "unauthorized") unauthorized = true;
  }
  state = unauthorized ? L"폰 화면에서 'USB 디버깅 허용'을 눌러 주세요" : L"폰이 연결되지 않았습니다 (USB 디버깅을 켜고 연결해 주세요)";
  return {};
}

// 셸 명령 여러 개를 한 번에 (명령줄 길이를 넘지 않게 나눠서)
void ShellBatch(const Adb& adb, const std::string& serial, const std::vector<std::string>& commands) {
  std::string line;
  for (size_t i = 0; i < commands.size(); ++i) {
    line += commands[i] + " >/dev/null 2>&1; ";
    if (line.size() > 6000 || i + 1 == commands.size()) {
      adb.Shell(serial, line, 120000);
      line.clear();
    }
  }
}

std::string ScanCommand(const std::string& name) {
  return "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://" + PhonePath(name);
}

std::wstring Clock() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t buf[32];
  swprintf(buf, 32, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
  return buf;
}

}  // namespace

std::wstring DescribePhone() {
  Adb adb;
  std::wstring state;
  FindPhone(adb, state);
  return state;
}

void Uploader::Start(const UploadOptions& options, const UploadEvents& events) {
  Join();
  stop_ = false;
  running_ = true;
  thread_ = std::thread(&Uploader::Run, this, options, events);
}

void Uploader::Join() {
  if (thread_.joinable()) thread_.join();
}

void Uploader::Run(UploadOptions opt, UploadEvents ev) {
  const Adb adb;
  FILE* logFile = nullptr;
  _wfopen_s(&logFile, (opt.runDir + L"\\upload-log.txt").c_str(), L"a, ccs=UTF-8");
  auto log = [&](const std::wstring& text) {
    const auto line = Clock() + L"  " + text;
    if (logFile) {
      fwprintf(logFile, L"%ls\n", line.c_str());
      fflush(logFile);
    }
    ev.log(line);
  };
  UploadStatus st;
  auto publish = [&] { ev.status(st); };
  // stop_이 켜지면 바로 깨어나는 대기
  auto sleepFor = [&](int seconds) {
    for (int i = 0; i < seconds * 10 && !stop_; ++i) Sleep(100);
  };

  log(L"업로드 시작: " + opt.runDir + L"  (한 번에 " + std::to_wstring(opt.batchSize) + L"개, 변동 없음 " +
      std::to_wstring(opt.quietSeconds / 60) + L"분" + (opt.quietSeconds % 60 ? L" " + std::to_wstring(opt.quietSeconds % 60) + L"초" : L"") +
      L" 후 완료)");

  std::vector<PcFile> batch;
  QuietTimer timer;
  std::string serial;
  bool wasConnected = false;
  std::wstring lastBackup;

  while (!stop_) {
    std::wstring phoneState;
    serial = FindPhone(adb, phoneState);
    st.phone = phoneState;
    if (serial.empty()) {
      if (wasConnected) log(L"폰 연결 끊김 — 다시 연결되면 이어서 합니다");
      wasConnected = false;
      st.stage = L"폰 연결을 기다리는 중";
      st.quietNeeded = 0;
      publish();
      sleepFor(3);
      continue;
    }
    if (!wasConnected) {
      log(phoneState);
      timer.Reset(NowSeconds());  // 끊겼던 동안은 알 수 없으므로 처음부터 다시 센다
      wasConnected = true;
    }

    if (batch.empty()) {
      // 다음 묶음: 폰에 이미 있는 파일(이전 실행에서 넣은 것)부터, 모자라면 새로 넣는다
      auto pending = ListPending(opt.runDir);
      st.uploaded = CountUploaded(opt.runDir);
      st.total = st.uploaded + pending.size();
      if (pending.empty()) {
        st.stage = L"모두 완료";
        st.inBatch = 0;
        st.quietNeeded = 0;
        publish();
        log(L"모든 파일을 올렸습니다 (" + std::to_wstring(st.uploaded) + L"개)");
        break;
      }
      adb.Shell(serial, std::string("mkdir -p ") + kPhoneDir, 20000);
      const auto onPhone = ParseNames(adb.Shell(serial, std::string("ls -1 ") + kPhoneDir, 60000).output);
      std::vector<PcFile> toPush;
      for (const auto& f : pending) {
        if (batch.size() + toPush.size() >= opt.batchSize) break;
        (onPhone.count(f.name) ? batch : toPush).push_back(f);
      }
      if (!batch.empty()) log(L"폰에 남아 있던 " + std::to_wstring(batch.size()) + L"개를 현재 묶음으로 이어서 확인합니다");

      st.stage = L"폰으로 복사하는 중";
      st.inBatch = batch.size() + toPush.size();
      st.quietNeeded = 0;
      publish();
      std::vector<std::string> pushed;
      for (size_t i = 0; i < toPush.size() && !stop_; ++i) {
        const auto& f = toPush[i];
        // -a: 수정 시각 보존
        auto r = adb.Run(serial, {L"push", L"-a", f.path, Utf8ToWide(PhonePath(f.name))}, 10 * 60 * 1000);
        if (r.exitCode != 0) r = adb.Run(serial, {L"push", f.path, Utf8ToWide(PhonePath(f.name))}, 10 * 60 * 1000);
        if (r.exitCode != 0) {
          log(L"복사 실패: " + f.path + L" — " + Utf8ToWide(Trim(r.output)));
          continue;
        }
        batch.push_back(f);
        pushed.push_back(f.name);
        if (opt.batchSize == 1 || toPush.size() <= 5) log(L"폰으로 복사: " + f.chunk + L"\\" + Utf8ToWide(f.name));
        st.stage = L"폰으로 복사하는 중 " + std::to_wstring(i + 1) + L" / " + std::to_wstring(toPush.size());
        publish();
      }
      if (stop_) break;
      if (toPush.size() > 5) log(L"폰으로 복사: " + std::to_wstring(pushed.size()) + L"개 (" + toPush.front().chunk + L"\\" +
                                 Utf8ToWide(toPush.front().name) + L" ~)");
      std::vector<std::string> scans;
      for (const auto& n : pushed) scans.push_back(ScanCommand(n));
      ShellBatch(adb, serial, scans);  // Google 포토가 바로 알아차리도록 미디어 등록
      if (batch.empty()) {
        log(L"복사된 파일이 없어 3초 뒤 다시 시도합니다");
        sleepFor(3);
        continue;
      }
      st.inBatch = batch.size();
      timer.Reset(NowSeconds());
      lastBackup.clear();
      log(L"Google 포토 백업을 기다립니다 (" + std::to_wstring(batch.size()) + L"개)");
    }

    // Google 포토 알림 확인
    const auto snap = ParsePhotosNotifications(adb.Shell(serial, "dumpsys notification --noredact", 30000).output);
    const auto now = NowSeconds();
    if (timer.Observe(now, snap)) {
      const auto text = snap.summary.empty() ? std::wstring(L"알림 없음") : Utf8ToWide(snap.summary);
      if (text != lastBackup) log(L"Google 포토: " + text + (snap.active ? L"  (백업 중)" : L""));
      lastBackup = text;
    }
    st.backup = Utf8ToWide(snap.summary);
    st.backingUp = snap.active;
    st.quietFor = static_cast<int>(timer.QuietFor(now));
    st.quietNeeded = opt.quietSeconds;
    st.stage = snap.active ? L"Google 포토가 백업하는 중" : L"변동 없음 확인 중";
    publish();

    if (timer.Done(now, opt.quietSeconds)) {
      log(std::to_wstring(opt.quietSeconds / 60) + L"분" + (opt.quietSeconds % 60 ? L" " + std::to_wstring(opt.quietSeconds % 60) + L"초" : L"") +
          L" 동안 변동 없음 → " + std::to_wstring(batch.size()) + L"개 업로드 완료로 판단");
      // PC: <runDir>\업로드 완료\NNN\ 으로 이동
      size_t moved = 0;
      std::set<std::wstring> chunks;
      for (const auto& f : batch) {
        const auto dir = opt.runDir + L"\\" + kUploadedFolder + L"\\" + f.chunk;
        CreateDirectoryW((opt.runDir + L"\\" + kUploadedFolder).c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        if (MoveFileExW(f.path.c_str(), (dir + L"\\" + Utf8ToWide(f.name)).c_str(), MOVEFILE_COPY_ALLOWED))
          ++moved;
        else
          log(L"PC 이동 실패: " + f.path);
        chunks.insert(opt.runDir + L"\\" + f.chunk);
      }
      for (const auto& c : chunks) RemoveDirectoryW(c.c_str());  // 비었으면 지운다
      // 폰: 사본 삭제 + 미디어 목록에서 제거
      std::vector<std::string> cmds;
      for (const auto& f : batch) cmds.push_back("rm -f " + PhonePath(f.name));
      for (const auto& f : batch) cmds.push_back(ScanCommand(f.name));
      ShellBatch(adb, serial, cmds);
      log(L"PC '" + std::wstring(kUploadedFolder) + L"' 폴더로 " + std::to_wstring(moved) + L"개 이동, 폰에서 " + std::to_wstring(batch.size()) +
          L"개 삭제");
      batch.clear();
      continue;
    }
    sleepFor(opt.batchSize == 1 ? 3 : 10);
  }

  if (stop_) log(L"중지했습니다. 폰에 넣은 현재 묶음은 그대로 두고, 다시 시작하면 이어서 확인합니다.");
  if (logFile) fclose(logFile);
  running_ = false;
  ev.finished();
}

}  // namespace gpu
