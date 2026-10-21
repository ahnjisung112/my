; 밝기조절 설치 프로그램 (NSIS, 관리자 권한 불필요 — 현재 사용자에게 설치)
Unicode true
Target amd64-unicode
!include "MUI2.nsh"

Name "밝기조절"
OutFile "out/BrightnessSetup.exe"
InstallDir "$LOCALAPPDATA\Programs\BrightnessControl"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "밝기조절 1.0"

!define MUI_ICON "app.ico"
!define MUI_UNICON "app.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Brightness.exe"
!define MUI_FINISHPAGE_RUN_TEXT "지금 밝기조절 실행"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BrightnessControl"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Korean"

Section "밝기조절 (필수)" SecMain
  SectionIn RO
  ; 실행 중이면 종료 후 덮어쓰기
  nsExec::Exec 'taskkill /IM Brightness.exe /F'
  SetOutPath "$INSTDIR"
  File "out/Brightness.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\밝기조절"
  CreateShortcut "$SMPROGRAMS\밝기조절\밝기조절.lnk" "$INSTDIR\Brightness.exe"
  CreateShortcut "$SMPROGRAMS\밝기조절\밝기조절 제거.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "밝기조절"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\Brightness.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "1.0.0"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "바탕화면 바로가기" SecDesktop
  CreateShortcut "$DESKTOP\밝기조절.lnk" "$INSTDIR\Brightness.exe"
SectionEnd

Section "Windows 시작 시 자동 실행" SecStartup
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "BrightnessControl" '"$INSTDIR\Brightness.exe"'
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM Brightness.exe /F'
  Delete "$INSTDIR\Brightness.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\밝기조절\밝기조절.lnk"
  Delete "$SMPROGRAMS\밝기조절\밝기조절 제거.lnk"
  RMDir "$SMPROGRAMS\밝기조절"
  Delete "$DESKTOP\밝기조절.lnk"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "BrightnessControl"
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
