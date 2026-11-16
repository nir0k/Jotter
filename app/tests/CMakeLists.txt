set(JOTTER_TS_FILES
    "${PROJECT_SOURCE_DIR}/i18n/jotter_en.ts"
    "${PROJECT_SOURCE_DIR}/i18n/jotter_ru.ts")

add_test(NAME translations_complete
    COMMAND ${CMAKE_COMMAND} "-DTS_FILES=${JOTTER_TS_FILES}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckTranslations.cmake")

find_program(JOTTER_LCHECK NAMES lcheck lcheck-qt6 HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
if(JOTTER_LCHECK)
    add_test(NAME translations_consistent
        COMMAND ${JOTTER_LCHECK} --check-finished "${PROJECT_SOURCE_DIR}/i18n/jotter_ru.ts")
endif()

# Adds a Qt Test case built from tst_<name>.cpp and linked to the given libraries.
function(jotter_add_test name)
    qt_add_executable(tst_${name} tst_${name}.cpp)
    target_link_libraries(tst_${name} PRIVATE Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND tst_${name})
endfunction()

jotter_add_test(palette jotter_ui)
jotter_add_test(library jotter_core)
jotter_add_test(index jotter_core)
jotter_add_test(search jotter_core)
jotter_add_test(rules jotter_core)
jotter_add_test(links jotter_core)
jotter_add_test(tags jotter_core)
jotter_add_test(numbering jotter_core)
jotter_add_test(attachments jotter_core)
jotter_add_test(session jotter_core)
jotter_add_test(merge jotter_core)
jotter_add_test(sync jotter_core)
jotter_add_test(editor jotter_editor)
jotter_add_test(fixtures jotter_editor)
# Library file access refuses `..` in paths, so the fixtures are named by a normalised path.
cmake_path(SET JOTTER_FIXTURES_DIR NORMALIZE "${PROJECT_SOURCE_DIR}/../tests/fixtures")
target_compile_definitions(tst_fixtures PRIVATE JOTTER_FIXTURES_DIR="${JOTTER_FIXTURES_DIR}")
jotter_add_test(noteeditor jotter_editor)
jotter_add_test(export jotter_editor KF6::Archive)
jotter_add_test(mainwindow jotter_app)
