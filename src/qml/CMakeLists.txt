set_source_files_properties(Mvp/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_policy(SET QTP0004 NEW)

qt_add_executable(LumenQml
    main.cpp
    window_chrome_controller.h
    window_chrome_controller.cpp
    app_settings.h
    app_settings.cpp
    browse_controller.h
    browse_controller.cpp
    folder_tree_branch_model.h
    browse_workspace_controller.h
    browse_workspace_controller.cpp
    thumbnail_image_provider.h
    accounted_texture_factory.h
    thumbnail_image_provider.cpp
    qml_image_canvas.h
    qml_image_canvas.cpp
    panorama_canvas.h
    panorama_canvas.cpp
    compare_controller.h
    compare_controller.cpp
    image_properties_controller.h
    image_properties_controller.cpp
    full_screen_controller.h
    full_screen_controller.cpp
    raw_parameters_controller.h
    raw_parameters_controller.cpp
)
if(WIN32)
    target_link_libraries(LumenQml PRIVATE comctl32)
endif()

qt_add_qml_module(LumenQml
    URI MvpViewQml
    VERSION 1.0
    QML_FILES
        Main.qml
        Mvp/Theme.qml
        Mvp/ShortcutContext.qml
        Mvp/SettingsButton.qml
        Mvp/SettingsToggle.qml
        Mvp/SettingsRow.qml
        Mvp/SettingsHeading.qml
        Mvp/SettingsNotice.qml
        Mvp/SettingsComboBox.qml
        Mvp/SettingsCard.qml
        Mvp/DiagnosticsPage.qml
        Mvp/AppIconButton.qml
        Mvp/AppTextInputDialog.qml
        Mvp/AppConfirmDialog.qml
        Mvp/UpdateDialog.qml
        Mvp/AppMenu.qml
        Mvp/AppMenuItem.qml
        Mvp/AppMenuSeparator.qml
        Mvp/BrowserPane.qml
        Mvp/PaneHeader.qml
        Mvp/CompareLumaHistogram.qml
        Mvp/FolderNavigator.qml
        Mvp/InspectorHeader.qml
        Mvp/InspectorTabs.qml
        Mvp/ImagePropertiesContent.qml
        Mvp/ImagePropertiesPanel.qml
        Mvp/ImagePropertiesDialog.qml
        Mvp/ImageOperationsMenu.qml
        Mvp/ImageResizeDialog.qml
        Mvp/PropertiesHistogram.qml
        Mvp/PropertyFieldList.qml
        Mvp/RawNumberField.qml
        Mvp/RawParameterComboBox.qml
        Mvp/RawParameterRow.qml
        Mvp/RawParametersContent.qml
        Mvp/RawParametersPanel.qml
        Mvp/RawParametersDialog.qml
        Mvp/ProductionCompareCanvas.qml
        Mvp/ProductionFullScreenCanvas.qml
        Mvp/ProductionPanoramaCanvas.qml
        Mvp/TopToolbar.ui.qml
        Mvp/WindowCaptionButtons.qml
        Mvp/ApplicationNotices.qml
        Mvp/ThumbnailViewport.qml
        Mvp/NavigationThumbnail.qml
        Mvp/FileToolTip.qml
        Mvp/ThumbnailTile.qml
        Mvp/PanoramaBadge.qml
        Pages/BrowsePage.qml
        Pages/ComparePage.qml
        Pages/FullScreenPage.qml
)

qt_add_translations(LumenQml
    IMMEDIATE_CALL
    SOURCE_TARGETS LumenQml mvpview_diagnostics mvpview_io
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/i18n/lumen_zh_CN.ts"
    RESOURCE_PREFIX "/i18n"
)

set(_mvpview_qml_icons
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/caption-minimize.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/caption-maximize.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/caption-restore.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/caption-close.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/caption-close-white.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/back.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/forward.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/up.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/down.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/zoom-in.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/zoom-out.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/panorama.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/chevron-down.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/folder.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/folder-open.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/folder-plus.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/folder-pane-plus.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/windows-folder.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/windows-folder-open.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-home.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-desktop.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-documents.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-downloads.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-pictures.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-drive.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/nav-network.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/windows-drive.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/macos-folder.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/macos-folder-open.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/close.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/search.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/sort.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/compare.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/cover.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/grid.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/list.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/gallery.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/thumbnail-preview.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/info.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/actual-size.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/smooth-display.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/side-by-side.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/split-vertical.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/fit.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/fullscreen.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/exif.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/histogram.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/pixel-probe.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/screenshot.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/rotate-cw-square.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/rotate-ccw-square.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/image-resize.svg
    ${PROJECT_SOURCE_DIR}/assets/icons/ui/settings.svg
)
qt_add_resources(LumenQml "mvpview_qml_icons"
    PREFIX "/icons/ui"
    BASE "${PROJECT_SOURCE_DIR}/assets/icons/ui"
    FILES ${_mvpview_qml_icons}
)

set(_mvpview_qml_dark_icons)
foreach(_icon IN LISTS _mvpview_qml_icons)
    get_filename_component(_icon_name "${_icon}" NAME)
    list(APPEND _mvpview_qml_dark_icons
        "${PROJECT_SOURCE_DIR}/assets/icons/ui-dark/${_icon_name}")
endforeach()
qt_add_resources(LumenQml "mvpview_qml_dark_icons"
    PREFIX "/icons/ui-dark"
    BASE "${PROJECT_SOURCE_DIR}/assets/icons/ui-dark"
    FILES ${_mvpview_qml_dark_icons}
)

set(_mvpview_brand_assets
    ${PROJECT_SOURCE_DIR}/assets/brand/app_icon.png
)
qt_add_resources(LumenQml "mvpview_brand_assets"
    PREFIX "/brand"
    BASE "${PROJECT_SOURCE_DIR}/assets/brand"
    FILES ${_mvpview_brand_assets}
)

target_include_directories(LumenQml PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(LumenQml PRIVATE
    MVPVIEW_PROJECT_VERSION="${PROJECT_VERSION}"
    MVPVIEW_GITHUB_REPOSITORY="${MVPVIEW_GITHUB_REPOSITORY}"
)
target_link_libraries(LumenQml PRIVATE
    mvpview_browser_backend
    mvpview_render_backend
    mvpview_io
    # QmlImageCanvas implements the QQuickRhiItem renderer directly and uses
    # QRhi resource types.  Do not rely on another Qt target to leak the
    # versioned private QtGui include directory: framework builds on macOS may
    # do that incidentally, while MSYS2/MinGW correctly keeps it private.
    Qt6::GuiPrivate
    Qt6::Network
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickLayouts
    Qt6::QuickTemplates2
    Qt6::Svg
)
mvpview_enable_warnings(LumenQml)

if(BUILD_TESTING AND MVPVIEW_ENABLE_UI_TESTS)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Test)
    target_sources(LumenQml PRIVATE fullscreen_menu_test.cpp fullscreen_menu_test.h)
    target_link_libraries(LumenQml PRIVATE Qt6::Test OpenEXR::OpenEXR)
    if(APPLE)
        target_link_libraries(LumenQml PRIVATE "-framework CoreGraphics")
    elseif(WIN32)
        target_link_libraries(LumenQml PRIVATE user32)
    endif()
    target_compile_definitions(LumenQml PRIVATE MVPVIEW_ENABLE_UI_TESTS)
    foreach(mode IN ITEMS windowed maximized)
        add_test(NAME fullscreen_menu_${mode} COMMAND LumenQml)
        set_tests_properties(fullscreen_menu_${mode} PROPERTIES
            ENVIRONMENT "LUMEN_FULLSCREEN_TEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/test-fullscreen-${mode};LUMEN_FULLSCREEN_TEST_MODE=${mode}"
            RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endif()

# The QML executable is the sole production application artifact.
set_target_properties(LumenQml PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    OUTPUT_NAME "Lumen"
    MACOSX_BUNDLE_BUNDLE_NAME "Lumen"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.mvpview.viewer"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

if(APPLE)
    set(MVPVIEW_SPARKLE_FRAMEWORK "" CACHE PATH "Path to Sparkle.framework for in-app macOS updates")
    if(MVPVIEW_SPARKLE_FRAMEWORK)
        enable_language(OBJCXX)
        set_target_properties(LumenQml PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
        target_sources(LumenQml PRIVATE mac_update_driver.mm mac_update_driver.h)
        set_source_files_properties(mac_update_driver.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(LumenQml PRIVATE MVPVIEW_HAS_SPARKLE)
        target_link_libraries(LumenQml PRIVATE "${MVPVIEW_SPARKLE_FRAMEWORK}" "-framework Foundation")
        set_property(TARGET LumenQml APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
        add_custom_command(TARGET LumenQml POST_BUILD
            COMMAND /usr/bin/ditto "${MVPVIEW_SPARKLE_FRAMEWORK}"
                "$<TARGET_BUNDLE_DIR:LumenQml>/Contents/Frameworks/Sparkle.framework"
            VERBATIM)
    endif()
endif()

set(_mvpview_qml_macos_icon "${PROJECT_SOURCE_DIR}/assets/icons/macos/Lumen.icns")
if(APPLE AND EXISTS "${_mvpview_qml_macos_icon}")
    set_source_files_properties("${_mvpview_qml_macos_icon}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(LumenQml PRIVATE "${_mvpview_qml_macos_icon}")
    set_target_properties(LumenQml PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "Lumen.icns")
endif()

# The QML application is the Windows production executable as well, so embed the
# same application icon used by the macOS bundle.
set(_mvpview_qml_windows_icon "${PROJECT_SOURCE_DIR}/assets/icons/windows/Lumen.ico")
if(WIN32 AND EXISTS "${_mvpview_qml_windows_icon}")
    qt_add_resources(LumenQml "mvpview_windows_icon"
        PREFIX "/brand"
        BASE "${PROJECT_SOURCE_DIR}/assets/icons/windows"
        FILES "${_mvpview_qml_windows_icon}"
    )
    set(_mvpview_qml_windows_icon_rc "${CMAKE_CURRENT_BINARY_DIR}/LumenQmlIcon.rc")
    string(CONCAT _mvpview_qml_windows_icon_rc_content
        "IDI_ICON1 ICON \"${_mvpview_qml_windows_icon}\"\n"
        "1 VERSIONINFO\n"
        "FILEVERSION ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0\n"
        "PRODUCTVERSION ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0\n"
        "FILEFLAGSMASK 0x3fL\n"
        "FILEOS 0x40004L\n"
        "FILETYPE 0x1L\n"
        "BEGIN\n"
        "  BLOCK \"StringFileInfo\"\n"
        "  BEGIN\n"
        "    BLOCK \"040904b0\"\n"
        "    BEGIN\n"
        "      VALUE \"FileDescription\", \"Lumen\"\n"
        "      VALUE \"FileVersion\", \"${PROJECT_VERSION}\"\n"
        "      VALUE \"InternalName\", \"Lumen\"\n"
        "      VALUE \"OriginalFilename\", \"Lumen.exe\"\n"
        "      VALUE \"ProductName\", \"Lumen\"\n"
        "      VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"\n"
        "    END\n"
        "  END\n"
        "  BLOCK \"VarFileInfo\"\n"
        "  BEGIN\n"
        "    VALUE \"Translation\", 0x0409, 1200\n"
        "  END\n"
        "END\n")
    file(GENERATE OUTPUT "${_mvpview_qml_windows_icon_rc}"
        CONTENT "${_mvpview_qml_windows_icon_rc_content}")
    add_library(LumenQml_windows_icon OBJECT "${_mvpview_qml_windows_icon_rc}")
    target_sources(LumenQml PRIVATE
        $<TARGET_OBJECTS:LumenQml_windows_icon>
    )
endif()

unset(_mvpview_qml_icons)
unset(_mvpview_brand_assets)
unset(_mvpview_qml_macos_icon)
unset(_mvpview_qml_windows_icon)
unset(_mvpview_qml_windows_icon_rc)
unset(_mvpview_qml_windows_icon_rc_content)

if(WIN32)
    add_dependencies(LumenQml mvpview_crash_handler)
    add_custom_command(TARGET LumenQml POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mvpview_crash_handler>
            $<TARGET_FILE_DIR:LumenQml>)
endif()
if(APPLE AND MVPVIEW_HAS_CRASHPAD)
    add_custom_command(TARGET LumenQml POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LumenQml>/../Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVPVIEW_CRASHPAD_HANDLER}"
            "$<TARGET_FILE_DIR:LumenQml>/../Helpers/crashpad_handler")
endif()
