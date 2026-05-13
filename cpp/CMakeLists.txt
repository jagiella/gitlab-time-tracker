cmake_minimum_required(VERSION 3.20)
project(gtt_cpp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG master
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(CLI11 yaml-cpp nlohmann_json)

find_package(CURL REQUIRED)

add_executable(gtt
  vendor/hashids_c/hashids.c
  src/main.cpp
  src/config.cpp
  src/gitlab_client.cpp
  src/throttle.cpp
  src/hashids_encode.cpp
  src/time_format.cpp
  src/frame.cpp
  src/tasks.cpp
  src/report.cpp
  src/output.cpp
  src/cmd_log.cpp
  src/commands.cpp
  src/tty_interactive.cpp
)

target_include_directories(gtt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gtt PRIVATE
  CLI11::CLI11
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  CURL::libcurl
)

if(MSVC)
  target_compile_definitions(gtt PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
