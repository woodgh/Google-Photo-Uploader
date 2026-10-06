// Google-Photo-Uploader: 폴더를 끌어다 놓거나 골라서 [완료]를 누르면
// 사진·동영상을 100개씩 나눠 <저장 폴더>\<실행 시각>\001 ... 로 옮긴다. (파일 이름: 년_월_일_시_해쉬)
//
// 창 없이 실행(CI 확인용):
//   Google-Photo-Uploader.exe --organize --dest <저장 폴더> <원본 폴더>...
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core.hpp"
#include "organizer.hpp"
#include "version.h"

namespace {

constexpr wchar_t kTitle[] = L"Google Photo Uploader";
constexpr wchar_t kRegKey[] = L"Software\\Google-Photo-Uploader";
constexpr wchar_t kRegDest[] = L"Destination";

enum : int { IDC_HINT = 100, IDC_LIST, IDC_ADD, IDC_REMOVE, IDC_CLEAR, IDC_DEST_LABEL, IDC_DEST, IDC_DEST_CHANGE,
             IDC_PROGRESS, IDC_STATUS, IDC_DONE };
enum : UINT { WM_APP_PROGRESS = WM_APP + 1, WM_APP_FINISHED };

struct App {
  HWND hwnd = nullptr;
  HWND hint, list, add, remove, clear, destLabel, dest, destChange, progress, status, done;
  HFONT font = nullptr, bold = nullptr;
  UINT dpi = 96;
  std::vector<std::wstring> sources;
  std::wstring destRoot;
  bool busy = false;
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
  SetStatus(L"파일을 찾는 중...");
  std::thread([hwnd = g.hwnd, sources = g.sources, dest = g.destRoot] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = std::make_unique<gpu::OrganizeResult>(gpu::Organize(sources, dest, [hwnd](gpu::Phase phase, size_t done, size_t total) {
      // 진행률: 찾기 0~5%, 읽기 5~60%, 이동 60~100%
      const int base[] = {0, 50, 600}, span[] = {50, 550, 400};
      const int i = static_cast<int>(phase);
      const int pos = base[i] + (total ? static_cast<int>(span[i] * done / total) : 0);
      PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(phase), MAKELPARAM(pos, 0));
    }));
    CoUninitialize();
    PostMessageW(hwnd, WM_APP_FINISHED, 0, reinterpret_cast<LPARAM>(result.release()));
  }).detach();
}

void OnProgress(gpu::Phase phase, int pos) {
  static const wchar_t* const kText[] = {L"파일을 찾는 중...", L"촬영 시각과 해시를 읽는 중...", L"폴더로 옮기는 중..."};
  SendMessageW(g.progress, PBM_SETPOS, pos, 0);
  SetStatus(kText[static_cast<int>(phase)]);
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
    ShellExecuteW(g.hwnd, L"open", r->outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  UpdateButtons();
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
  for (HWND h : {g.list, g.add, g.remove, g.clear, g.destLabel, g.dest, g.destChange, g.status})
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
  for (HWND h : {g.hint, g.done}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.bold), TRUE);
}

void Layout() {
  RECT rc;
  GetClientRect(g.hwnd, &rc);
  const int m = Scale(16), gap = Scale(8), w = rc.right - 2 * m, bh = Scale(32), bw = Scale(110);
  int y = m;
  MoveWindow(g.hint, m, y, w, Scale(24), TRUE);
  y += Scale(24) + gap;
  const int bottom = rc.bottom - m - Scale(44) - gap - Scale(20) - gap - Scale(18) - gap - bh - gap - bh - gap;
  const int listH = (std::max)(Scale(80), bottom - y);
  MoveWindow(g.list, m, y, w, listH, TRUE);
  y += listH + gap;
  MoveWindow(g.add, m, y, bw, bh, TRUE);
  MoveWindow(g.remove, m + bw + gap, y, bw, bh, TRUE);
  MoveWindow(g.clear, m + 2 * (bw + gap), y, bw, bh, TRUE);
  y += bh + gap * 2;
  const int lw = Scale(72);
  MoveWindow(g.destLabel, m, y + Scale(7), lw, Scale(20), TRUE);
  MoveWindow(g.dest, m + lw, y + Scale(2), w - lw - bw - gap, Scale(26), TRUE);
  MoveWindow(g.destChange, rc.right - m - bw, y, bw, bh, TRUE);
  y += bh + gap;
  MoveWindow(g.progress, m, y, w, Scale(18), TRUE);
  y += Scale(18) + gap;
  MoveWindow(g.status, m, y, w, Scale(20), TRUE);
  MoveWindow(g.done, m, rc.bottom - m - Scale(44), w, Scale(44), TRUE);
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
  g.hint = Make(L"STATIC", L"업로드할 폴더를 끌어다 놓거나 [폴더 추가]로 선택하세요", SS_LEFT, IDC_HINT);
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
      mmi->ptMinTrackSize = {Scale(520), Scale(440)};
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
      if (reinterpret_cast<HWND>(lp) == g.dest) break;
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
      }
      return 0;
    case WM_APP_PROGRESS:
      OnProgress(static_cast<gpu::Phase>(wp), LOWORD(lp));
      return 0;
    case WM_APP_FINISHED:
      OnFinished(std::unique_ptr<gpu::OrganizeResult>(reinterpret_cast<gpu::OrganizeResult*>(lp)));
      return 0;
    case WM_CLOSE:
      if (g.busy && MessageBoxW(hwnd, L"파일을 옮기는 중입니다. 정말 종료할까요?", kTitle, MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
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

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 2 && wcscmp(argv[1], L"--organize") == 0) return RunHeadless(argc, argv);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  g.destRoot = LoadDest();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"GooglePhotoUploaderWindow";
  RegisterClassExW(&wc);

  const std::wstring title = std::wstring(kTitle) + L"  " + GPU_VERSION;
  const UINT dpi = GetDpiForSystem();
  HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              MulDiv(640, dpi, 96), MulDiv(560, dpi, 96), nullptr, nullptr, inst, nullptr);
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
