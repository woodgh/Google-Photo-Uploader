#pragma once
// 플랫폼 독립 규칙: adb 출력 해석(기기 목록·Google 포토 백업 알림·폴더 목록)과 "변동 없음" 판정.
// (테스트는 tests/phone_model_tests.cpp)
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace gpu {

inline constexpr char kPhotosPackage[] = "com.google.android.apps.photos";
inline constexpr char kPhoneDir[] = "/sdcard/DCIM/Camera";  // Google 포토가 따로 설정 없이 백업하는 폴더

inline std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

inline std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

struct Device {
  std::string serial;
  std::string state;  // device, unauthorized, offline ...
};

// `adb devices` 출력
inline std::vector<Device> ParseDevices(const std::string& out) {
  std::vector<Device> devices;
  for (const auto& raw : Lines(out)) {
    const auto line = Trim(raw);
    if (line.empty() || line.rfind("List of devices", 0) == 0 || line[0] == '*') continue;
    const auto tab = line.find_first_of("\t ");
    if (tab == std::string::npos) continue;
    devices.push_back({line.substr(0, tab), Trim(line.substr(tab))});
  }
  return devices;
}

// `adb shell ls -1 <dir>` 출력 → 파일 이름들
inline std::set<std::string> ParseNames(const std::string& out) {
  std::set<std::string> names;
  for (const auto& raw : Lines(out)) {
    const auto name = Trim(raw);
    if (!name.empty() && name.find("No such file") == std::string::npos && name.find(':') == std::string::npos) names.insert(name);
  }
  return names;
}

// Google 포토 알림 상태
struct BackupSnapshot {
  bool active = false;     // 백업 진행 중으로 보이는 알림이 있음 (진행률·진행 중·category=progress)
  std::string summary;     // 화면 표시용: "백업 중 · 2개 남음" (알림이 없으면 빈 문자열)
  std::string signature;   // 변동 비교용 (제목·내용·진행률)
};

namespace detail {
// `key=Type (value)` 형식의 값. 없으면 빈 문자열.
inline std::string ExtraValue(const std::string& line, const std::string& key) {
  const auto pos = line.find(key + "=");
  if (pos == std::string::npos) return {};
  const auto open = line.find('(', pos);
  const auto close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open) return {};
  return line.substr(open + 1, close - open - 1);
}

inline long long ToNumber(const std::string& s, int base = 10) {
  try {
    return s.empty() ? 0 : std::stoll(s, nullptr, base);
  } catch (...) {
    return 0;
  }
}
}  // namespace detail

// `adb shell dumpsys notification --noredact` 출력에서 Google 포토 알림만 읽는다.
// 알림 문구는 폰 언어에 따라 달라서 판정에는 진행률·플래그·category만 쓰고, 문구는 표시·변동 비교에만 쓴다.
inline BackupSnapshot ParsePhotosNotifications(const std::string& dumpsys) {
  struct Record {
    std::string title, text;
    long long progress = 0, progressMax = 0;
    bool indeterminate = false, ongoing = false, progressCategory = false;
  };
  std::vector<Record> records;
  bool inPhotos = false;
  for (const auto& line : Lines(dumpsys)) {
    if (line.find("NotificationRecord(") != std::string::npos) {
      inPhotos = line.find(std::string("pkg=") + kPhotosPackage + " ") != std::string::npos;
      if (!inPhotos) continue;
      Record r;
      const auto flags = line.find(" flags=0x");
      if (flags != std::string::npos) r.ongoing = (detail::ToNumber(line.substr(flags + 9, 8), 16) & 0x2) != 0;  // FLAG_ONGOING_EVENT
      r.progressCategory = line.find("category=progress") != std::string::npos;
      records.push_back(r);
      continue;
    }
    if (!inPhotos || records.empty()) continue;
    auto& r = records.back();
    const auto t = Trim(line);
    if (t.rfind("android.title=", 0) == 0) r.title = detail::ExtraValue(t, "android.title");
    else if (t.rfind("android.text=", 0) == 0) r.text = detail::ExtraValue(t, "android.text");
    else if (t.rfind("android.progressMax=", 0) == 0) r.progressMax = detail::ToNumber(detail::ExtraValue(t, "android.progressMax"));
    else if (t.rfind("android.progress=", 0) == 0) r.progress = detail::ToNumber(detail::ExtraValue(t, "android.progress"));
    else if (t.rfind("android.progressIndeterminate=", 0) == 0) r.indeterminate = detail::ExtraValue(t, "android.progressIndeterminate") == "true";
  }

  BackupSnapshot snap;
  std::set<std::string> parts;  // 같은 알림이 여러 목록에 나올 수 있어 중복 제거 + 순서 고정
  for (const auto& r : records) {
    if (r.progressMax > 0 || r.indeterminate || r.ongoing || r.progressCategory) snap.active = true;
    std::string summary = r.title;
    if (!r.text.empty()) summary += (summary.empty() ? "" : " · ") + r.text;
    if (summary.empty()) continue;
    std::string sig = summary;
    if (r.progressMax > 0) sig += " [" + std::to_string(r.progress) + "/" + std::to_string(r.progressMax) + "]";
    parts.insert(sig);
  }
  for (const auto& p : parts) {
    if (!snap.summary.empty()) snap.summary += " / ";
    snap.summary += p;
    snap.signature += p + "\n";
  }
  return snap;
}

// 묶음을 폰에 넣은 뒤: 알림이 바뀌면 다시 센다. 백업 중이 아니고 quietSeconds 동안 변동이 없으면 완료.
class QuietTimer {
 public:
  void Reset(int64_t now) {
    lastChange_ = now;
    signature_.clear();
    active_ = false;
  }
  // 변동이 있었으면 true
  bool Observe(int64_t now, const BackupSnapshot& snap) {
    if (snap.signature == signature_ && snap.active == active_) return false;
    signature_ = snap.signature;
    active_ = snap.active;
    lastChange_ = now;
    return true;
  }
  int64_t QuietFor(int64_t now) const { return now - lastChange_; }
  bool Done(int64_t now, int64_t quietSeconds) const { return !active_ && QuietFor(now) >= quietSeconds; }

 private:
  int64_t lastChange_ = 0;
  std::string signature_;
  bool active_ = false;
};

}  // namespace gpu
