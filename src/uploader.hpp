#pragma once
// 정리된 폴더(<실행 시각>\001, 002 ...)의 파일을 묶음 단위로 Pixel 폰에 넣고, Google 포토 백업이 끝나면 다음 묶음을 넣는다.
//  1. 묶음(1/50/100개)을 폰 DCIM/Camera로 복사하고 미디어 스캔
//  2. Google 포토 알림을 지켜본다. 알림이 바뀌면 다시 센다
//  3. 백업 중이 아니고 정한 시간 동안 변동이 없으면 완료 → PC 파일을 <실행 시각>\업로드 완료\NNN\ 으로 옮기고 폰 사본 삭제
//  4. 다음 묶음으로. 앱을 다시 켜면 폰에 남아 있는 파일을 현재 묶음으로 보고 이어서 한다.
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace gpu {

inline constexpr wchar_t kUploadedFolder[] = L"업로드 완료";

struct UploadOptions {
  std::wstring runDir;
  size_t batchSize = 100;
  int quietSeconds = 30 * 60;
};

struct UploadStatus {
  std::wstring phone;   // 폰 연결 상태
  std::wstring stage;   // 현재 하는 일
  std::wstring backup;  // Google 포토 알림 (없으면 빈 문자열)
  bool backingUp = false;
  size_t uploaded = 0, total = 0, inBatch = 0;
  int quietFor = 0, quietNeeded = 0;  // 변동 없음 경과/필요 시간(초). quietNeeded 0이면 대기 중 아님
};

struct UploadEvents {
  std::function<void(const std::wstring&)> log;       // 작업 스레드에서 호출
  std::function<void(const UploadStatus&)> status;    // 작업 스레드에서 호출
  std::function<void()> finished;                     // 작업 스레드에서 마지막에 한 번
};

class Uploader {
 public:
  // 앱 종료 시: 기다리지 않는다 (진행 중인 adb 복사가 몇 분 걸릴 수 있음). 프로세스가 끝나면 스레드도 끝난다.
  ~Uploader() {
    stop_ = true;
    if (thread_.joinable()) thread_.detach();
  }
  void Start(const UploadOptions& options, const UploadEvents& events);
  void RequestStop() { stop_ = true; }  // 진행 중인 adb 명령이 끝나면 멈추고 finished를 부른다. 폰의 현재 묶음은 그대로 둔다
  void Join();                          // finished 이후 스레드 정리
  bool Running() const { return running_; }
  bool Stopping() const { return running_ && stop_; }

 private:
  void Run(UploadOptions options, UploadEvents events);
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false};
};

// 폰 연결 상태 한 줄 (업로드 중이 아닐 때 화면 표시용)
std::wstring DescribePhone();

}  // namespace gpu
