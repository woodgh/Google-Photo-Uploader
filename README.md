# Google-Photo-Uploader

Google 포토에 올리기 전에 사진·동영상을 **100개씩 폴더로 나누고 이름을 정리**하는 Windows 앱입니다.

## 받기

[Releases → Google-Photo-Uploader (main)](https://github.com/woodgh/Google-Photo-Uploader/releases/tag/uploader-main)에서
`Google-Photo-Uploader-<날짜>-<빌드>.zip`을 받아 압축을 풀고 `Google-Photo-Uploader.exe`를 실행합니다. (설치 없음)
서명되지 않은 실행 파일이라 SmartScreen 경고가 뜨면 [추가 정보] → [실행]을 누르세요.

## 사용법

1. 정리할 폴더 여러 개를 창에 **끌어다 놓거나** [폴더 추가...]로 고릅니다. (하위 폴더까지 모두 포함)
2. 필요하면 [변경...]으로 **저장 폴더**를 바꿉니다. 기본값은 `사진\Google-Photo-Uploader`이며 다음 실행 때도 기억합니다.
3. [완료]를 누르면 정리가 끝난 뒤 결과 폴더가 열립니다.

## 결과

```
<저장 폴더>\2026-10-06_22-30-15\      ← 실행 시각
    001\2019_07_14_09_3fa2c81d.jpg     ← 100개
    002\...                            ← 100개
    003\2024_05_01_13_ab12cd34.mp4     ← 나머지
```

- 파일은 **이동**됩니다(복사 아님). 원본 폴더 구조는 남고 사진·동영상만 빠집니다.
- 이름: `년_월_일_시_해쉬.확장자`
  - 시각: 사진은 EXIF 촬영 시각, 동영상은 촬영(인코딩) 시각, 없으면 파일 수정 시각 (내 PC 시간대 기준)
  - 해쉬: 파일 내용 SHA-256의 앞 8자리. 같은 이름이 이미 있으면 `_2`, `_3`을 붙입니다.
- 오래된 파일부터 `001` 폴더에 들어갑니다.
- 사진·동영상(jpg, png, heic, raw, mp4, mov 등)만 옮기고 다른 파일은 제자리에 둡니다.

## 빌드

Visual Studio 2022 + CMake 3.24 이상:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

main에 push하면 GitHub Actions가 빌드·테스트 후 Release `uploader-main`의 zip을 최신 빌드로 바꿉니다.
