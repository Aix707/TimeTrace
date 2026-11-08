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
    ; Stage the new executable so a running copy is stopped before any installed file changes.
    InitPluginsDir
    File /oname=$PLUGINSDIR\TimeTrace.exe "..\dist\TimeTrace.exe"
    ClearErrors
    ExecWait '"$PLUGINSDIR\TimeTrace.exe" --stop' $0
    ${If} ${Errors}
      MessageBox MB_OK|MB_ICONSTOP "无法关闭 TimeTrace。请退出后重新安装。"
      SetErrorLevel 1
      Abort
    ${EndIf}
    ${If} $0 != 0
    ${AndIf} $0 != 2
      MessageBox MB_OK|MB_ICONSTOP "TimeTrace 仍在保存数据。请稍后重新安装。"
      SetErrorLevel 1
      Abort
    ${EndIf}
    SetOutPath "$INSTDIR"
    ClearErrors
    CopyFiles /SILENT "$PLUGINSDIR\TimeTrace.exe" "$INSTDIR\TimeTrace.exe"
    ${If} ${Errors}
      MessageBox MB_OK|MB_ICONSTOP "无法更新程序文件。请退出 TimeTrace 后重新安装。"
      SetErrorLevel 1
      Abort
    ${EndIf}
    File "..\LICENSE.txt"
    File "..\README.md"
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
    IfFileExists "$INSTDIR\TimeTrace.exe" 0 stopped
    ClearErrors
    ExecWait '"$INSTDIR\TimeTrace.exe" --stop' $0
    ${If} ${Errors}
      MessageBox MB_OK|MB_ICONSTOP "无法关闭 TimeTrace。请退出后重新卸载。"
      SetErrorLevel 1
      Abort
    ${EndIf}
    ${If} $0 != 0
    ${AndIf} $0 != 2
      MessageBox MB_OK|MB_ICONSTOP "TimeTrace 仍在保存数据。请稍后重新卸载。"
      SetErrorLevel 1
      Abort
    ${EndIf}
stopped:
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
