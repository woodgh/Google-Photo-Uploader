// Google-Photo-Uploader
//  ① 폴더를 끌어다 놓거나 골라서 [완료]를 누르면 사진·동영상을 100개씩 나눠 <저장 폴더>\<실행 시각>\001 ... 로 옮긴다.
//     (파일 이름: 년_월_일_시_해쉬)
//  ② 정리된 폴더를 묶음(슬로우 1개 / 50개 / 100개)씩 Pixel 폰에 넣고, Google 포토 백업이 끝나면 다음 묶음을 넣는다.
//
// 창 없이 실행(CI 확인용):
//   Google-Photo-Uploader.exe --organize --dest <저장 폴더> <원본 폴더>...
//   Google-Photo-Uploader.exe --upload --dir <정리 폴더> --batch <개수> --quiet-seconds <초>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core.hpp"
#include "adb.hpp"
#include "organizer.hpp"
#include "uploader.hpp"
#include "version.h"

namespace {

constexpr wchar_t kTitle[] = L"Google Photo Uploader";
constexpr wchar_t kRegKey[] = L"Software\\Google-Photo-Uploader";
constexpr wchar_t kRegDest[] = L"Destination";
constexpr wchar_t kRegLastOutput[] = L"LastOutput";
constexpr wchar_t kRegUploadMode[] = L"UploadMode";
constexpr wchar_t kRegQuietMinutes[] = L"QuietMinutes";

// 업로드 방식: 콤보 순서와 같다
struct UploadMode {
  const wchar_t* label;
  size_t batch;
  int defaultMinutes;
};
constexpr UploadMode kModes[] = {{L"슬로우 (1개씩)", 1, 3}, {L"50개씩", 50, 30}, {L"100개씩", 100, 30}};

enum : int { IDC_HINT = 100, IDC_LIST, IDC_ADD, IDC_REMOVE, IDC_CLEAR, IDC_DEST_LABEL, IDC_DEST, IDC_DEST_CHANGE,
             IDC_PROGRESS, IDC_STATUS, IDC_DONE,
             IDC_UP_HEADER, IDC_UP_PHONE, IDC_UP_DIR_LABEL, IDC_UP_DIR, IDC_UP_DIR_CHANGE, IDC_UP_MODE_LABEL, IDC_UP_MODE,
             IDC_UP_QUIET_LABEL, IDC_UP_QUIET, IDC_UP_START, IDC_UP_STATUS, IDC_UP_BACKUP, IDC_UP_LOG };
enum : UINT { WM_APP_PROGRESS = WM_APP + 1, WM_APP_FINISHED, WM_APP_UP_LOG, WM_APP_UP_STATUS, WM_APP_UP_DONE, WM_APP_PHONE };
constexpr UINT_PTR kPhoneTimer = 1;

struct App {
  HWND hwnd = nullptr;
  HWND hint, list, add, remove, clear, destLabel, dest, destChange, progress, status, done;
  HWND upHeader, upPhone, upDirLabel, upDir, upDirChange, upModeLabel, upMode, upQuietLabel, upQuiet, upStart, upStatus, upBackup, upLog;
  HFONT font = nullptr, bold = nullptr;
  UINT dpi = 96;
  std::vector<std::wstring> sources;
  std::wstring destRoot;
  std::wstring uploadDir;
  bool busy = false;
  gpu::Uploader uploader;
  std::atomic<bool> phoneCheck{false};  // 폰 상태 확인이 진행 중
};
App g;

int Scale(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

// --- 설정 (저장 폴더) ---------------------------------------------------------

std::wstring DefaultDest() {
  PWSTR pictures = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) dir = std::wstring(pictures) + L"\\Google-Photo-Uploader";
  CoTaskMemFree(pictures);
  return dir;
}

std::wstring LoadDest() {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegDest, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0]) return buf;
  return DefaultDest();
}

void SaveDest(const std::wstring& dir) {
  RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, kRegDest, REG_SZ, dir.c_str(), static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));
}

std::wstring LoadString(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
  return {};
}

void SaveString(const wchar_t* name, const std::wstring& value) {
  RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

DWORD LoadNumber(const wchar_t* name, DWORD fallback) {
  DWORD value = 0, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) return value;
  return fallback;
}

void SaveNumber(const wchar_t* name, DWORD value) { RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &value, sizeof(value)); }

