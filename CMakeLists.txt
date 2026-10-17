cmake_minimum_required(VERSION 3.24)
project(price-app LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
option(CRYPTO_BUILD_VIEWER "Build the Windows platform-h viewer" ${WIN32})
option(CRYPTO_BUILD_STORAGE_BENCH "Build opt-in SPQB/spike-db measurement tool" ON)
if(CRYPTO_BUILD_VIEWER AND NOT MSVC)
    message(FATAL_ERROR "The viewer requires Windows/MSVC. Use -DCRYPTO_BUILD_VIEWER=OFF for headless CLI.")
endif()
include(CTest)
set(NN_ENABLE_AVX2 OFF CACHE BOOL "DQN uses precise scalar binary32 kernels" FORCE)
include(cmake/dependencies.cmake)
crypto_dependency(libcmlp)
crypto_dependency(libcfindata)
if(CRYPTO_BUILD_STORAGE_BENCH)
    crypto_dependency(spike-db)
endif()
if(CRYPTO_BUILD_VIEWER)
    crypto_dependency(platform-h)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/dependency-fingerprint.txt" "${crypto_fingerprint}")

function(crypto_options target)
    if(MSVC)
        target_include_directories(${target} PRIVATE src)
        target_compile_options(${target} PRIVATE /W3 /permissive- /fp:precise $<$<CONFIG:Release>:/arch:AVX2>)
        target_link_options(${target} PRIVATE /DEBUG $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    else()
        # Our features.h is not glibc's <features.h>.
        target_compile_options(${target} PRIVATE "-iquote${CMAKE_CURRENT_SOURCE_DIR}/src"
            -Wall -Wextra -ffp-contract=off -fno-fast-math)
    endif()
endfunction()

add_library(crypto_core STATIC src/agents.cpp src/bars.cpp src/common.cpp
    src/env.cpp src/experiment.cpp src/features.cpp src/metrics.cpp
    src/patterns.cpp src/run.cpp)
crypto_options(crypto_core)
string(JSON crypto_cmlp_revision GET "${crypto_dependencies}" dependencies libcmlp commit)
target_compile_definitions(crypto_core PUBLIC
    CRYPTO_DEPENDENCY_MANIFEST_SHA256="${crypto_manifest_hash}"
    CRYPTO_CMLP_REVISION="${crypto_cmlp_revision}")
add_library(crypto_network STATIC src/cmlp_adapter.cpp)
target_link_libraries(crypto_network PUBLIC cmlp)
crypto_options(crypto_network)
target_link_libraries(crypto_core PUBLIC crypto_network)
add_executable(crypto_cli src/cli.cpp src/download.cpp src/publication.cpp)
if(WIN32)
    # Without it a path outside the ANSI code page (C:\data\漢字) reaches the CLI as '??'.
    target_sources(crypto_cli PRIVATE src/cli.manifest)
endif()
target_link_libraries(crypto_cli PRIVATE crypto_core cfindata)
crypto_options(crypto_cli)
set_target_properties(crypto_cli PROPERTIES OUTPUT_NAME cli)
set(crypto_binaries crypto_cli)

if(CRYPTO_BUILD_VIEWER)
    platform_add_app(crypto_app SOURCES src/app.cpp src/bars.cpp src/common.cpp
        ICON src/app.ico DESCRIPTION "price-app viewer" OUTPUT_NAME app)
    crypto_options(crypto_app)
    list(APPEND crypto_binaries crypto_app)
endif()
foreach(target IN LISTS crypto_binaries)
    set_target_properties(${target} PROPERTIES DEBUG_POSTFIX -d
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/exe"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/exe")
endforeach()

if(CRYPTO_BUILD_STORAGE_BENCH)
    add_executable(storage_bench tests/storage_bench.cpp src/bars.cpp src/common.cpp)
    target_link_libraries(storage_bench PRIVATE spike_db)
    crypto_options(storage_bench)
    if(WIN32)
        target_link_libraries(storage_bench PRIVATE psapi)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(download_offline_probe src/cli.cpp src/download.cpp src/publication.cpp
        tests/no_network_transport.cpp)
    target_link_libraries(download_offline_probe PRIVATE crypto_core cfindata)
    crypto_options(download_offline_probe)
    add_executable(publication_probe tests/publication_probe.cpp src/publication.cpp)
    crypto_options(publication_probe)
    add_executable(crypto_tests tests/network.cpp)
    target_link_libraries(crypto_tests PRIVATE crypto_network)
    crypto_options(crypto_tests)
    add_test(NAME network COMMAND crypto_tests "${CMAKE_BINARY_DIR}/test-artifacts")
    set_tests_properties(network PROPERTIES LABELS crypto TIMEOUT 120)
    find_program(PWSH_EXECUTABLE NAMES pwsh REQUIRED)
    add_executable(market_tests tests/market.cpp)
    target_link_libraries(market_tests PRIVATE crypto_core)
    crypto_options(market_tests)
    add_test(NAME market_contract COMMAND market_tests "${CMAKE_BINARY_DIR}/test-artifacts/market")
    add_test(NAME generic_predictor COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/tests/generic.ps1" -Cli "$<TARGET_FILE:crypto_cli>"
        -Directory "${CMAKE_BINARY_DIR}/test-artifacts/generic")
    set_tests_properties(market_contract generic_predictor PROPERTIES LABELS crypto TIMEOUT 300)
    add_test(NAME publication_concurrency COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/tests/publication.ps1" -Probe "$<TARGET_FILE:publication_probe>"
        -Directory "${CMAKE_BINARY_DIR}/test-artifacts/publication")
    set_tests_properties(publication_concurrency PROPERTIES LABELS crypto TIMEOUT 120)
    add_test(NAME acquisition COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/tests/acquisition.ps1" -Cli "$<TARGET_FILE:crypto_cli>"
        -OfflineCli "$<TARGET_FILE:download_offline_probe>"
        -Directory "${CMAKE_BINARY_DIR}/test-artifacts/acquisition")
    add_test(NAME agents_causality COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/tests/e2e.ps1" -Cli "$<TARGET_FILE:crypto_cli>"
        -Directory "${CMAKE_BINARY_DIR}/test-artifacts/agents")
    set_tests_properties(acquisition agents_causality PROPERTIES LABELS crypto TIMEOUT 300)
    add_test(NAME dd_adoption COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File "${CMAKE_SOURCE_DIR}/tests/dd-adoption.ps1")
    set_tests_properties(dd_adoption PROPERTIES LABELS crypto TIMEOUT 120)
    if(CRYPTO_BUILD_STORAGE_BENCH)
        add_test(NAME storage_parity COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
            "${CMAKE_SOURCE_DIR}/tests/storage.ps1" -Bench "$<TARGET_FILE:storage_bench>"
            -Directory "${CMAKE_BINARY_DIR}/test-artifacts/storage")
        set_tests_properties(storage_parity PROPERTIES LABELS crypto TIMEOUT 120)
    endif()
    if(CRYPTO_BUILD_VIEWER)
        add_test(NAME viewer_offscreen COMMAND crypto_app --offscreen)
        add_test(NAME viewer_inputs COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File
            "${CMAKE_SOURCE_DIR}/tests/viewer.ps1" -App "$<TARGET_FILE:crypto_app>" -Cli "$<TARGET_FILE:crypto_cli>"
            -Directory "${CMAKE_BINARY_DIR}/test-artifacts/viewer")
        set_tests_properties(viewer_offscreen PROPERTIES LABELS crypto TIMEOUT 30)
        set_tests_properties(viewer_inputs PROPERTIES LABELS crypto TIMEOUT 120)
    endif()
endif()
