Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
Name "TimeTrace"
!ifndef APP_VERSION
!error "Use scripts/Build.ps1 -Package to supply APP_VERSION."
!endif
OutFile "..\dist\TimeTrace-${APP_VERSION}-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\TimeTrace"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 16
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "TimeTrace"
VIAddVersionKey /LANG=1033 "FileDescription" "TimeTrace offline installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "TimeTrace contributors"
!define MUI_ICON "..\assets\app.ico"
!define MUI_UNICON "..\assets\app.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\TimeTrace.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
Function .onInit
    SetRegView 64
    ClearErrors
    ${GetOptions} "$CMDLINE" "/D=" $1
    ${IfNot} ${Errors}
      Return
    ${EndIf}
    ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TimeTrace" "InstallLocation"
    ${If} $0 != ""
      IfFileExists "$0\TimeTrace.exe" 0 +2
        StrCpy $INSTDIR $0
    ${EndIf}
FunctionEnd
Section "TimeTrace" SEC_APP
    SetShellVarContext current
    SetRegView 64
    IfFileExists "$INSTDIR\TimeTrace.exe" 0 +2
      ExecWait '"$INSTDIR\TimeTrace.exe" --stop'
    SetOutPath "$INSTDIR"
    File "..\dist\TimeTrace.exe"
    File "..\LICENSE.txt"
    File "..\README.md"
    ; Also stop an older copy running from a different folder for this same user profile.
    ExecWait '"$INSTDIR\TimeTrace.exe" --stop' $0
    ${If} $0 == 5
      MessageBox MB_OK|MB_ICONSTOP "TimeTrace 仍在保存数据。请稍后重新安装。"
      SetErrorLevel 1
      Abort
    ${EndIf}
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    ExecWait '"$INSTDIR\TimeTrace.exe" --register-install' $0
    ${If} $0 != 0
      MessageBox MB_OK|MB_ICONSTOP "未能完成通知和快捷方式注册。请重新运行安装程序。"
      SetErrorLevel 1
      Abort
    ${EndIf}
SectionEnd
Section "Uninstall"
    SetShellVarContext current
    SetRegView 64
    ; Remove shared registration only when this installation is still its owner.
    ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TimeTrace" "InstallLocation"
    StrCmp $0 $INSTDIR 0 keep_registration
    ExecWait '"$INSTDIR\TimeTrace.exe" --stop'
    Delete "$SMPROGRAMS\TimeTrace.lnk"
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TimeTrace"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TimeTrace"
    DeleteRegKey HKCU "Software\Classes\AppUserModelId\TimeTrace.Desktop"
    DeleteRegKey HKCU "Software\Classes\timetrace"
keep_registration:
    Delete "$INSTDIR\TimeTrace.exe"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    ; Preserve local history and reminder settings.
SectionEnd
