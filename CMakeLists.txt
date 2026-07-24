cmake_minimum_required(VERSION 3.23)
project(WindowsLaunchpad VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "Windows Launchpad can only be built on Windows.")
endif()

if(MSVC)
  # Keep the release executable self-contained. The installer must work on a
  # clean Windows account without first installing the Visual C++ runtime.
  set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_language(RC)

add_executable(Launchpad WIN32
  src/launchpad_layout.cpp
  src/launchpad_layout.h
  src/launchpad_model.cpp
  src/launchpad_model.h
  src/main.cpp
  src/resource.h
  src/resources.rc
)

target_compile_definitions(Launchpad PRIVATE
  NOMINMAX
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00
)

target_link_libraries(Launchpad PRIVATE
  d2d1
  dwrite
  dwmapi
  gdi32
  ole32
  propsys
  shell32
  user32
  winmm
  windowscodecs
)

if(MSVC)
  target_compile_options(Launchpad PRIVATE /W4 /permissive- /utf-8)
  target_link_options(Launchpad PRIVATE /MANIFEST:NO)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(LaunchpadTests
    src/launchpad_layout.cpp
    src/launchpad_layout.h
    src/launchpad_model.cpp
    src/launchpad_model.h
    tests/model_tests.cpp
  )
  target_include_directories(LaunchpadTests PRIVATE src)
  target_compile_definitions(LaunchpadTests PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
  )
  if(MSVC)
    target_compile_options(LaunchpadTests PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME LaunchpadModelTests COMMAND LaunchpadTests)
endif()

install(TARGETS Launchpad RUNTIME DESTINATION .)
install(DIRECTORY Applications DESTINATION .)
install(FILES
  scripts/install-launchpad.cmd
  scripts/install-launchpad.ps1
  scripts/uninstall-launchpad.cmd
  scripts/uninstall-launchpad.ps1
  DESTINATION .
)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME
  "WindowsLaunchpad-${PROJECT_VERSION}-win64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Launchpad)
