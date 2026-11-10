cmake_minimum_required(VERSION 3.25)

project(TeslaDownloadManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TDM_BUILD_TESTS "Build unit tests" ON)
option(TDM_BUILD_UI "Build the Qt UI" ON)

# Common compile settings for every TDM target.
add_library(tdm_options INTERFACE)
target_compile_definitions(tdm_options INTERFACE
    _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(tdm_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
    target_compile_options(tdm_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

configure_file(src/core/version.hpp.in ${CMAKE_BINARY_DIR}/generated/core/version.hpp @ONLY)

find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(src)

# ---------------------------------------------------------------------------
# Install tree (input for the Windows installer):
#   <prefix>/bin/TeslaDM.exe, tdm-cli.exe, Qt + vcpkg + MSVC runtime DLLs, Qt plugins
#   <prefix>/THIRD_PARTY_NOTICES.txt, <prefix>/licenses/*.txt, <prefix>/README.md
# ---------------------------------------------------------------------------
install(TARGETS tdm_cli RUNTIME DESTINATION bin)
# Runtime DLLs the executables import. $<TARGET_RUNTIME_DLLS> misses OpenSSL:
# FindOpenSSL's imported targets carry no DLL location, so the vcpkg OpenSSL
# DLLs are listed explicitly. System DLLs (Windows ICU) are filtered out.
install(FILES $<FILTER:$<TARGET_RUNTIME_DLLS:tdm_cli>,EXCLUDE,[Ii][Cc][Uu]> DESTINATION bin)
file(GLOB TDM_OPENSSL_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libssl-*.dll"
                           "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libcrypto-*.dll")
if(NOT TDM_OPENSSL_DLLS)
    message(FATAL_ERROR "OpenSSL DLLs not found in the vcpkg tree; the install would not run")
endif()
install(FILES ${TDM_OPENSSL_DLLS} DESTINATION bin)
if(TDM_BUILD_UI)
    # TeslaDM.exe, its Qt DLLs and plugins: see src/ui/CMakeLists.txt
    # MSVC runtime next to the executables (the UCRT ships with Windows 10/11).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)

    set(_vcpkg_share "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    install(FILES installer/THIRD_PARTY_NOTICES.txt README.md DESTINATION .)
    install(FILES "${_vcpkg_share}/asio/copyright" DESTINATION licenses RENAME asio.txt)
    install(FILES "${_vcpkg_share}/nlohmann-json/copyright" DESTINATION licenses RENAME nlohmann-json.txt)
    install(FILES "${_vcpkg_share}/openssl/copyright" DESTINATION licenses RENAME openssl.txt)
    install(FILES "${_vcpkg_share}/sqlite3/copyright" DESTINATION licenses RENAME sqlite3.txt)
    install(DIRECTORY installer/licenses/ DESTINATION licenses OPTIONAL)  # LGPL-3.0 / GPL-3.0 texts for Qt

    configure_file(installer/tesla-dm.iss.in "${CMAKE_BINARY_DIR}/tesla-dm.iss" @ONLY)
endif()

if(TDM_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
