; WhichPC installer (NSIS 3, Unicode). Per-user install, no administrator rights needed.
;
; Command line (all optional):
;   /S                  silent install
;   /TYPE=laptop        preselect / force the device type (laptop | desktop)
;   /OVERLAY=1          also show the on-screen corner badge
;   /NOAUTOSTART        do not start WhichPC with Windows
;   /D=C:\path          install directory (must be last)

Unicode True
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!define APPNAME   "WhichPC"
!define APPEXE    "WhichPC.exe"
!define MAINCLASS "WhichPC.MainWindow"
!define REGKEY    "Software\WhichPC"
!define RUNKEY    "Software\Microsoft\Windows\CurrentVersion\Run"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WhichPC"

Name "WhichPC"
Caption "WhichPC ${VERSION} 설치"
UninstallCaption "WhichPC 제거"
BrandingText "WhichPC ${VERSION}"
OutFile "..\dist\WhichPC-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\WhichPC"
InstallDirRegKey HKCU "${UNINSTKEY}" "InstallLocation"
ShowInstDetails nevershow
ShowUninstDetails nevershow

VIProductVersion "${VERSION}.0"
VIFileVersion "${VERSION}.0"
VIAddVersionKey /LANG=1042 "ProductName" "WhichPC"
VIAddVersionKey /LANG=1042 "FileDescription" "WhichPC 설치 프로그램"
VIAddVersionKey /LANG=1042 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1042 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1042 "LegalCopyright" "(c) 2026 WhichPC"

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

!define MUI_ICON "..\assets\app.ico"
!define MUI_UNICON "..\assets\app.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\assets\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "..\assets\wizard.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "..\assets\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "..\assets\header.bmp"
!define MUI_ABORTWARNING
!define MUI_ABORTWARNING_TEXT "WhichPC 설치를 취소할까요?"

Var DeviceType      ; 0 = laptop, 1 = desktop
Var Autostart       ; 1 / 0
Var Overlay         ; 1 / 0
Var DetectText
Var Dialog
Var RadioLaptop
Var RadioDesktop
Var ChkAutostart
Var ChkOverlay
Var IconLaptop
Var IconDesktop
Var hIconLaptop
Var hIconDesktop
Var WasRunning      ; 1 if an older WhichPC was running when setup started

; ---------------------------------------------------------------------------
; pages
; ---------------------------------------------------------------------------
!define MUI_WELCOMEPAGE_TITLE "WhichPC 설치"
!define MUI_WELCOMEPAGE_TEXT "WhichPC는 지금 보고 있는 화면이 노트북인지 데스크탑인지 작업 표시줄 아이콘으로 알려 줍니다.$\r$\n$\r$\n노트북과 데스크탑에 각각 설치하고, 설치 중에 이 PC의 종류를 고르세요. 노트북에서 데스크탑으로 원격 접속하면, 원격 화면의 작업 표시줄에는 데스크탑 아이콘이 보입니다.$\r$\n$\r$\n관리자 권한 없이 현재 사용자 계정에만 설치됩니다.$\r$\n$\r$\n계속하려면 [다음]을 누르세요."
!insertmacro MUI_PAGE_WELCOME
Page custom DevicePageCreate DevicePageLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "설치 완료"
!define MUI_FINISHPAGE_TEXT_LARGE
!define MUI_FINISHPAGE_TEXT "WhichPC가 설치되었습니다.$\r$\n$\r$\n• 작업 표시줄 오른쪽(알림 영역)에 아이콘이 나타납니다.$\r$\n• 아이콘을 클릭하면 화면 가운데에 크게 표시됩니다.$\r$\n• 아이콘을 우클릭하면 종류·색·화면 배지를 바꿀 수 있습니다.$\r$\n$\r$\n다른 PC에도 같은 설치 파일로 설치하고 종류만 다르게 고르세요."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APPEXE}"
!define MUI_FINISHPAGE_RUN_TEXT "WhichPC 지금 실행"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Korean"
SetFont /LANG=${LANG_KOREAN} "Malgun Gothic" 9

; ---------------------------------------------------------------------------
; helpers
; ---------------------------------------------------------------------------
!macro CLOSE_RUNNING UN
; Asks a running WhichPC to quit and waits until the process has really exited
; (so its exe is no longer locked). Sets $WasRunning to 1 when one was found.
Function ${UN}CloseRunning
  StrCpy $WasRunning 0
  FindWindow $0 "${MAINCLASS}"
  ${If} $0 <> 0
    StrCpy $WasRunning 1
    DetailPrint "실행 중인 WhichPC 종료..."
    System::Call 'user32::GetWindowThreadProcessId(p r0, *i .r2) i .r4'
    System::Call 'kernel32::OpenProcess(i 0x00100000, i 0, i r2) p .r3'  ; SYNCHRONIZE
    SendMessage $0 ${WM_CLOSE} 0 0 /TIMEOUT=3000
    ${If} $3 P<> 0
      System::Call 'kernel32::WaitForSingleObject(p r3, i 8000) i .r4'
      System::Call 'kernel32::CloseHandle(p r3)'
    ${Else}
      StrCpy $1 0
      ${DoWhile} $1 < 30
        Sleep 150
        FindWindow $0 "${MAINCLASS}"
        ${If} $0 = 0
          ${Break}
        ${EndIf}
        IntOp $1 $1 + 1
      ${Loop}
      Sleep 500
    ${EndIf}
  ${EndIf}
