cmake_minimum_required(VERSION 3.19 FATAL_ERROR)
project(all)

option(QSC_BUILD_UI_TESTS "Build non-device GUI state tests" OFF)
if(QSC_BUILD_UI_TESTS)
    enable_testing()
endif()

include(cmake/ApplyCorePluginPatch.cmake)
add_subdirectory(QtScrcpy)
