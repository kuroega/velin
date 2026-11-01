; SPDX-License-Identifier: GPL-3.0-or-later
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!ifndef VERSION
!error "VERSION must identify the application version."
!endif
!ifndef STAGE
!error "STAGE must identify the CMake install staging directory."
!endif
!ifndef OUTPUT
!error "OUTPUT must identify the installer output file."
!endif
!ifndef PAYLOAD_INSTALL
!error "PAYLOAD_INSTALL must identify the generated payload include."
!endif
!ifndef PAYLOAD_UNINSTALL
!error "PAYLOAD_UNINSTALL must identify the generated uninstall include."
!endif

Name "Velin"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Velin"
InstallDirRegKey HKCU "Software\Velin" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64
BrandingText "Velin media player"
Icon "..\resources\velin.ico"
UninstallIcon "..\resources\velin.ico"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Velin"
VIAddVersionKey /LANG=1033 "FileDescription" "Velin per-user installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (C) 2026 Velin contributors"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Velin requires 64-bit Windows."
    Abort
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Velin player (required)" Core
  SectionIn RO
  !include "${PAYLOAD_INSTALL}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\Velin" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "DisplayName" "Velin"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "Publisher" "Velin contributors"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "DisplayIcon" "$INSTDIR\velin.exe,0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "QuietUninstallString" '$\"$INSTDIR\uninstall.exe$\" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\Velin"
  InitPluginsDir
  File /oname=$PLUGINSDIR\Create-ShortcutIfMissing.ps1 "Create-ShortcutIfMissing.ps1"
  ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Create-ShortcutIfMissing.ps1" -Path "$SMPROGRAMS\Velin\Velin.lnk" -Target "$INSTDIR\velin.exe"'
  ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Create-ShortcutIfMissing.ps1" -Path "$SMPROGRAMS\Velin\Uninstall Velin.lnk" -Target "$INSTDIR\uninstall.exe"'
SectionEnd

Section /o "Add media formats to Open with" Associations
  WriteRegStr HKCU "Software\Classes\Velin.Media" "" "Velin media file"
  WriteRegStr HKCU "Software\Classes\Velin.Media\DefaultIcon" "" '$\"$INSTDIR\velin.exe$\",0'
  WriteRegStr HKCU "Software\Classes\Velin.Media\shell\open\command" "" '$\"$INSTDIR\velin.exe$\" $\"%1$\"'
  WriteRegStr HKCU "Software\Velin\Capabilities" "ApplicationName" "Velin"
  WriteRegStr HKCU "Software\Velin\Capabilities" "ApplicationDescription" "Native Windows media player"
  !include "${PAYLOAD_INSTALL}.associations"
  WriteRegStr HKCU "Software\RegisteredApplications" "Velin" "Software\Velin\Capabilities"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section /o "Browser integration (velin: protocol)" Protocol
  WriteRegStr HKCU "Software\Classes\velin" "" "URL:Velin media protocol"
  WriteRegStr HKCU "Software\Classes\velin" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\velin\DefaultIcon" "" '$\"$INSTDIR\velin.exe$\",0'
  WriteRegStr HKCU "Software\Classes\velin\shell\open\command" "" '$\"$INSTDIR\velin.exe$\" $\"%1$\"'
SectionEnd

Section /o "Desktop shortcut" Desktop
  InitPluginsDir
  File /oname=$PLUGINSDIR\Create-ShortcutIfMissing.ps1 "Create-ShortcutIfMissing.ps1"
  ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Create-ShortcutIfMissing.ps1" -Path "$DESKTOP\Velin.lnk" -Target "$INSTDIR\velin.exe"'
SectionEnd

Section /o "Install Microsoft Edge WebView2 Runtime" WebView
  InitPluginsDir
  File /oname=$PLUGINSDIR\Install-WebView2.ps1 "Install-WebView2.ps1"
  ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\Install-WebView2.ps1"' $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "WebView2 could not be installed. Native playback is available; plugin web panels require the WebView2 Runtime."
  ${EndIf}
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Uninstall"
  ReadRegStr $0 HKCU "Software\Classes\velin\shell\open\command" ""
  ${If} $0 == '$\"$INSTDIR\velin.exe$\" $\"%1$\"'
    DeleteRegKey HKCU "Software\Classes\velin"
  ${EndIf}
  ReadRegStr $0 HKCU "Software\Classes\Velin.Media\shell\open\command" ""
  ${If} $0 == '$\"$INSTDIR\velin.exe$\" $\"%1$\"'
    !include "${PAYLOAD_UNINSTALL}.associations"
    DeleteRegKey HKCU "Software\Classes\Velin.Media"
    DeleteRegKey HKCU "Software\Velin\Capabilities"
    DeleteRegValue HKCU "Software\RegisteredApplications" "Velin"
  ${EndIf}
  ; A later installation can own the shared shortcuts and registration. An older
  ; uninstaller must not remove them merely because their names are the same.
  ReadRegStr $0 HKCU "Software\Velin" "InstallLocation"
  ${If} $0 == $INSTDIR
    ; Preserve a shortcut if it was replaced or its target cannot be inspected.
    InitPluginsDir
    File /oname=$PLUGINSDIR\Remove-OwnedShortcut.ps1 "Remove-OwnedShortcut.ps1"
    ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Remove-OwnedShortcut.ps1" -Path "$DESKTOP\Velin.lnk" -Target "$INSTDIR\velin.exe"'
    ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Remove-OwnedShortcut.ps1" -Path "$SMPROGRAMS\Velin\Velin.lnk" -Target "$INSTDIR\velin.exe"'
    ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\Remove-OwnedShortcut.ps1" -Path "$SMPROGRAMS\Velin\Uninstall Velin.lnk" -Target "$INSTDIR\uninstall.exe"'
    RMDir "$SMPROGRAMS\Velin"
    DeleteRegValue HKCU "Software\Velin" "InstallLocation"
  ${EndIf}
  !include "${PAYLOAD_UNINSTALL}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin" "InstallLocation"
  ${If} $0 == $INSTDIR
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin"
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  ; Application data, playback history, user scripts, and unrelated files are retained.
SectionEnd
