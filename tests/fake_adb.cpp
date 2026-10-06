// CI용 가짜 adb.exe: 폰 하나가 연결된 것처럼 동작한다. 폰 저장소는 %FAKE_ADB_ROOT% 아래 폴더로 흉내 낸다.
//   /sdcard/DCIM/Camera/x.jpg → %FAKE_ADB_ROOT%\sdcard\DCIM\Camera\x.jpg
//   dumpsys notification       → %FAKE_ADB_ROOT%\notification.txt 내용 (없으면 빈 출력)
// 받은 명령은 %FAKE_ADB_ROOT%\calls.log에 한 줄씩 남긴다.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static fs::path Root() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"FAKE_ADB_ROOT", buf, MAX_PATH);
  return n ? fs::path(buf) : fs::current_path();
}

static std::string ToUtf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

static fs::path FromUtf8(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return fs::path(w);
}

static fs::path Local(const std::string& phonePath) {
  std::string rel = phonePath;
  while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
  return Root() / FromUtf8(rel);
}

static std::string Trim(std::string s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

static std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

static int ShellOne(std::string cmd) {
  const auto redirect = cmd.find(" >/dev/null");
  if (redirect != std::string::npos) cmd = cmd.substr(0, redirect);
  cmd = Trim(cmd);
  const auto w = Words(cmd);
  if (w.empty()) return 0;
  if (w[0] == "getprop") {
    std::cout << "Pixel\n";
  } else if (w[0] == "mkdir" && w.size() >= 3) {
    fs::create_directories(Local(w.back()));
  } else if (w[0] == "ls" && w.size() >= 3) {
    const auto dir = Local(w.back());
    if (!fs::exists(dir)) {
      std::cout << "ls: " << w.back() << ": No such file or directory\n";
      return 1;
    }
    for (const auto& e : fs::directory_iterator(dir)) std::cout << ToUtf8(e.path().filename().c_str()) << "\n";
  } else if (w[0] == "rm") {
    for (size_t i = 1; i < w.size(); ++i)
      if (w[i][0] != '-') fs::remove(Local(w[i]));
  } else if (w[0] == "dumpsys") {
    std::ifstream in(Root() / "notification.txt", std::ios::binary);
    std::cout << in.rdbuf();
  }  // am broadcast 등은 무시
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(ToUtf8(argv[i]));
  {
    std::ofstream log(Root() / "calls.log", std::ios::app | std::ios::binary);
    for (const auto& a : args) log << a << " | ";
    log << "\n";
  }
  size_t i = 0;
  if (i + 1 < args.size() && args[i] == "-s") i += 2;
  if (i >= args.size()) return 1;
  const auto& cmd = args[i];
  if (cmd == "devices") {
    std::cout << "List of devices attached\nFAKE0001\tdevice\n\n";
    return 0;
  }
  if (cmd == "push") {
    std::vector<std::string> rest(args.begin() + static_cast<long>(i) + 1, args.end());
    if (!rest.empty() && rest[0] == "-a") rest.erase(rest.begin());
    if (rest.size() != 2) return 1;
    const auto dst = Local(rest[1]);
    fs::create_directories(dst.parent_path());
    std::error_code ec;
    fs::copy_file(FromUtf8(rest[0]), dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      std::cout << "adb: error: " << ec.message() << "\n";
      return 1;
    }
    std::cout << rest[0] << ": 1 file pushed\n";
    return 0;
  }
  if (cmd == "shell" && i + 1 < args.size()) {
    std::string all;
    for (size_t k = i + 1; k < args.size(); ++k) all += (k > i + 1 ? " " : "") + args[k];
    int rc = 0;
    std::stringstream ss(all);
    for (std::string part; std::getline(ss, part, ';');) rc = ShellOne(part);
    return rc;
  }
  return 1;
}