// --- 폴더 목록 ----------------------------------------------------------------

void SetStatus(const std::wstring& text) { SetWindowTextW(g.status, text.c_str()); }

void UpdateButtons() {
  const bool idle = !g.busy;
  EnableWindow(g.add, idle);
  EnableWindow(g.remove, idle && SendMessageW(g.list, LB_GETSELCOUNT, 0, 0) > 0);
  EnableWindow(g.clear, idle && !g.sources.empty());
  EnableWindow(g.destChange, idle);
  EnableWindow(g.done, idle && !g.sources.empty());
  DragAcceptFiles(g.hwnd, idle);
}

void AddSource(const std::wstring& path) {
  for (const auto& s : g.sources)
    if (gpu::Lower(s) == gpu::Lower(path)) return;
  g.sources.push_back(path);
  SendMessageW(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
}

void SourcesChanged() {
  SetStatus(g.sources.empty() ? L"" : std::to_wstring(g.sources.size()) + L"개 선택됨 — [완료]를 누르면 정리를 시작합니다.");
  UpdateButtons();
}

void OnDrop(HDROP drop) {
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(len + 1, L'\0');
    DragQueryFileW(drop, i, path.data(), len + 1);
    path.resize(len);
    AddSource(path);
  }
  DragFinish(drop);
  SourcesChanged();
}

// 여러 폴더를 한 번에 고르는 대화상자. 고른 경로들을 돌려준다.
std::vector<std::wstring> PickFolders(bool multi, const wchar_t* title) {
  std::vector<std::wstring> picked;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return picked;
  FILEOPENDIALOGOPTIONS opts = 0;
  dlg->GetOptions(&opts);
  opts |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM;
  if (multi) opts |= FOS_ALLOWMULTISELECT;
  dlg->SetOptions(opts);
  dlg->SetTitle(title);
  if (SUCCEEDED(dlg->Show(g.hwnd))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD n = 0;
      items->GetCount(&n);
      for (DWORD i = 0; i < n; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) picked.emplace_back(path);
        CoTaskMemFree(path);
        item->Release();
      }
      items->Release();
    }
  }
  dlg->Release();
  return picked;
}

void OnAdd() {
  for (const auto& p : PickFolders(true, L"정리할 폴더 선택 (여러 개 가능)")) AddSource(p);
  SourcesChanged();
}

void OnRemove() {
  const int n = static_cast<int>(SendMessageW(g.list, LB_GETCOUNT, 0, 0));
  for (int i = n - 1; i >= 0; --i) {
    if (SendMessageW(g.list, LB_GETSEL, i, 0) > 0) {
      SendMessageW(g.list, LB_DELETESTRING, i, 0);
      g.sources.erase(g.sources.begin() + i);
    }
  }
  SourcesChanged();
}

void OnClear() {
  SendMessageW(g.list, LB_RESETCONTENT, 0, 0);
  g.sources.clear();
  SourcesChanged();
}

void OnDestChange() {
  const auto picked = PickFolders(false, L"정리한 파일을 옮길 폴더 선택");
  if (picked.empty()) return;
  g.destRoot = picked.front();
  SaveDest(g.destRoot);
  SetWindowTextW(g.dest, g.destRoot.c_str());
}

// --- 정리 실행 ----------------------------------------------------------------

// 작업 스레드들의 진행 보고. 마지막 값만 남기고, 창에 보낸 메시지가 처리되기 전에는 더 보내지 않는다.
struct ProgressState {
  std::atomic<int> phase{0};
  std::atomic<size_t> done{0}, total{0};
  std::atomic<bool> pending{false};
};
ProgressState g_progress;

void ReportProgress(HWND hwnd, gpu::Phase phase, size_t done, size_t total) {
  const int p = static_cast<int>(phase);
  if (g_progress.phase.exchange(p) != p) {
    g_progress.done = done;  // 단계가 바뀌는 보고는 한 스레드에서만 온다
  } else {
    // 여러 워커가 순서 없이 보고하므로 큰 값만 남긴다
    size_t cur = g_progress.done;
    while (cur < done && !g_progress.done.compare_exchange_weak(cur, done)) {
    }
  }
  g_progress.total = total;
  if (!g_progress.pending.exchange(true)) PostMessageW(hwnd, WM_APP_PROGRESS, 0, 0);
}

