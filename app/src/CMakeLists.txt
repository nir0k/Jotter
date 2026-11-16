# Library format, index, search and other logic without widgets.
qt_add_library(jotter_core STATIC
    core/Attachments.cpp core/Attachments.h
    core/Credentials.cpp core/Credentials.h
    core/Directory.cpp core/Directory.h
    core/Error.cpp core/Error.h
    core/Export.cpp core/Export.h
    core/Files.cpp core/Files.h
    core/FileGuard.cpp core/FileGuard.h
    core/Html.cpp core/Html.h
    core/Index.cpp core/Index.h
    core/Library.cpp core/Library.h
    core/LibrarySession.cpp core/LibrarySession.h
    core/LibraryWatcher.cpp core/LibraryWatcher.h
    core/Merge.cpp core/Merge.h
    core/Note.cpp core/Note.h
    core/NoteLinks.cpp core/NoteLinks.h
    core/Numbering.cpp core/Numbering.h
    core/PlainText.cpp core/PlainText.h
    core/Rules.cpp core/Rules.h
    core/RemoteStorage.h
    core/Search.cpp core/Search.h
    core/SftpStorage.cpp core/SftpStorage.h
    core/Stemmer.cpp core/Stemmer.h
    core/SyncEngine.cpp core/SyncEngine.h
    core/SyncScheduler.cpp core/SyncScheduler.h
    core/SyncState.cpp core/SyncState.h
    core/TagColors.h
    core/TableOfContents.cpp core/TableOfContents.h
    core/Tags.cpp core/Tags.h
)
target_include_directories(jotter_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(jotter_core PUBLIC Qt6::Core Qt6::Concurrent Qt6::Sql
    PRIVATE SQLite3::SQLite3 Stemmer::Stemmer ssh)

# Interface look: palettes, roles, style sheet, icons.
qt_add_library(jotter_ui STATIC
    ui/Icons.cpp ui/Icons.h
    ui/Palette.cpp ui/Palette.h
    ui/Popups.cpp ui/Popups.h
    ui/StyleSheet.cpp ui/StyleSheet.h
    ui/TagColor.cpp ui/TagColor.h
    ui/Theme.cpp ui/Theme.h
    ui/Widgets.cpp ui/Widgets.h
)
target_include_directories(jotter_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(jotter_ui PUBLIC Qt6::Widgets PRIVATE Qt6::Svg)

file(GLOB JOTTER_ICON_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/icons/*.svg")
qt_add_resources(jotter_ui "icons"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES ${JOTTER_ICON_FILES})

# The rich text editor and its HTML codec.
qt_add_library(jotter_editor STATIC
    editor/Commands.cpp editor/Commands.h
    editor/Decorations.cpp editor/Decorations.h
    editor/Export.cpp editor/Export.h
    editor/FindBar.cpp editor/FindBar.h
    editor/Formats.h
    editor/NoteAction.h
    editor/HtmlCodec.cpp editor/HtmlCodec.h
    editor/Markdown.cpp editor/Markdown.h editor/MarkdownWriter.cpp
    editor/InfoPanel.cpp editor/InfoPanel.h
    editor/LanguagePopup.cpp editor/LanguagePopup.h
    editor/NoteEditor.cpp editor/NoteEditor.h
    editor/NotePicker.cpp editor/NotePicker.h
    editor/Objects.cpp editor/Objects.h
    editor/Presenter.cpp editor/Presenter.h
    editor/Syntax.cpp editor/Syntax.h
    editor/TagStrip.cpp editor/TagStrip.h
    editor/TextEdit.cpp editor/TextEdit.h
)
target_link_libraries(jotter_editor PUBLIC jotter_core jotter_ui
    PRIVATE KF6::SyntaxHighlighting KF6::Archive PkgConfig::MD4C)

# The application: windows, dialogs, command line.
qt_add_library(jotter_app STATIC
    app/Application.cpp app/Application.h
    app/CaptureWindow.cpp app/CaptureWindow.h
    app/CollectionCombo.cpp app/CollectionCombo.h
    app/DownloadDialog.cpp app/DownloadDialog.h
    app/ExportDialog.cpp app/ExportDialog.h
    app/FirstStartDialog.cpp app/FirstStartDialog.h
    app/Locator.cpp app/Locator.h
    app/MainWindow.cpp app/MainWindow.h
    app/MembershipDialog.cpp app/MembershipDialog.h
    app/Mime.h
    app/NoteList.cpp app/NoteList.h
    app/RemoteFolderDialog.cpp app/RemoteFolderDialog.h
    app/RuleEditorDialog.cpp app/RuleEditorDialog.h
    app/SearchPanel.cpp app/SearchPanel.h
    app/Settings.cpp app/Settings.h
    app/SettingsDialog.cpp app/SettingsDialog.h
    app/Sidebar.cpp app/Sidebar.h
    app/SingleInstance.cpp app/SingleInstance.h
    app/SyncConnectionForm.cpp app/SyncConnectionForm.h
    app/SyncPage.cpp app/SyncPage.h
    app/SyncStatus.cpp app/SyncStatus.h
    app/View.cpp app/View.h
)
target_link_libraries(jotter_app PUBLIC jotter_core jotter_editor jotter_ui Qt6::Network
    PRIVATE Qt6::Positioning)
qt_add_resources(jotter_app "app-resources"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/app/io.github.nir0k.Jotter-256.png"
        "${PROJECT_SOURCE_DIR}/resources/app/io.github.nir0k.Jotter-symbolic.svg"
        "${PROJECT_SOURCE_DIR}/resources/licenses/Bootstrap-Icons-MIT.txt")

qt_add_executable(jotter app/main.cpp)
target_link_libraries(jotter PRIVATE jotter_app)

install(TARGETS jotter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration: the desktop entry, AppStream metadata stamped with the
# version, the icon and the licences of bundled third-party glyphs.
string(TIMESTAMP JOTTER_RELEASE_DATE "%Y-%m-%d" UTC)
configure_file("${PROJECT_SOURCE_DIR}/packaging/io.github.nir0k.Jotter.metainfo.xml.in"
    "${PROJECT_BINARY_DIR}/io.github.nir0k.Jotter.metainfo.xml" @ONLY)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/io.github.nir0k.Jotter.desktop"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES "${PROJECT_BINARY_DIR}/io.github.nir0k.Jotter.metainfo.xml"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The application icon is a raster drawing, installed in every size of the hicolor theme.
foreach(size 16 24 32 48 64 128 256 512)
    install(FILES "${PROJECT_SOURCE_DIR}/resources/app/io.github.nir0k.Jotter-${size}.png"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
        RENAME io.github.nir0k.Jotter.png)
endforeach()
# The monochrome tray icon, also offered to desktops that look for a symbolic application icon.
install(FILES "${PROJECT_SOURCE_DIR}/resources/app/io.github.nir0k.Jotter-symbolic.svg"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps)
install(FILES "${PROJECT_SOURCE_DIR}/resources/licenses/Bootstrap-Icons-MIT.txt"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/jotter)
