cmake_minimum_required(VERSION 3.25)
project(Zevvoryn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Включаем все предупреждения
add_compile_options(/W4 /permissive-)

# Статический рантайм MSVC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================
# Внешние зависимости через FetchContent (без vcpkg/conan)
# ============================================================
include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# ============================================================
# OpenSSL (опционально — для онлайн-аутентификации)
# ============================================================
find_package(OpenSSL QUIET)

# ============================================================
# Исходники
# ============================================================
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp
    src/network/*.cpp
    src/protocol/*.cpp
    src/registries/*.cpp
    src/world/*.cpp
    src/entity/*.cpp
    src/inventory/*.cpp
    src/commands/*.cpp
    src/auth/*.cpp
    src/utils/*.cpp
)

add_executable(zevvoryn
    src/main.cpp
    ${ALL_SOURCES}
    icon/app.rc
)

set_target_properties(zevvoryn PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

target_include_directories(zevvoryn PRIVATE src)
target_link_libraries(zevvoryn PRIVATE nlohmann_json::nlohmann_json)

if(OpenSSL_FOUND)
    target_link_libraries(zevvoryn PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(zevvoryn PRIVATE HAS_OPENSSL=1)
    message(STATUS "OpenSSL найден - аутентификация включена")
else()
    message(STATUS "OpenSSL не найден - offline-режим")
endif()

if(WIN32)
    target_link_libraries(zevvoryn PRIVATE ws2_32 crypt32)
    set(CMAKE_RC_FLAGS "/I\"${CMAKE_SOURCE_DIR}/icon\"")
endif()
