Unicode True

!include "MUI2.nsh"

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

Section "Install" SEC_INSTALL
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"

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
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\Lumen\Lumen.lnk"
  Delete "$SMPROGRAMS\Lumen\Uninstall Lumen.lnk"
  RMDir "$SMPROGRAMS\Lumen"
  DeleteRegKey HKCU "${APP_REGISTRY_KEY}"
  RMDir /r "$INSTDIR"
SectionEnd
