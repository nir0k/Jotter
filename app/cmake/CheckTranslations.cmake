# Fails when a translation file still has unfinished, obsolete or vanished entries.
# Usage: cmake -DTS_FILES="a.ts;b.ts" -P CheckTranslations.cmake
foreach(ts_file IN LISTS TS_FILES)
    file(READ "${ts_file}" content)
    foreach(state unfinished obsolete vanished)
        string(REGEX MATCHALL "type=\"${state}\"" matches "${content}")
        list(LENGTH matches count)
        if(count GREATER 0)
            message(SEND_ERROR "${ts_file}: ${count} ${state} entries")
        endif()
    endforeach()
endforeach()
