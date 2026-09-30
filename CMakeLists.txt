cmake_minimum_required(VERSION 3.15)
# meta
set(DM_VERSION "1.1.0" CACHE STRING "DisplayManager version (major.minor.patch [.build])")
set(MY_DESCRIPTION "An alternative to WindowResizer, handles fullscreen scaling and window sizing." CACHE STRINGS "description")
project(DisplayManager 
  VERSION ${DM_VERSION}
  DESCRIPTION ${MY_DESCRIPTION}
  LANGUAGES C CXX
)
set(PROJECT_DESCRIPTION "Window & fullscreen manager for Soku" CACHE STRINGS "description short ver" FORCE)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "This project can only be build as x86(Win32).")
endif()

# generates version.rc
string(REGEX MATCHALL "[0-9]+" DM_VERSION_PARTS "${DM_VERSION}")
string(TIMESTAMP DM_BUILD_DATE "%Y%m%d")
string(TIMESTAMP DM_BUILD_DATE_SHORT "%m%d")
list(LENGTH DM_VERSION_PARTS _len)
list(APPEND DM_VERSION_PARTS 0 0 0)
list(GET DM_VERSION_PARTS 0 DM_VERSION_MAJOR)
list(GET DM_VERSION_PARTS 1 DM_VERSION_MINOR)
list(GET DM_VERSION_PARTS 2 DM_VERSION_PATCH)
if(_len GREATER 3)
    list(GET DM_VERSION_PARTS 3 DM_VERSION_BUILD)
else()
    set(DM_VERSION_BUILD "${DM_BUILD_DATE_SHORT}")
endif()
configure_file(${CMAKE_SOURCE_DIR}/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)


# target
add_library(DisplayManager SHARED
  src/DisplayManager.cpp
  ${CMAKE_BINARY_DIR}/version.rc
)

target_include_directories(DisplayManager PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(DisplayManager PRIVATE
  _CRT_SECURE_NO_WARNINGS
  WINVER=0x0601
  _WIN32_WINNT=0x0601
)
# generator specified
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(DisplayManager PRIVATE /EHsc /nologo)
  target_compile_options(DisplayManager PRIVATE $<$<CONFIG:Release>:/O2>)
else()
  target_compile_options(DisplayManager PRIVATE -Wall -Wextra)
  target_compile_options(DisplayManager PRIVATE $<$<CONFIG:Release>:-O2>)
endif()

set_target_properties(DisplayManager PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_link_libraries(DisplayManager PRIVATE
  Shlwapi
  user32
)

# install
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/build" CACHE PATH "Install prefix, can be targeted to game modules folder")
# copy dll
install(FILES $<TARGET_FILE:DisplayManager> DESTINATION ${PROJECT_NAME})
# copy assets
install(FILES
    "${CMAKE_SOURCE_DIR}/DisplayManager.ini"
    DESTINATION ${PROJECT_NAME}
)