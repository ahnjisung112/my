# 모니터 밝기 조절기 (Windows)

화면 구석에 작은 창이 뜨고, **창 위에서 마우스 휠**을 굴리면 밝기가 바뀝니다.
꺼진 것처럼 보일 만큼(0% = 완전 검정) 어둡게 만들 수 있습니다.

- `win/` — **설치용 Windows 프로그램 (C, Python 불필요)** ← 권장
- `brightness.py` — 같은 기능의 Python 버전

## 동작 방식
| 구간 | 하는 일 |
|---|---|
| 50 ~ 100% | 모니터 자체 밝기(백라이트) 조절 — 외장 모니터는 DDC/CI, 노트북은 WMI |
| 1 ~ 50% | 모니터 밝기는 최저로 두고, 클릭이 통과하는 검은 막을 화면 전체에 덮어 더 어둡게 |
| 0% | 완전 검정 (화면이 꺼진 것처럼 보임) |

모니터가 DDC/CI를 지원하지 않으면 0~100% 전체를 검은 막으로 조절합니다.

## 조작
- **마우스 휠** (창 위, 포커스 없어도 됨): 2% · **Shift+휠** 1% · **Ctrl+휠** 10%
- 막대 클릭/드래그, 방향키, 우클릭 메뉴(100/75/50/25/10/0%, 숨기기, 종료)
- 창 드래그로 이동, ✕ 종료, Esc 숨기기
- 트레이 아이콘: 클릭 = 창 보이기/숨기기, 우클릭 = 메뉴
- **전역 단축키**: `Ctrl+Alt+PageUp` 밝게 · `Ctrl+Alt+PageDown` 어둡게 · `Ctrl+Alt+Home` 100%

## 빌드 (Linux에서 Windows용으로)
```sh
sudo apt install gcc-mingw-w64-x86-64 nsis
brightness/win/build.sh   # -> win/out/Brightness.exe, win/out/BrightnessSetup.exe
```
설치 프로그램은 관리자 권한 없이 `%LOCALAPPDATA%\Programs\BrightnessControl`에 설치하고,
시작 메뉴·바탕화면 바로가기와 (선택) Windows 시작 시 자동 실행을 등록합니다.
