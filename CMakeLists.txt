cmake_minimum_required(VERSION 3.16)
project(nesemu LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Debugging niceties
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(BUILD_SANITIZERS "Enable Address/UB sanitizers in Debug builds" ON)

# Sources
set(SRC
    src/bus.cpp
    src/nes.cpp
    src/cpu.cpp
    src/ppu.cpp
    src/apu.cpp
    src/cartridge.cpp
    src/mapper_nrom.cpp
    src/mapper_mmc1.cpp
    src/mapper_mmc3.cpp
    src/input.cpp
    src/timgui.cpp
    src/main.cpp
)

add_executable(nes ${SRC})
set_target_properties(nes PROPERTIES OUTPUT_NAME "nes")
target_include_directories(nes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Warnings
if(MSVC)
  target_compile_options(nes PRIVATE /W4 /permissive-)
else()
  target_compile_options(nes PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Build types
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(NOT MSVC)
    target_compile_options(nes PRIVATE -O3)
  endif()
else()
  if(NOT MSVC)
    target_compile_options(nes PRIVATE -O0 -g3)
  endif()
endif()

# Sanitizers (Debug only)
if(BUILD_SANITIZERS AND CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo" AND NOT MSVC)
  target_compile_options(nes PRIVATE -fsanitize=address,undefined)
  target_link_options(nes PRIVATE -fsanitize=address,undefined)
endif()

find_package(SDL2 REQUIRED)
# Prefer the CMake package if present
find_package(SDL2_ttf QUIET)

if (SDL2_ttf_FOUND)
  target_link_libraries(nes PRIVATE SDL2::SDL2 SDL2::SDL2main SDL2_ttf::SDL2_ttf)
else()
  # Fallback to pkg-config
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2TTF QUIET SDL2_ttf)
  if (NOT SDL2TTF_FOUND)
    pkg_check_modules(SDL2TTF REQUIRED sdl2_ttf) # some distros use lowercase name
  endif()
  target_include_directories(nes PRIVATE ${SDL2TTF_INCLUDE_DIRS})
  target_link_libraries(nes PRIVATE ${SDL2TTF_LIBRARIES} SDL2::SDL2 SDL2::SDL2main)
endif()

# ---- SDL2 detection ----
# Try modern CMake config package first
set(SDL2_FOUND_BY_CONFIG FALSE)
find_package(SDL2 QUIET CONFIG)
if(SDL2_FOUND AND TARGET SDL2::SDL2)
  set(SDL2_FOUND_BY_CONFIG TRUE)
  target_link_libraries(nes PRIVATE SDL2::SDL2)
elseif(SDL2_FOUND AND TARGET SDL2::SDL2main)
  set(SDL2_FOUND_BY_CONFIG TRUE)
  target_link_libraries(nes PRIVATE SDL2::SDL2main SDL2::SDL2)
endif()

# Fallback to module mode or pkg-config
if(NOT SDL2_FOUND_BY_CONFIG)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND AND SDL2_INCLUDE_DIRS AND SDL2_LIBRARIES)
    target_include_directories(nes PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(nes PRIVATE ${SDL2_LIBRARIES})
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(nes PRIVATE PkgConfig::SDL2)
  endif()
endif()

# Filesystem link workaround for older GCC (<9.1)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(nes PRIVATE stdc++fs)
  endif()
endif()

# Install (optional)
install(TARGETS nes RUNTIME DESTINATION bin)
