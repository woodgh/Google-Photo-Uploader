#pragma once
// 선택한 폴더/파일 → 사진·동영상을 촬영 시각 순으로 정렬해 100개씩 하위 폴더로 이동한다.
//   <저장 폴더>\<실행 시각>\001\2024_05_01_13_ab12cd34.jpg
#include <functional>
#include <string>
#include <vector>

namespace gpu {

enum class Phase { Collect, Scan, Move };

struct OrganizeResult {
  std::wstring outputDir;            // 이번 실행으로 만든 폴더 (옮긴 파일이 없으면 빈 문자열)
  size_t moved = 0;
  size_t chunks = 0;
  size_t skipped = 0;                // 사진·동영상이 아니라 그대로 둔 파일
  size_t threads = 1;                  // 읽기·이동 워커 스레드 수 (논리 프로세서 수)
  std::vector<std::wstring> failed;    // 옮기지 못한 파일 "경로: 사유"
  std::vector<std::wstring> warnings;  // 옮겼지만 날짜·속성을 되돌리지 못한 파일
};

// progress(phase, done, total): 작업 스레드에서 호출된다. 읽기·이동 단계에서는 여러 스레드가 동시에 부른다.
using ProgressFn = std::function<void(Phase, size_t, size_t)>;

// 경로가 속한 볼륨의 루트 ("C:\\"). 실패하면 빈 문자열.
std::wstring VolumeOf(const std::wstring& path);

// 파일 이동 (다른 드라이브면 OS가 복사 후 원본 삭제). 만든·수정·접근 날짜와 속성을 원래 값으로 맞춘다.
// 이동 실패 시 false + error. 이동은 됐지만 날짜 복원에 실패하면 true + metadataRestored=false + error.
bool MovePreservingMetadata(const std::wstring& src, const std::wstring& dst, std::wstring& error, bool& metadataRestored);

// 호출 스레드에서 COM이 초기화되어 있어야 한다.
OrganizeResult Organize(const std::vector<std::wstring>& sources, const std::wstring& destRoot, const ProgressFn& progress);

}  // namespace gpu