FunctionEnd
!macroend
!insertmacro CLOSE_RUNNING ""
!insertmacro CLOSE_RUNNING "un."

; Loads an .ico at 48 px (scaled for the dialog's DPI) into a static control.
!macro SET_BIG_ICON CONTROL FILE OUTHANDLE
  System::Call 'user32::GetDpiForWindow(p $Dialog) i .r9'
  ${If} $9 < 96
    StrCpy $9 96
  ${EndIf}
  IntOp $8 $9 * 48
  IntOp $8 $8 / 96
  System::Call 'user32::LoadImageW(p 0, w "${FILE}", i 1, i r8, i r8, i 0x10) p .s'
  Pop ${OUTHANDLE}
  SendMessage ${CONTROL} ${STM_SETICON} ${OUTHANDLE} 0
!macroend

Function .onInit
  InitPluginsDir
  File /oname=$PLUGINSDIR\laptop.ico "..\assets\laptop.ico"
  File /oname=$PLUGINSDIR\desktop.ico "..\assets\desktop.ico"
  File /oname=$PLUGINSDIR\${APPEXE} "..\build\${APPEXE}"

  StrCpy $Autostart 1
  StrCpy $Overlay 0

  ; automatic detection: exit code = 1 + device + 10 * reason
  ExecWait '"$PLUGINSDIR\${APPEXE}" --detect' $0
  IntOp $1 $0 % 10
  IntOp $1 $1 - 1
  IntOp $2 $0 / 10
  ${If} $1 <> 0
  ${AndIf} $1 <> 1
    StrCpy $1 1
    StrCpy $2 0
  ${EndIf}
  StrCpy $DeviceType $1
  ${If} $1 = 0
    StrCpy $3 "노트북"
  ${Else}
    StrCpy $3 "데스크탑"
  ${EndIf}
  ${If} $2 = 1
    StrCpy $DetectText "자동 감지 결과: 이 PC는 $3(으)로 보입니다 (섀시 정보 기준)."
  ${ElseIf} $2 = 2
    StrCpy $DetectText "자동 감지 결과: 이 PC는 $3(으)로 보입니다 (배터리 유무 기준)."
  ${Else}
    StrCpy $DetectText "PC 종류를 자동으로 판단하지 못했습니다. 직접 골라 주세요."
  ${EndIf}

  ; an earlier installation wins over detection
  ClearErrors
  ReadRegDWORD $4 HKCU "${REGKEY}" "DeviceType"
  ${IfNot} ${Errors}
  ${AndIf} $4 >= 0
  ${AndIf} $4 <= 1
    StrCpy $DeviceType $4
    StrCpy $DetectText "$DetectText  (이전 설치에서 고른 값을 불러왔습니다.)"
  ${EndIf}
  ClearErrors
  ReadRegDWORD $4 HKCU "${REGKEY}" "Overlay"
  ${IfNot} ${Errors}
    StrCpy $Overlay $4
  ${EndIf}
  ClearErrors
  ReadRegStr $4 HKCU "${RUNKEY}" "WhichPC"
  ReadRegStr $5 HKCU "${UNINSTKEY}" "DisplayVersion"
  ${If} $4 == ""
  ${AndIf} $5 != ""
    StrCpy $Autostart 0  ; upgrade of an install that had autostart switched off
  ${EndIf}

  ; command line overrides
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/TYPE=" $R1
  ${IfNot} ${Errors}
    ${If} $R1 == "laptop"
      StrCpy $DeviceType 0
    ${ElseIf} $R1 == "desktop"
      StrCpy $DeviceType 1
    ${EndIf}
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/OVERLAY=" $R1
  ${IfNot} ${Errors}
    ${If} $R1 == "1"
      StrCpy $Overlay 1
    ${Else}
      StrCpy $Overlay 0
    ${EndIf}
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/NOAUTOSTART" $R1
  ${IfNot} ${Errors}
    StrCpy $Autostart 0
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; "which PC is this?" page
; ---------------------------------------------------------------------------
Function OnLaptopIcon
  ${NSD_Check} $RadioLaptop
  ${NSD_Uncheck} $RadioDesktop
FunctionEnd

Function OnDesktopIcon
  ${NSD_Check} $RadioDesktop
  ${NSD_Uncheck} $RadioLaptop
FunctionEnd

Function DevicePageCreate
  !insertmacro MUI_HEADER_TEXT "이 PC는 노트북인가요, 데스크탑인가요?" "작업 표시줄에 표시할 아이콘과 이름이 정해집니다."
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}

  ${NSD_CreateIcon} 4u 2u 30u 30u ""
  Pop $IconLaptop
  !insertmacro SET_BIG_ICON $IconLaptop "$PLUGINSDIR\laptop.ico" $hIconLaptop
  ${NSD_OnClick} $IconLaptop OnLaptopIcon
  ${NSD_CreateRadioButton} 42u 4u 250u 12u "노트북"
  Pop $RadioLaptop
  ${NSD_AddStyle} $RadioLaptop ${WS_GROUP}
  ${NSD_CreateLabel} 54u 17u 240u 10u "들고 다니는 PC. 밖에서 데스크탑으로 원격 접속할 때 쓰는 기기"
  Pop $0

  ${NSD_CreateIcon} 4u 38u 30u 30u ""
  Pop $IconDesktop
  !insertmacro SET_BIG_ICON $IconDesktop "$PLUGINSDIR\desktop.ico" $hIconDesktop
  ${NSD_OnClick} $IconDesktop OnDesktopIcon
  ${NSD_CreateRadioButton} 42u 40u 250u 12u "데스크탑"
  Pop $RadioDesktop
  ${NSD_CreateLabel} 54u 53u 240u 10u "집에 있는 고사양 PC. 로컬 AI·게임·3D 작업을 하는 기기"
  Pop $0

  ${NSD_CreateLabel} 4u 74u 292u 18u "$DetectText"
  Pop $0
  SetCtlColors $0 0x4B5563 transparent

  ${NSD_CreateHLine} 4u 96u 292u 1u ""
  Pop $0

  ${NSD_CreateCheckbox} 4u 103u 292u 12u "Windows 시작 시 자동 실행 (권장)"
  Pop $ChkAutostart
  ${NSD_CreateCheckbox} 4u 118u 292u 12u "화면 모서리에도 배지 표시 (전체 화면 게임·원격 화면에서도 구분)"
  Pop $ChkOverlay

  ${If} $DeviceType = 0
    ${NSD_Check} $RadioLaptop
  ${Else}
    ${NSD_Check} $RadioDesktop
  ${EndIf}
  ${If} $Autostart = 1
    ${NSD_Check} $ChkAutostart
  ${EndIf}
  ${If} $Overlay = 1
    ${NSD_Check} $ChkOverlay
  ${EndIf}

  nsDialogs::Show
  System::Call 'user32::DestroyIcon(p $hIconLaptop)'
  System::Call 'user32::DestroyIcon(p $hIconDesktop)'
