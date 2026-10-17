cmake_minimum_required(VERSION 3.25)
project(RifeLove VERSION 0.1.0 LANGUAGES C CXX)

option(RIFELOVE_BUILD_TESTS "Build automated tests" ON)
option(RIFELOVE_BUILD_DESKTOP "Build the Qt Quick desktop shell when Qt is available" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
  URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3)
add_library(rifelove_sqlite STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(rifelove_sqlite PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(rifelove_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1)

if(RIFELOVE_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

add_library(rifelove_core
  src/common/rational.cpp
  src/timeline/timeline.cpp
  src/database/task_store.cpp)
target_include_directories(rifelove_core PUBLIC src)
target_link_libraries(rifelove_core PUBLIC rifelove_sqlite)
if(MSVC)
  target_compile_options(rifelove_core PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(rifelove_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(rifelove-cli apps/cli/main.cpp)
target_link_libraries(rifelove-cli PRIVATE rifelove_core)

if(RIFELOVE_BUILD_DESKTOP)
  find_package(Qt6 QUIET COMPONENTS Quick QuickControls2)
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_add_executable(rifelove-desktop apps/desktop/main.cpp)
    qt_add_qml_module(rifelove-desktop URI RifeLove VERSION 1.0 QML_FILES ui/Main.qml)
    target_link_libraries(rifelove-desktop PRIVATE Qt6::Quick Qt6::QuickControls2 rifelove_core)
  else()
    message(STATUS "Qt 6 Quick was not found; desktop shell is deferred, core and CLI remain available.")
  endif()
endif()

if(RIFELOVE_BUILD_TESTS)
  add_executable(rifelove-tests tests/unit/rational_test.cpp tests/unit/timeline_test.cpp tests/unit/task_store_test.cpp)
  target_link_libraries(rifelove-tests PRIVATE rifelove_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rifelove-tests)
endif()
