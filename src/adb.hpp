#pragma once
// adb.exe 실행. 앱 옆의 adb.exe를 먼저 쓰고, 없으면 PATH에서 찾는다.
#include <string>
#include <vector>

namespace gpu {

struct ProcessResult {
  int exitCode = -1;   // 실행 실패 -1, 시간 초과 -2
  std::string output;  // stdout + stderr (UTF-8)
};

class Adb {
 public:
  Adb();
  bool Found() const { return !exe_.empty(); }
  const std::wstring& Exe() const { return exe_; }

  // serial이 비어 있지 않으면 -s serial을 붙인다. 인자는 각각 따옴표로 감싼다.
  ProcessResult Run(const std::string& serial, const std::vector<std::wstring>& args, unsigned timeoutMs = 60000) const;
  ProcessResult Shell(const std::string& serial, const std::string& command, unsigned timeoutMs = 60000) const;

 private:
  std::wstring exe_;
};

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

}  // namespace gpu
