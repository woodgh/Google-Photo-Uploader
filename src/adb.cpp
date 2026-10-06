#include "adb.hpp"

#include <windows.h>

#include <algorithm>

namespace gpu {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

namespace {

std::wstring Quote(const std::wstring& arg) {
  // CommandLineToArgvW 규칙: 따옴표 앞의 역슬래시는 두 배로.
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'"') {
      out.append(slashes * 2 + 1, L'\\');
    } else {
      out.append(slashes, L'\\');
    }
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

ProcessResult RunProcess(const std::wstring& exe, const std::wstring& args, unsigned timeoutMs) {
  ProcessResult result;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return result;
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = Quote(exe) + L" " + args;
  const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writePipe);
  if (!started) {
    CloseHandle(readPipe);
    return result;
  }
  // 출력이 파이프 버퍼보다 크면 자식이 멈추므로 기다리는 동안 계속 비운다.
  // ReadFile로 EOF를 기다리지 않는다: adb가 띄운 서버가 파이프를 물려받으면 EOF가 오지 않기 때문.
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  auto drain = [&] {
    DWORD avail = 0;
    char buf[4096];
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD n = 0;
      if (!ReadFile(readPipe, buf, (std::min)(avail, static_cast<DWORD>(sizeof(buf))), &n, nullptr) || n == 0) break;
      result.output.append(buf, n);
    }
  };
  for (;;) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 20);
    drain();
    if (wait == WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      result.exitCode = static_cast<int>(code);
      break;
    }
    if (GetTickCount64() >= deadline) {
      TerminateProcess(pi.hProcess, 1);
      result.exitCode = -2;
      break;
    }
  }
  CloseHandle(readPipe);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return result;
}

}  // namespace

Adb::Adb() {
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  std::wstring dir(path, n);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  if (GetFileAttributesW((dir + L"adb.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
    exe_ = dir + L"adb.exe";
    return;
  }
  wchar_t found[MAX_PATH * 2];
  if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH * 2, found, nullptr)) exe_ = found;
}

ProcessResult Adb::Run(const std::string& serial, const std::vector<std::wstring>& args, unsigned timeoutMs) const {
  if (exe_.empty()) return {};
  std::wstring line;
  if (!serial.empty()) line += L"-s " + Quote(Utf8ToWide(serial)) + L" ";
  for (const auto& a : args) line += Quote(a) + L" ";
  return RunProcess(exe_, line, timeoutMs);
}

ProcessResult Adb::Shell(const std::string& serial, const std::string& command, unsigned timeoutMs) const {
  return Run(serial, {L"shell", Utf8ToWide(command)}, timeoutMs);
}

}  // namespace gpu