std::wstring WithCommas(size_t n) {
  auto digits = std::to_wstring(n);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), L",");
  return digits;
}

void OnDone() {
  if (g.sources.empty() || g.busy) return;
  for (const auto& s : g.sources) {
    if (gpu::IsUnder(g.destRoot, s)) {
      MessageBoxW(g.hwnd, (L"저장 폴더가 선택한 폴더 안에 있습니다.\n\n" + s).c_str(), kTitle, MB_ICONWARNING);
      return;
    }
  }
  // 다른 드라이브면 이동이 복사 후 원본 삭제로 처리되어 오래 걸린다 → 시작 전에 확인.
  std::wstring otherDrive;
  const auto destVolume = gpu::Lower(gpu::VolumeOf(g.destRoot));
  for (const auto& s : g.sources)
    if (gpu::Lower(gpu::VolumeOf(s)) != destVolume) otherDrive += s + L"\n";
  if (!otherDrive.empty()) {
    const auto msg = L"다음 항목은 저장 폴더와 다른 드라이브에 있습니다.\n\n" + otherDrive +
                     L"\n드라이브 사이 이동은 파일을 옮겨 쓴 뒤 원본을 지우므로 시간이 걸립니다. (원본은 남지 않고 날짜는 보존됩니다)\n계속할까요?";
    if (MessageBoxW(g.hwnd, msg.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
  }
  g.busy = true;
  UpdateButtons();
  SendMessageW(g.progress, PBM_SETPOS, 0, 0);
  g_progress.phase = 0;
  g_progress.done = 0;
  g_progress.total = 0;
  SetStatus(L"폴더를 살펴보는 중...");
  std::thread([hwnd = g.hwnd, sources = g.sources, dest = g.destRoot] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = std::make_unique<gpu::OrganizeResult>(gpu::Organize(sources, dest, [hwnd](gpu::Phase phase, size_t done, size_t total) {
      ReportProgress(hwnd, phase, done, total);
    }));
    CoUninitialize();
    PostMessageW(hwnd, WM_APP_FINISHED, 0, reinterpret_cast<LPARAM>(result.release()));
  }).detach();
}

void OnProgress() {
  g_progress.pending = false;  // 먼저 내려야 이후 보고가 새 메시지를 보낸다
  const auto phase = static_cast<gpu::Phase>(g_progress.phase.load());
  const size_t done = g_progress.done, total = g_progress.total;
  // 진행률: 찾기 0~5%, 읽기 5~60%, 이동 60~100%
  const int base[] = {0, 50, 600}, span[] = {50, 550, 400};
  const int i = static_cast<int>(phase);
  SendMessageW(g.progress, PBM_SETPOS, base[i] + (total ? static_cast<int>(span[i] * done / total) : 0), 0);
  static const wchar_t* const kText[] = {L"폴더를 살펴보는 중", L"촬영 시각과 해시를 읽는 중", L"폴더로 옮기는 중"};
  std::wstring text = kText[i];
  if (phase == gpu::Phase::Collect)
    text += L"...";
  else
    text += L"...  " + WithCommas(done) + L" / " + WithCommas(total) + L"개";
  SetStatus(text);
}

void OnFinished(std::unique_ptr<gpu::OrganizeResult> r) {
  g.busy = false;
  SendMessageW(g.progress, PBM_SETPOS, 1000, 0);
  std::wstring text = std::to_wstring(r->moved) + L"개 파일을 " + std::to_wstring(r->chunks) + L"개 폴더로 옮겼습니다.";
  if (r->skipped) text += L"  사진·동영상이 아닌 " + std::to_wstring(r->skipped) + L"개는 그대로 두었습니다.";
  if (!r->failed.empty()) text += L"  실패 " + std::to_wstring(r->failed.size()) + L"개.";
  if (!r->warnings.empty()) text += L"  날짜 복원 실패 " + std::to_wstring(r->warnings.size()) + L"개.";
  SetStatus(text);
  auto showList = [](const wchar_t* heading, const std::vector<std::wstring>& list) {
    if (list.empty()) return;
    std::wstring detail = heading;
    for (size_t i = 0; i < list.size() && i < 20; ++i) detail += list[i] + L"\n";
    if (list.size() > 20) detail += L"... 외 " + std::to_wstring(list.size() - 20) + L"개\n";
    MessageBoxW(g.hwnd, detail.c_str(), kTitle, MB_ICONWARNING);
  };
  showList(L"옮기지 못한 파일:\n\n", r->failed);
  showList(L"옮겼지만 날짜·속성을 원래대로 되돌리지 못한 파일:\n\n", r->warnings);
  if (r->moved > 0) {
    OnClear();
    SetStatus(text);
    // 방금 만든 폴더를 ② 업로드 대상으로
    if (!g.uploader.Running()) {
      g.uploadDir = r->outputDir;
      SaveString(kRegLastOutput, g.uploadDir);
      SetWindowTextW(g.upDir, g.uploadDir.c_str());
    }
    ShellExecuteW(g.hwnd, L"open", r->outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  UpdateButtons();
}

// --- ② 폰 업로드 ---------------------------------------------------------------

std::wstring Duration(int seconds) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%d:%02d", seconds / 60, seconds % 60);
  return buf;
}

void AppendLog(const std::wstring& line) {
  const int len = GetWindowTextLengthW(g.upLog);
  if (len > 400000) {  // 오래된 줄은 버린다 (전체 기록은 upload-log.txt)
    SendMessageW(g.upLog, EM_SETSEL, 0, 100000);
    SendMessageW(g.upLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
  }
  const int end = GetWindowTextLengthW(g.upLog);
  SendMessageW(g.upLog, EM_SETSEL, end, end);
  SendMessageW(g.upLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

void UpdateUploadControls() {
  const bool running = g.uploader.Running();
  SetWindowTextW(g.upStart, !running ? L"업로드 시작" : g.uploader.Stopping() ? L"중지하는 중..." : L"중지");
  EnableWindow(g.upStart, !g.uploader.Stopping());
  EnableWindow(g.upDirChange, !running);
  EnableWindow(g.upMode, !running);
  EnableWindow(g.upQuiet, !running);
}

void OnUploadDirChange() {
  const auto picked = PickFolders(false, L"업로드할 정리 폴더 선택 (001, 002 ... 가 들어 있는 폴더)");
  if (picked.empty()) return;
  g.uploadDir = picked.front();
  SaveString(kRegLastOutput, g.uploadDir);
  SetWindowTextW(g.upDir, g.uploadDir.c_str());
}

void OnModeChange() {
  const int mode = static_cast<int>(SendMessageW(g.upMode, CB_GETCURSEL, 0, 0));
  if (mode < 0) return;
  SetWindowTextW(g.upQuiet, std::to_wstring(kModes[mode].defaultMinutes).c_str());
}

void OnUploadStart() {
  if (g.uploader.Running()) {
    g.uploader.RequestStop();
    UpdateUploadControls();
    return;
  }
  const DWORD attrs = GetFileAttributesW(g.uploadDir.c_str());
  if (g.uploadDir.empty() || attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    MessageBoxW(g.hwnd, L"업로드할 폴더를 선택하세요. (① 정리로 만든 001, 002 ... 가 들어 있는 폴더)", kTitle, MB_ICONINFORMATION);
    return;
  }
  wchar_t buf[16];
  GetWindowTextW(g.upQuiet, buf, 16);
  const int minutes = _wtoi(buf);
  if (minutes < 1) {
    MessageBoxW(g.hwnd, L"변동 없음 판정 시간은 1분 이상이어야 합니다.", kTitle, MB_ICONINFORMATION);
    return;
  }
  const int mode = (std::max)(0, static_cast<int>(SendMessageW(g.upMode, CB_GETCURSEL, 0, 0)));
  SaveNumber(kRegUploadMode, static_cast<DWORD>(mode));
  SaveNumber(kRegQuietMinutes, static_cast<DWORD>(minutes));

  gpu::UploadOptions opt;
  opt.runDir = g.uploadDir;
  opt.batchSize = kModes[mode].batch;
  opt.quietSeconds = minutes * 60;
  const HWND hwnd = g.hwnd;
  gpu::UploadEvents ev;
  ev.log = [hwnd](const std::wstring& line) { PostMessageW(hwnd, WM_APP_UP_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(line))); };
  ev.status = [hwnd](const gpu::UploadStatus& st) {
    PostMessageW(hwnd, WM_APP_UP_STATUS, 0, reinterpret_cast<LPARAM>(new gpu::UploadStatus(st)));
  };
  ev.finished = [hwnd] { PostMessageW(hwnd, WM_APP_UP_DONE, 0, 0); };
  g.uploader.Start(opt, ev);
  UpdateUploadControls();
}

void OnUploadStatus(const gpu::UploadStatus& st) {
  SetWindowTextW(g.upPhone, st.phone.c_str());
  std::wstring text = st.stage;
  if (st.total) text += L"  ·  완료 " + WithCommas(st.uploaded) + L" / " + WithCommas(st.total) + L"개";
  if (st.inBatch) text += L"  ·  폰에 " + WithCommas(st.inBatch) + L"개";
  if (st.quietNeeded) text += L"  ·  변동 없음 " + Duration((std::min)(st.quietFor, st.quietNeeded)) + L" / " + Duration(st.quietNeeded);
  SetWindowTextW(g.upStatus, text.c_str());
  SetWindowTextW(g.upBackup, (L"Google 포토 알림: " + (st.backup.empty() ? std::wstring(L"없음") : st.backup)).c_str());
}

// 업로드 중이 아닐 때 3초마다 폰 연결 상태를 확인한다 (adb 실행은 UI를 막지 않게 따로)
void CheckPhoneAsync() {
  if (g.uploader.Running() || g.phoneCheck.exchange(true)) return;
  std::thread([hwnd = g.hwnd] {
    PostMessageW(hwnd, WM_APP_PHONE, 0, reinterpret_cast<LPARAM>(new std::wstring(gpu::DescribePhone())));
    g.phoneCheck = false;
  }).detach();
}

// --- 창 -----------------------------------------------------------------------

void ApplyFonts() {
  if (g.font) DeleteObject(g.font);
  if (g.bold) DeleteObject(g.bold);
  NONCLIENTMETRICSW ncm{sizeof(ncm)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
  ncm.lfMessageFont.lfHeight = -Scale(14);
  g.font = CreateFontIndirectW(&ncm.lfMessageFont);
  ncm.lfMessageFont.lfHeight = -Scale(16);
  ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
  g.bold = CreateFontIndirectW(&ncm.lfMessageFont);
  for (HWND h : {g.list, g.add, g.remove, g.clear, g.destLabel, g.dest, g.destChange, g.status, g.upPhone, g.upDirLabel, g.upDir,
                 g.upDirChange, g.upModeLabel, g.upMode, g.upQuietLabel, g.upQuiet, g.upStart, g.upStatus, g.upBackup, g.upLog})
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
  for (HWND h : {g.hint, g.done, g.upHeader}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.bold), TRUE);
}

void Layout() {
  RECT rc;
  GetClientRect(g.hwnd, &rc);
  const int m = Scale(16), gap = Scale(8), w = rc.right - 2 * m, bh = Scale(30), bw = Scale(110), th = Scale(20), hh = Scale(24);
  // 높이가 고정된 줄을 빼고 남는 높이를 폴더 목록(40%)과 업로드 로그(60%)가 나눠 쓴다
  const int fixed1 = hh + gap + gap + bh + gap * 2 + bh + gap + Scale(18) + gap + th + gap + Scale(40);
  const int fixed2 = Scale(20) + hh + gap + th + gap + bh + gap + bh + gap + th + Scale(4) + th + gap;
  const int flexible = (std::max)(Scale(160), static_cast<int>(rc.bottom) - 2 * m - fixed1 - fixed2);
  const int listH = flexible * 4 / 10;
  const int lw = Scale(80);

  int y = m;
  MoveWindow(g.hint, m, y, w, hh, TRUE);
  y += hh + gap;
  MoveWindow(g.list, m, y, w, listH, TRUE);
  y += listH + gap;
  MoveWindow(g.add, m, y, bw, bh, TRUE);
  MoveWindow(g.remove, m + bw + gap, y, bw, bh, TRUE);
  MoveWindow(g.clear, m + 2 * (bw + gap), y, bw, bh, TRUE);
  y += bh + gap * 2;
  MoveWindow(g.destLabel, m, y + Scale(6), lw, th, TRUE);
  MoveWindow(g.dest, m + lw, y + Scale(2), w - lw - bw - gap, Scale(26), TRUE);
  MoveWindow(g.destChange, rc.right - m - bw, y, bw, bh, TRUE);
  y += bh + gap;
  MoveWindow(g.progress, m, y, w, Scale(18), TRUE);
  y += Scale(18) + gap;
  MoveWindow(g.status, m, y, w, th, TRUE);
  y += th + gap;
  MoveWindow(g.done, m, y, w, Scale(40), TRUE);
  y += Scale(40) + Scale(20);

  MoveWindow(g.upHeader, m, y, w, hh, TRUE);
  y += hh + gap;
  MoveWindow(g.upPhone, m, y, w, th, TRUE);
  y += th + gap;
  MoveWindow(g.upDirLabel, m, y + Scale(6), lw, th, TRUE);
  MoveWindow(g.upDir, m + lw, y + Scale(2), w - lw - bw - gap, Scale(26), TRUE);
  MoveWindow(g.upDirChange, rc.right - m - bw, y, bw, bh, TRUE);
  y += bh + gap;
  MoveWindow(g.upModeLabel, m, y + Scale(6), lw, th, TRUE);
  MoveWindow(g.upMode, m + lw, y + Scale(2), Scale(150), Scale(200), TRUE);
  const int qx = m + lw + Scale(150) + Scale(16);
  MoveWindow(g.upQuietLabel, qx, y + Scale(6), Scale(100), th, TRUE);
  MoveWindow(g.upQuiet, qx + Scale(100), y + Scale(2), Scale(50), Scale(26), TRUE);
  MoveWindow(g.upStart, rc.right - m - bw, y, bw, bh, TRUE);
  y += bh + gap;
  MoveWindow(g.upStatus, m, y, w, th, TRUE);
  y += th + Scale(4);
  MoveWindow(g.upBackup, m, y, w, th, TRUE);
  y += th + gap;
  MoveWindow(g.upLog, m, y, w, (std::max)(Scale(60), static_cast<int>(rc.bottom) - m - y), TRUE);
}

LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
  if (msg == WM_DROPFILES) return SendMessageW(GetParent(h), msg, wp, lp);
  if (msg == WM_PAINT && SendMessageW(h, LB_GETCOUNT, 0, 0) == 0) {
    // 빈 목록 안내 문구
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(dc, L"여기로 폴더를 끌어다 놓으세요", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(h, &ps);
    return 0;
  }
  if (msg == WM_SIZE) InvalidateRect(h, nullptr, TRUE);
  return DefSubclassProc(h, msg, wp, lp);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
  return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g.hwnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void Create(HWND hwnd) {
  g.hwnd = hwnd;
  g.dpi = GetDpiForWindow(hwnd);
  g.hint = Make(L"STATIC", L"① 폴더 정리 — 폴더를 끌어다 놓거나 [폴더 추가]로 선택하세요", SS_LEFT, IDC_HINT);
  g.list = Make(WC_LISTBOXW, L"", WS_VSCROLL | WS_HSCROLL | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IDC_LIST,
                WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES);
  SetWindowSubclass(g.list, ListProc, 1, 0);
  SendMessageW(g.list, LB_SETHORIZONTALEXTENT, 4000, 0);
  g.add = Make(WC_BUTTONW, L"폴더 추가...", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD);
  g.remove = Make(WC_BUTTONW, L"선택 제거", BS_PUSHBUTTON | WS_TABSTOP, IDC_REMOVE);
  g.clear = Make(WC_BUTTONW, L"모두 지우기", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR);
  g.destLabel = Make(L"STATIC", L"저장 폴더", SS_LEFT, IDC_DEST_LABEL);
  g.dest = Make(WC_EDITW, g.destRoot.c_str(), ES_READONLY | ES_AUTOHSCROLL, IDC_DEST, WS_EX_CLIENTEDGE);
  g.destChange = Make(WC_BUTTONW, L"변경...", BS_PUSHBUTTON | WS_TABSTOP, IDC_DEST_CHANGE);
  g.progress = Make(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS);
  SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
  g.status = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS);
  g.done = Make(WC_BUTTONW, L"완료", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_DONE);

  g.upHeader = Make(L"STATIC", L"② Pixel 폰으로 업로드", SS_LEFT, IDC_UP_HEADER);
  g.upPhone = Make(L"STATIC", L"폰 상태 확인 중...", SS_LEFT | SS_ENDELLIPSIS, IDC_UP_PHONE);
  g.upDirLabel = Make(L"STATIC", L"업로드 폴더", SS_LEFT, IDC_UP_DIR_LABEL);
  g.upDir = Make(WC_EDITW, g.uploadDir.c_str(), ES_READONLY | ES_AUTOHSCROLL, IDC_UP_DIR, WS_EX_CLIENTEDGE);
  g.upDirChange = Make(WC_BUTTONW, L"변경...", BS_PUSHBUTTON | WS_TABSTOP, IDC_UP_DIR_CHANGE);
  g.upModeLabel = Make(L"STATIC", L"방식", SS_LEFT, IDC_UP_MODE_LABEL);
  g.upMode = Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_UP_MODE);
  for (const auto& mode : kModes) SendMessageW(g.upMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode.label));
  const DWORD mode = (std::min)(LoadNumber(kRegUploadMode, 0), static_cast<DWORD>(std::size(kModes) - 1));  // 처음엔 슬로우로 확인
  SendMessageW(g.upMode, CB_SETCURSEL, mode, 0);
  g.upQuietLabel = Make(L"STATIC", L"변동 없음(분)", SS_LEFT, IDC_UP_QUIET_LABEL);
  g.upQuiet = Make(WC_EDITW, std::to_wstring(LoadNumber(kRegQuietMinutes, static_cast<DWORD>(kModes[mode].defaultMinutes))).c_str(),
                   ES_NUMBER | ES_CENTER | WS_TABSTOP, IDC_UP_QUIET, WS_EX_CLIENTEDGE);
  g.upStart = Make(WC_BUTTONW, L"업로드 시작", BS_PUSHBUTTON | WS_TABSTOP, IDC_UP_START);
  g.upStatus = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_UP_STATUS);
  g.upBackup = Make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_UP_BACKUP);
  g.upLog = Make(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_UP_LOG, WS_EX_CLIENTEDGE);
  SendMessageW(g.upLog, EM_SETLIMITTEXT, 1 << 20, 0);
  SetTimer(hwnd, kPhoneTimer, 3000, nullptr);
  CheckPhoneAsync();
  ApplyFonts();
  Layout();
  UpdateButtons();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      Create(hwnd);
      return 0;
    case WM_SIZE:
      Layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize = {Scale(600), Scale(640)};
      return 0;
    }
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      const auto* r = reinterpret_cast<const RECT*>(lp);
      ApplyFonts();
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
      if (reinterpret_cast<HWND>(lp) == g.dest || reinterpret_cast<HWND>(lp) == g.upDir || reinterpret_cast<HWND>(lp) == g.upLog) break;
      SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DROPFILES:
      if (g.busy)
        DragFinish(reinterpret_cast<HDROP>(wp));
      else
        OnDrop(reinterpret_cast<HDROP>(wp));
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_ADD: OnAdd(); break;
        case IDC_REMOVE: OnRemove(); break;
        case IDC_CLEAR: OnClear(); break;
        case IDC_DEST_CHANGE: OnDestChange(); break;
        case IDC_DONE: OnDone(); break;
        case IDC_LIST:
          if (HIWORD(wp) == LBN_SELCHANGE) UpdateButtons();
          break;
        case IDC_UP_DIR_CHANGE: OnUploadDirChange(); break;
        case IDC_UP_START: OnUploadStart(); break;
        case IDC_UP_MODE:
          if (HIWORD(wp) == CBN_SELCHANGE) OnModeChange();
          break;
      }
      return 0;
    case WM_APP_PROGRESS:
      OnProgress();
      return 0;
    case WM_APP_FINISHED:
      OnFinished(std::unique_ptr<gpu::OrganizeResult>(reinterpret_cast<gpu::OrganizeResult*>(lp)));
      return 0;
    case WM_APP_UP_LOG: {
      std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
      AppendLog(*line);
      return 0;
    }
    case WM_APP_UP_STATUS: {
      std::unique_ptr<gpu::UploadStatus> st(reinterpret_cast<gpu::UploadStatus*>(lp));
      OnUploadStatus(*st);
      return 0;
    }
    case WM_APP_UP_DONE:
      g.uploader.Join();
      UpdateUploadControls();
      CheckPhoneAsync();
      return 0;
    case WM_APP_PHONE: {
      std::unique_ptr<std::wstring> state(reinterpret_cast<std::wstring*>(lp));
      if (!g.uploader.Running()) SetWindowTextW(g.upPhone, state->c_str());
      return 0;
    }
    case WM_TIMER:
      if (wp == kPhoneTimer) CheckPhoneAsync();
      return 0;
    case WM_CLOSE:
      if (g.busy && MessageBoxW(hwnd, L"파일을 옮기는 중입니다. 정말 종료할까요?", kTitle, MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
      if (g.uploader.Running() &&
          MessageBoxW(hwnd, L"폰 업로드 중입니다. 종료할까요?\n(폰에 넣은 묶음은 그대로 두고, 다시 켜서 [업로드 시작]을 누르면 이어서 확인합니다)", kTitle,
                      MB_YESNO | MB_ICONWARNING) != IDYES)
        return 0;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- 창 없는 실행 -------------------------------------------------------------

int RunHeadless(int argc, wchar_t** argv) {
  std::wstring dest;
  std::vector<std::wstring> sources;
  for (int i = 2; i < argc; ++i) {
    if (wcscmp(argv[i], L"--dest") == 0 && i + 1 < argc)
      dest = argv[++i];
    else
      sources.emplace_back(argv[i]);
  }
  if (dest.empty() || sources.empty()) return 1;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const auto r = gpu::Organize(sources, dest, [](gpu::Phase, size_t, size_t) {});
  CoUninitialize();
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* out = nullptr;
    _wfreopen_s(&out, L"CONOUT$", L"w", stdout);
    wprintf(L"moved=%zu chunks=%zu skipped=%zu failed=%zu warnings=%zu threads=%zu output=%ls\n", r.moved, r.chunks, r.skipped,
            r.failed.size(), r.warnings.size(), r.threads, r.outputDir.c_str());
    for (const auto& f : r.failed) wprintf(L"failed: %ls\n", f.c_str());
    for (const auto& w : r.warnings) wprintf(L"warning: %ls\n", w.c_str());
  }
  return r.failed.empty() && r.warnings.empty() ? 0 : 2;
}

// 업로드를 끝까지(모두 완료되거나 폰 없음이 오래 이어질 때까지) 돌리고 로그를 콘솔에 쓴다.
int RunHeadlessUpload(int argc, wchar_t** argv) {
  gpu::UploadOptions opt;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (wcscmp(argv[i], L"--dir") == 0) opt.runDir = argv[i + 1];
    else if (wcscmp(argv[i], L"--batch") == 0) opt.batchSize = static_cast<size_t>((std::max)(1, _wtoi(argv[i + 1])));
    else if (wcscmp(argv[i], L"--quiet-seconds") == 0) opt.quietSeconds = (std::max)(1, _wtoi(argv[i + 1]));
  }
  if (opt.runDir.empty()) return 1;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* out = nullptr;
    _wfreopen_s(&out, L"CONOUT$", L"w", stdout);
  }
  std::atomic<bool> done{false};
  std::atomic<bool> allUploaded{false};
  gpu::UploadEvents ev;
  ev.log = [](const std::wstring& line) {
    wprintf(L"%ls\n", line.c_str());
    fflush(stdout);
  };
  ev.status = [&](const gpu::UploadStatus& st) {
    if (st.total && st.uploaded == st.total) allUploaded = true;
  };
  ev.finished = [&] { done = true; };
  gpu::Uploader uploader;
  uploader.Start(opt, ev);
  while (!done) Sleep(100);
  uploader.Join();
  return allUploaded ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 2 && wcscmp(argv[1], L"--organize") == 0) return RunHeadless(argc, argv);
  if (argc >= 2 && wcscmp(argv[1], L"--upload") == 0) return RunHeadlessUpload(argc, argv);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_USEREX_CLASSES};
  InitCommonControlsEx(&icc);
  g.destRoot = LoadDest();
  g.uploadDir = LoadString(kRegLastOutput);

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"GooglePhotoUploaderWindow";
  RegisterClassExW(&wc);

  const std::wstring title = std::wstring(kTitle) + L"  " + GPU_VERSION;
  const UINT dpi = GetDpiForSystem();
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int height = (std::min)(MulDiv(920, dpi, 96), static_cast<int>(work.bottom - work.top));
  HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              MulDiv(700, dpi, 96), height, nullptr, nullptr, inst, nullptr);
  ShowWindow(hwnd, show);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (IsDialogMessageW(hwnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CoUninitialize();
  return 0;
}
