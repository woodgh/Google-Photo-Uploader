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
  std::vector<std::wstring> failed;  // "경로: 사유"
};

// progress(phase, done, total): 작업 스레드에서 호출된다.
using ProgressFn = std::function<void(Phase, size_t, size_t)>;

// 호출 스레드에서 COM이 초기화되어 있어야 한다.
OrganizeResult Organize(const std::vector<std::wstring>& sources, const std::wstring& destRoot, const ProgressFn& progress);

}  // namespace gpu
