Unicode True

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef STAGE_DIR
  !error "STAGE_DIR must point to the deployed application directory"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE must point to the installer to create"
!endif
!ifndef APP_VERSION
  !error "APP_VERSION must contain MAJOR.MINOR.PATCH"
!endif
!ifndef APP_ICON
  !error "APP_ICON must point to the application icon"
!endif

!define APP_NAME "Lumen"
!define APP_PUBLISHER "Lumen"
!define APP_EXE "Lumen.exe"
; Stable upgrade identity: reuse the existing installation and uninstall entry.
!define APP_REGISTRY_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MVPImageViewer"

Name "${APP_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Lumen"
InstallDirRegKey HKCU "${APP_REGISTRY_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) Lumen contributors"

!define MUI_ABORTWARNING
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Var updatePid
Var updateStage
Var updateBackup
Var updateUnsafeTree

LangString UpdateRestartFailed ${LANG_ENGLISH} "Lumen could not complete the update. Try opening Lumen from the Start menu."
LangString UpdateRestartFailed ${LANG_SIMPCHINESE} "Lumen 无法完成更新。请尝试从开始菜单打开 Lumen。"
LangString UpdateRestoreFailed ${LANG_ENGLISH} "Lumen could not restore the previous installation. It is available at $updateBackup."
LangString UpdateRestoreFailed ${LANG_SIMPCHINESE} "Lumen 无法还原到原安装位置。旧版本位于 $updateBackup。"

Function .onInit
  ${GetParameters} $0
  ${GetOptions} "$0" "/UPDATEPID=" $updatePid
  IfErrors normal_start
  SetSilent silent
  # Hold this mutex until the process exits so two updates cannot swap the same directory.
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\LumenInAppUpdate")p.r1 ?e'
  Pop $2
  StrCmp $1 0 invalid_install
  IntCmp $2 183 invalid_install
  ReadRegStr $INSTDIR HKCU "${APP_REGISTRY_KEY}" "InstallLocation"
  StrCmp $INSTDIR "" invalid_install
  IfFileExists "$INSTDIR\${APP_EXE}" normal_start
invalid_install:
  SetErrorLevel 1
  Abort
normal_start:
FunctionEnd

Function ExtractApplication
  File /r "${STAGE_DIR}\*.*"
FunctionEnd

Function CheckUpdateTree
  Exch $0
  Push $1
  Push $2
  Push $3
  # Avoid copying or recursively removing junctions and other reparse points.
  System::Call 'kernel32::GetFileAttributesW(w r0)i.r3'
  IntCmp $3 -1 unsafe_tree
  IntOp $2 $3 & 0x0400
  StrCmp $2 0 0 unsafe_tree
  IntOp $2 $3 & 0x0010
  StrCmp $2 0 tree_done
  ClearErrors
  FindFirst $1 $2 "$0\*"
  IfErrors tree_done
tree_entry:
  StrCmp $2 "." next_entry
  StrCmp $2 ".." next_entry
  Push "$0\$2"
  Call CheckUpdateTree
  StrCmp $updateUnsafeTree 1 tree_close
next_entry:
  ClearErrors
  FindNext $1 $2
  IfErrors tree_close tree_entry
tree_close:
  FindClose $1
  Goto tree_done
unsafe_tree:
  StrCpy $updateUnsafeTree 1
tree_done:
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

Function InstallLumenUpdate
  # Let the running instance exit before replacing any files.
  SetOutPath "$TEMP"
  StrCpy $0 $updatePid
  System::Call 'kernel32::OpenProcess(i 0x00100000, i 0, i r0)p.r1'
  StrCmp $1 0 stage_update
  System::Call 'kernel32::WaitForSingleObject(p r1, i 60000)i.r2'
  System::Call 'kernel32::CloseHandle(p r1)'
  StrCmp $2 0 stage_update
  SetErrorLevel 1
  MessageBox MB_OK|MB_ICONSTOP "$(UpdateRestartFailed)"
  Return

stage_update:
  StrCpy $updateUnsafeTree 0
  Push "$INSTDIR"
  Call CheckUpdateTree
  StrCmp $updateUnsafeTree 1 update_failed
  # Stage beside the current installation so both renames stay on the same volume.
  # Copy existing files first to preserve anything outside the release payload.
  ClearErrors
  ${GetParent} "$INSTDIR" $0
  StrCmp $0 "" update_failed
  GetTempFileName $updateStage "$0"
  IfErrors update_failed
  GetTempFileName $updateBackup "$0"
  IfErrors update_failed
  Delete "$updateStage"
  CreateDirectory "$updateStage"
  IfErrors update_failed
  CopyFiles /SILENT "$INSTDIR\*.*" "$updateStage"
  IfErrors update_failed
  SetOutPath "$updateStage"
  Call ExtractApplication
  IfErrors update_failed
  WriteUninstaller "$updateStage\Uninstall.exe"
  IfErrors update_failed
  SetOutPath "$TEMP"
  Delete "$updateBackup"
  Rename "$INSTDIR" "$updateBackup"
  IfErrors update_failed
  Rename "$updateStage" "$INSTDIR"
  IfErrors restore_backup

  SetOutPath "$INSTDIR"
  Exec '"$INSTDIR\${APP_EXE}"'
  IfErrors restore_after_launch_failure
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "DisplayVersion" "${APP_VERSION}"
  SetOutPath "$TEMP"
  RMDir /r "$updateBackup"
  Return

restore_after_launch_failure:
  SetOutPath "$TEMP"
  ClearErrors
  Rename "$INSTDIR" "$updateStage"
  IfErrors restore_failed
restore_backup:
  ClearErrors
  Rename "$updateBackup" "$INSTDIR"
  IfErrors restore_failed
  StrCpy $updateBackup ""
  Goto update_failed

restore_failed:
  SetErrorLevel 1
  MessageBox MB_OK|MB_ICONSTOP "$(UpdateRestoreFailed)"
  Return

update_failed:
  SetOutPath "$TEMP"
  StrCmp $updateStage "" +3
    Delete "$updateStage"
    RMDir /r "$updateStage"
  StrCmp $updateBackup "" +2
    Delete "$updateBackup"
  SetErrorLevel 1
  ClearErrors
  SetOutPath "$INSTDIR"
  Exec '"$INSTDIR\${APP_EXE}"'
  IfErrors 0 +2
    MessageBox MB_OK|MB_ICONSTOP "$(UpdateRestartFailed)"
FunctionEnd

Section "Install" SEC_INSTALL
  StrCmp $updatePid "" normal_install
  Call InstallLumenUpdate
  Goto install_done

normal_install:
  SetOutPath "$INSTDIR"
  Call ExtractApplication

  ; Remove only obsolete artifacts from the previous product name.
  Delete "$INSTDIR\MVPImageViewer.exe"
  Delete "$SMPROGRAMS\MVP Image Viewer\MVP Image Viewer.lnk"
  Delete "$SMPROGRAMS\MVP Image Viewer\Uninstall MVP Image Viewer.lnk"
  RMDir "$SMPROGRAMS\MVP Image Viewer"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Lumen"
  CreateShortcut "$SMPROGRAMS\Lumen\Lumen.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$SMPROGRAMS\Lumen\Uninstall Lumen.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE},0"
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${APP_REGISTRY_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${APP_REGISTRY_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_REGISTRY_KEY}" "NoRepair" 1
install_done:
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\Lumen\Lumen.lnk"
  Delete "$SMPROGRAMS\Lumen\Uninstall Lumen.lnk"
  RMDir "$SMPROGRAMS\Lumen"
  DeleteRegKey HKCU "${APP_REGISTRY_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
