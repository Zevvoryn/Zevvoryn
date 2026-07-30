cmake_minimum_required(VERSION 3.25)

# Убрали RC из языков по умолчанию, чтобы CMake не требовал windres/rc
project(Zevvoryn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Опция для ресурсов/иконки (по умолчанию ВЫКЛЮЧЕНА)
option(ENABLE_RC "Enable Windows resource file (.rc)" OFF)

if(ENABLE_RC AND WIN32)
    enable_language(RC)
endif()

# Включаем все предупреждения (только для MSVC — флаги /W4 и т.д. clang/gcc не понимают)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

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

# Формируем список файлов для сборки
set(ZEVVORYN_SOURCES
    src/main.cpp
    ${ALL_SOURCES}
)

# Добавляем .rc только если явно включен флаг ENABLE_RC=ON
if(ENABLE_RC AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zevvoryn.rc")
    list(APPEND ZEVVORYN_SOURCES zevvoryn.rc)
    message(STATUS "Ресурсы Windows (zevvoryn.rc) включены")
else()
    message(STATUS "Сборка БЕЗ ресурсов .rc и иконки")
endif()

add_executable(zevvoryn ${ZEVVORYN_SOURCES})

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
endif()
