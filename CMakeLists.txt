cmake_minimum_required(VERSION 3.25)

project(Zevvoryn VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_RC "Enable Windows resource file (.rc)" OFF)
option(ENABLE_PCH "Use precompiled headers" ON)
option(ENABLE_LTO "Use link-time optimization in Release" OFF)

if(ENABLE_RC AND WIN32)
    enable_language(RC)
endif()

# ============================================================
# Компилятор / Compiler
# ============================================================

if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /utf-8
        /MP
        # CRASHTRACE_V1: server.cpp упёрся в лимит числа секций obj-файла (C1128).
        # /bigobj поднимает потолок с 65 535 до 4 млрд секций. На код и скорость
        # не влияет вообще — это только формат промежуточного .obj.
        /bigobj
    )
else()
    add_compile_options(
        -Wall
        -Wextra
    )
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================
# LTO / IPO (Release) - off by default: /GL + /LTCG makes a cold
# build much slower and gives almost nothing for this project.
# ============================================================

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endif()

# ============================================================
# FetchContent
# ============================================================

include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(json)

# ============================================================
# OpenSSL
# ============================================================

find_package(OpenSSL QUIET)

# ============================================================
# Исходники / Sources
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
    src/console/*.cpp
)

set(ZEVVORYN_SOURCES
    src/main.cpp
    ${ALL_SOURCES}
)

if(ENABLE_RC AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zevvoryn.rc")
    list(APPEND ZEVVORYN_SOURCES zevvoryn.rc)
    message(STATUS "Ресурсы Windows (zevvoryn.rc) включены")
else()
    message(STATUS "Сборка БЕЗ ресурсов .rc и иконки")
endif()

# ============================================================
# Исполняемый файл / Executable
# ============================================================

add_executable(zevvoryn ${ZEVVORYN_SOURCES})

target_include_directories(
    zevvoryn
    PRIVATE
    src
)

# ============================================================
# Windows-определения / Windows defines
#
# WIN32_LEAN_AND_MEAN обязателен: без него <windows.h> тянет старый
# <winsock.h> (Winsock 1.1), а потом <winsock2.h> переопределяет sockaddr,
# fd_set, timeval, WSAData, accept/bind/send/... — это и давало C2011/C2375.
# ============================================================

if(WIN32)
    target_compile_definitions(
        zevvoryn
        PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
endif()

# ============================================================
# Precompiled Headers
#
# Порядок важен: winsock2.h и ws2tcpip.h СТРОГО ДО windows.h.
# ============================================================

if(ENABLE_PCH)
    target_precompile_headers(zevvoryn PRIVATE
        <algorithm>
        <array>
        <chrono>
        <filesystem>
        <fstream>
        <functional>
        <iostream>
        <map>
        <memory>
        <mutex>
        <optional>
        <queue>
        <set>
        <span>
        <sstream>
        <string>
        <string_view>
        <thread>
        <unordered_map>
        <unordered_set>
        <utility>
        <variant>
        <vector>

        <nlohmann/json.hpp>

        $<$<PLATFORM_ID:Windows>:<winsock2.h>>
        $<$<PLATFORM_ID:Windows>:<ws2tcpip.h>>
        $<$<PLATFORM_ID:Windows>:<windows.h>>
    )
endif()

# ============================================================
# Линковка / Linking
# ============================================================

target_link_libraries(
    zevvoryn
    PRIVATE
    nlohmann_json::nlohmann_json
)

if(OpenSSL_FOUND)
    target_link_libraries(
        zevvoryn
        PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
    )

    target_compile_definitions(
        zevvoryn
        PRIVATE
        HAS_OPENSSL=1
    )

    message(STATUS "OpenSSL найден - аутентификация включена")
else()
    message(STATUS "OpenSSL не найден - offline-режим")
endif()

if(WIN32)
    target_link_libraries(
        zevvoryn
        PRIVATE
        ws2_32
        crypt32
        winmm
        dbghelp # CRASHDEEP_V1: стек с символами и минидамп в отчёте о краше
    )
endif()

# ============================================================
# NOCONHOST_V1: в GUI-режиме zevvoryn больше не должен на секунду мелькать чёрным окном
# conhost при старте. /SUBSYSTEM:WINDOWS говорит Windows вообще не создавать консоль
# автоматически; /ENTRY:mainCRTStartup говорит всё равно входить через обычный int main()
# (без этого MSVC ожидал бы WinMain и сборка сломалась бы с LNK2019). В classic-режиме
# main.cpp теперь сам зовёт AllocConsole() (см. NOCONHOST_V1 в main.cpp).
# ============================================================

if(WIN32 AND MSVC)
    target_link_options(
        zevvoryn
        PRIVATE
        /SUBSYSTEM:WINDOWS
        /ENTRY:mainCRTStartup
    )
endif()

# ============================================================
# CRASHDEEP_V1: PDB для Release ГЕНЕРИРУЕТСЯ, НО В РЕЛИЗ НЕ КЛАДЁТСЯ.
# zevvoryn.pdb остаётся в build-ninja у разработчика и архивируется по версиям.
# Игроку он НЕ нужен: отчёт о краше сам по себе читаем — в нём есть журнал
# последних действий (CRASHTRACE_V1), отпечаток сборки и RVA-адреса; по этой
# тройке разработчик восстанавливает имена функций СВОИМ pdb у себя.
# /Zi не замедляет код (оптимизации остаются), а /OPT:REF /OPT:ICF возвращают
# обрезку мёртвого кода, которую /DEBUG иначе отключает. Сам exe от этого
# не толстеет и не медленнее.
# ============================================================

if(WIN32 AND MSVC)
    target_compile_options(zevvoryn PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(
        zevvoryn
        PRIVATE
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()
