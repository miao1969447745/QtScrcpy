find_package(Git REQUIRED)
set(_core_dir "${CMAKE_CURRENT_LIST_DIR}/../QtScrcpy/QtScrcpyCore")
set(_core_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/qtscrcpy-core-plugin.patch")
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_core_patch}"
    WORKING_DIRECTORY "${_core_dir}" RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
if(NOT _already EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${_core_patch}"
        WORKING_DIRECTORY "${_core_dir}" RESULT_VARIABLE _check OUTPUT_QUIET ERROR_VARIABLE _error)
    if(NOT _check EQUAL 0)
        message(FATAL_ERROR "Cannot apply QtScrcpy plugin core patch without overwriting changes: ${_error}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_core_patch}"
        WORKING_DIRECTORY "${_core_dir}" RESULT_VARIABLE _applied)
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "Failed to apply QtScrcpy plugin core patch")
    endif()
endif()