FunctionEnd

Function DevicePageLeave
  ${NSD_GetState} $RadioLaptop $0
  ${If} $0 = ${BST_CHECKED}
    StrCpy $DeviceType 0
  ${Else}
    StrCpy $DeviceType 1
  ${EndIf}
  ${NSD_GetState} $ChkAutostart $0
  ${If} $0 = ${BST_CHECKED}
    StrCpy $Autostart 1
  ${Else}
    StrCpy $Autostart 0
  ${EndIf}
  ${NSD_GetState} $ChkOverlay $0
  ${If} $0 = ${BST_CHECKED}
    StrCpy $Overlay 1
  ${Else}
    StrCpy $Overlay 0
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; install
; ---------------------------------------------------------------------------
Section "WhichPC" SecMain
  SectionIn RO
  Call CloseRunning

  SetOutPath "$INSTDIR"
  File "..\build\${APPEXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegDWORD HKCU "${REGKEY}" "DeviceType" $DeviceType
  WriteRegDWORD HKCU "${REGKEY}" "Overlay" $Overlay
  ${If} $Autostart = 1
    WriteRegStr HKCU "${RUNKEY}" "WhichPC" '"$INSTDIR\${APPEXE}" --autostart'
  ${Else}
    DeleteRegValue HKCU "${RUNKEY}" "WhichPC"
  ${EndIf}

  CreateShortCut "$SMPROGRAMS\WhichPC.lnk" "$INSTDIR\${APPEXE}" "" "$INSTDIR\${APPEXE}" 0 SW_SHOWNORMAL "" "노트북/데스크탑 표시기 (실행 중이면 설정 열기)"

  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "WhichPC (노트북/데스크탑 표시기)"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "WhichPC"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${APPEXE},0"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" "$0"

  ${If} ${Silent}
    ${If} $Autostart = 1
    ${OrIf} $WasRunning = 1
      Exec '"$INSTDIR\${APPEXE}" --autostart'
    ${EndIf}
  ${EndIf}
SectionEnd

; ---------------------------------------------------------------------------
; uninstall
; ---------------------------------------------------------------------------
Section "Uninstall"
  Call un.CloseRunning
  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\WhichPC.lnk"
  DeleteRegValue HKCU "${RUNKEY}" "WhichPC"
  DeleteRegKey HKCU "${UNINSTKEY}"
  MessageBox MB_YESNO|MB_ICONQUESTION "WhichPC 설정(PC 종류, 색, 배지 옵션)도 삭제할까요?$\r$\n다시 설치할 때 그대로 쓰려면 [아니요]를 누르세요." /SD IDYES IDNO keep_settings
  DeleteRegKey HKCU "${REGKEY}"
  keep_settings:
SectionEnd
