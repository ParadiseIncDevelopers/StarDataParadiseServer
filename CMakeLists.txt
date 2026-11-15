cmake_minimum_required(VERSION 4.2)
project(StarDataParadiseServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(CTest)

add_executable(StarDataParadiseServer
    StarDataParadiseServer/StarDataParadiseServer.cpp
    StarDataParadiseServer/config.cpp
    StarDataParadiseServer/service.cpp
    StarDataParadiseServer/storage_admin.cpp)

if(BUILD_TESTING)
    add_test(NAME help COMMAND StarDataParadiseServer --help)
    set_tests_properties(help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: StarDataParadiseServer")
    add_test(NAME version COMMAND StarDataParadiseServer --version)
    set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^StarDataParadiseServer 0[.]1[.]0")
    add_test(NAME invalid_option COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:StarDataParadiseServer>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_option.cmake")
endif()

include(cmake/ProtocolJson.cmake)
add_library(sdp_wire INTERFACE)
target_include_directories(sdp_wire INTERFACE contracts/v1/cpp)
target_link_libraries(sdp_wire INTERFACE nlohmann_json::nlohmann_json)

if(BUILD_TESTING)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    add_executable(wire_smoke contracts/v1/tests/wire_smoke.cpp)
    target_link_libraries(wire_smoke PRIVATE sdp_wire)
    target_compile_options(wire_smoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME wire_contract COMMAND wire_smoke
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/golden/values.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/capabilities.json")

    set(contract_archive "${CMAKE_CURRENT_BINARY_DIR}/contracts/sdp-contract-v1.zip")
    add_test(NAME package_contract COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/tools/package_contract.py" "${contract_archive}")
    set_tests_properties(package_contract PROPERTIES FIXTURES_SETUP contract_archive)
    add_test(NAME reproduce_contract COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/tools/package_contract.py" "${contract_archive}" --check)
    set_tests_properties(reproduce_contract PROPERTIES FIXTURES_REQUIRED contract_archive)
endif()

include(cmake/Transport.cmake)
target_link_libraries(StarDataParadiseServer PRIVATE sdp_wire sdp_tls)

if(BUILD_TESTING)
    find_program(SDP_OPENSSL_TOOL openssl HINTS "${OPENSSL_ROOT_DIR}/bin" REQUIRED)
    add_test(NAME server_tls_lifecycle COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/service_smoke.py" --server $<TARGET_FILE:StarDataParadiseServer>
        --openssl "${SDP_OPENSSL_TOOL}" --scratch "${CMAKE_CURRENT_BINARY_DIR}/test-instances")
    set_tests_properties(server_tls_lifecycle PROPERTIES TIMEOUT 60)
endif()
add_library(sdp_storage STATIC storage/disk.cpp storage/heap.cpp storage/wal.cpp storage/transactions.cpp storage/recovery.cpp)
target_include_directories(sdp_storage PUBLIC storage)
target_compile_definitions(sdp_storage PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(StarDataParadiseServer PRIVATE sdp_storage)

if(BUILD_TESTING)
    add_executable(storage_smoke tests/storage_smoke.cpp)
    target_compile_options(storage_smoke PRIVATE /utf-8)
    target_compile_definitions(storage_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(storage_smoke PRIVATE sdp_storage)
    add_test(NAME storage_format COMMAND storage_smoke "${CMAKE_CURRENT_BINARY_DIR}/storage-fixtures")
    add_test(NAME storage_ownership COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_ownership.py" --server $<TARGET_FILE:StarDataParadiseServer>
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/storage-test-instances")
endif()
if(BUILD_TESTING)
    add_executable(transaction_smoke tests/transaction_smoke.cpp)
    target_compile_definitions(transaction_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(transaction_smoke PRIVATE sdp_storage)
    add_test(NAME transaction_wal COMMAND transaction_smoke "${CMAKE_CURRENT_BINARY_DIR}/transaction-fixtures")
    set_tests_properties(transaction_wal PROPERTIES TIMEOUT 60)
endif()
if(BUILD_TESTING)
    add_executable(recovery_driver tests/recovery_driver.cpp)
    target_compile_definitions(recovery_driver PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(recovery_driver PRIVATE sdp_storage)
    set(SDP_TEST_STORAGE_V1 "" CACHE FILEPATH "Optional saved stage 08 disk fixture")
    set(SDP_TEST_WAL_V1 "" CACHE PATH "Optional saved stage 09 fixture directory")
    set(recovery_fixtures "")
    if(SDP_TEST_STORAGE_V1)
        list(APPEND recovery_fixtures --legacy-storage "${SDP_TEST_STORAGE_V1}")
    endif()
    if(SDP_TEST_WAL_V1)
        list(APPEND recovery_fixtures --legacy-wal "${SDP_TEST_WAL_V1}")
    endif()
    add_test(NAME crash_recovery COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_matrix.py"
        --driver $<TARGET_FILE:recovery_driver> --server $<TARGET_FILE:StarDataParadiseServer>
        --openssl "${SDP_OPENSSL_TOOL}" --scratch "${CMAKE_CURRENT_BINARY_DIR}/recovery-evidence" ${recovery_fixtures})
    set_tests_properties(crash_recovery PROPERTIES TIMEOUT 120)
endif()
option(SDP_STORAGE_ASAN "Instrument the Server storage implementation with MSVC AddressSanitizer" OFF)
if(SDP_STORAGE_ASAN)
    target_compile_options(sdp_storage PRIVATE /fsanitize=address /Z7)
    foreach(storage_consumer StarDataParadiseServer storage_smoke transaction_smoke recovery_driver)
        if(TARGET ${storage_consumer})
            target_link_options(${storage_consumer} PRIVATE /DEBUG /INCREMENTAL:NO)
            target_compile_options(${storage_consumer} PRIVATE /fsanitize=address /Z7)
        endif()
    endforeach()
endif()
