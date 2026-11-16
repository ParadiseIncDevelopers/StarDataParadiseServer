cmake_minimum_required(VERSION 4.2)
project(StarDataParadiseServer VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(CTest)
option(SDP_WITH_CPU_MLP "Build the CPU MLP training module" ON)

add_executable(StarDataParadiseServer
    StarDataParadiseServer/StarDataParadiseServer.cpp

    StarDataParadiseServer/service.cpp
    StarDataParadiseServer/storage_admin.cpp
    StarDataParadiseServer/update_snapshot.cpp
    StarDataParadiseServer/operations.cpp StarDataParadiseServer/support_bundle.cpp
    StarDataParadiseServer/windows_service.cpp)

if(BUILD_TESTING)
    add_test(NAME help COMMAND StarDataParadiseServer --help)
    set_tests_properties(help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: StarDataParadiseServer")
    add_test(NAME version COMMAND StarDataParadiseServer --version)
    set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^StarDataParadiseServer 0[.]4[.]0")
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
add_library(sdp_server_config STATIC StarDataParadiseServer/config.cpp)
target_link_libraries(sdp_server_config PUBLIC sdp_wire sdp_tls)
target_link_libraries(StarDataParadiseServer PRIVATE sdp_server_config)

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
add_library(sdp_sql STATIC sql/syntax.cpp sql/types.cpp sql/catalog.cpp sql/binder.cpp
    sql/executor.cpp sql/session.cpp sql/wire_values.cpp
    sql/btree.cpp sql/definition.cpp sql/relational.cpp sql/advanced.cpp sql/collation.cpp sql/security.cpp sql/access.cpp sql/pl_ir.cpp sql/pl_runtime.cpp sql/routines.cpp sql/debugger.cpp
    sql/history_journal.cpp sql/history_query.cpp sql/history_restore.cpp
    sql/decision.cpp sql/decision_condition.cpp sql/decision_inspection.cpp
    jobs/manager.cpp jobs/schedule.cpp jobs/scheduled_retrain.cpp jobs/worker.cpp backup/archive.cpp
    ml/dataset.cpp ml/preprocess.cpp ml/pl_dataset_job.cpp
    ml/model.cpp ml/training.cpp ml/devices.cpp ml/linear_models.cpp ml/tree_models.cpp ml/clustering.cpp ml/pca.cpp)
if(SDP_WITH_CPU_MLP)
    target_sources(sdp_sql PRIVATE ml/mlp.cpp)
    set_source_files_properties(ml/training.cpp PROPERTIES COMPILE_DEFINITIONS SDP_WITH_CPU_MLP=1)
endif()
target_include_directories(sdp_sql PUBLIC sql)
target_compile_options(sdp_sql PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(sdp_sql PUBLIC sdp_storage sdp_wire sdp_server_config PRIVATE advapi32)
target_link_libraries(StarDataParadiseServer PRIVATE sdp_sql)

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
if(BUILD_TESTING)
    add_executable(query_smoke tests/query_smoke.cpp)
    target_compile_options(query_smoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(query_smoke PRIVATE sdp_sql)
    add_test(NAME sql_advanced COMMAND query_smoke "${CMAKE_CURRENT_BINARY_DIR}/query-fixtures")
    set_tests_properties(sql_advanced PROPERTIES TIMEOUT 120)
    add_executable(relations_smoke tests/relations_smoke.cpp)
    target_link_libraries(relations_smoke PRIVATE sdp_sql)
    add_test(NAME sql_relational COMMAND relations_smoke "${CMAKE_CURRENT_BINARY_DIR}/relational-fixtures")
    set_tests_properties(sql_relational PROPERTIES TIMEOUT 90)
    add_executable(ledger_smoke tests/ledger_smoke.cpp)
    target_link_libraries(ledger_smoke PRIVATE sdp_sql)
    add_test(NAME sql_outcome_ledger COMMAND ledger_smoke "${CMAKE_CURRENT_BINARY_DIR}/ledger-fixtures")
    add_executable(sql_smoke tests/sql_smoke.cpp)
    target_compile_options(sql_smoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(sql_smoke PRIVATE sdp_sql)
    foreach(part catalog types parser)
        add_test(NAME sql_${part} COMMAND sql_smoke ${part} "${CMAKE_CURRENT_BINARY_DIR}/sql-fixtures")
        set_tests_properties(sql_${part} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(linear_models_smoke tests/linear_models_smoke.cpp)
    target_include_directories(linear_models_smoke PRIVATE ml)
    target_link_libraries(linear_models_smoke PRIVATE sdp_sql)
    add_test(NAME ml_linear_models COMMAND linear_models_smoke)
    add_executable(tree_models_smoke tests/tree_models_smoke.cpp)
    target_include_directories(tree_models_smoke PRIVATE ml)
    target_link_libraries(tree_models_smoke PRIVATE sdp_sql)
    add_test(NAME ml_tree_models COMMAND tree_models_smoke)
    add_test(NAME model_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/model_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/model-rpc")
    set_tests_properties(model_rpc PROPERTIES TIMEOUT 120)
    if(SDP_WITH_CPU_MLP)
        add_executable(mlp_smoke tests/mlp_smoke.cpp)
        target_include_directories(mlp_smoke PRIVATE ml)
        target_link_libraries(mlp_smoke PRIVATE sdp_sql)
        add_test(NAME ml_mlp COMMAND mlp_smoke)
        add_test(NAME mlp_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/mlp_rpc.py"
            --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
            --scratch "${CMAKE_CURRENT_BINARY_DIR}/mlp-rpc")
        set_tests_properties(mlp_rpc PROPERTIES TIMEOUT 150)
    endif()
    if(SDP_WITH_CPU_MLP)
        add_test(NAME model_lifecycle_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/model_lifecycle_rpc.py"
            --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
            --scratch "${CMAKE_CURRENT_BINARY_DIR}/model-lifecycle-rpc")
        set_tests_properties(model_lifecycle_rpc PROPERTIES TIMEOUT 150)
    endif()
    add_executable(model_lifecycle_smoke tests/model_lifecycle_smoke.cpp)
    target_include_directories(model_lifecycle_smoke PRIVATE ml)
    target_link_libraries(model_lifecycle_smoke PRIVATE sdp_sql)
    add_test(NAME model_lifecycle_catalog COMMAND model_lifecycle_smoke "${CMAKE_CURRENT_BINARY_DIR}/model-lifecycle-fixtures")
    add_executable(physical_snapshot_smoke tests/physical_snapshot_smoke.cpp)
    target_link_libraries(physical_snapshot_smoke PRIVATE sdp_storage)
    add_test(NAME backup_physical_snapshot COMMAND physical_snapshot_smoke "${CMAKE_CURRENT_BINARY_DIR}/physical-snapshot-fixtures")
    add_test(NAME admin_health_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/admin_health_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/admin-health-rpc")
    set_tests_properties(admin_health_rpc PROPERTIES TIMEOUT 150)
    add_test(NAME backup_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/backup_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/backup-rpc")
    set_tests_properties(backup_rpc PROPERTIES TIMEOUT 150)
    add_executable(clustering_smoke tests/clustering_smoke.cpp)
    target_link_libraries(clustering_smoke PRIVATE sdp_sql)
    add_test(NAME ml_clustering COMMAND clustering_smoke)
    add_executable(pca_smoke tests/pca_smoke.cpp)
    target_include_directories(pca_smoke PRIVATE ml)
    target_link_libraries(pca_smoke PRIVATE sdp_sql)
    add_test(NAME ml_pca COMMAND pca_smoke)
    add_test(NAME unsupervised_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/unsupervised_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/unsupervised-rpc")
    set_tests_properties(unsupervised_rpc PROPERTIES TIMEOUT 120)
    add_executable(preprocess_smoke tests/preprocess_smoke.cpp)
    target_include_directories(preprocess_smoke PRIVATE ml)
    target_link_libraries(preprocess_smoke PRIVATE sdp_sql)
    add_test(NAME dataset_preprocessing COMMAND preprocess_smoke)
    add_test(NAME dataset_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/dataset_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/dataset-rpc")
    set_tests_properties(dataset_rpc PROPERTIES TIMEOUT 120)
    add_executable(job_worker_smoke tests/job_worker_smoke.cpp)
    target_include_directories(job_worker_smoke PRIVATE jobs)
    target_link_libraries(job_worker_smoke PRIVATE sdp_sql)
    add_test(NAME job_worker COMMAND job_worker_smoke "${CMAKE_CURRENT_BINARY_DIR}/job-worker-fixtures")
    set_tests_properties(job_worker PROPERTIES TIMEOUT 60)
    add_executable(schedule_smoke tests/schedule_smoke.cpp)
    target_include_directories(schedule_smoke PRIVATE jobs)
    target_link_libraries(schedule_smoke PRIVATE sdp_sql)
    add_test(NAME schedule_calendar COMMAND schedule_smoke)
    add_test(NAME jobs_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/jobs_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/jobs-rpc")
    set_tests_properties(jobs_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME scheduled_retrain_rpc COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/scheduled_retrain_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/retrain-rpc")
    set_tests_properties(scheduled_retrain_rpc PROPERTIES TIMEOUT 80)
    add_test(NAME scheduled_sql_rpc COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/scheduled_sql_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/scheduled-sql-rpc")
    set_tests_properties(scheduled_sql_rpc PROPERTIES TIMEOUT 120)
    add_executable(decision_condition_smoke tests/decision_condition_smoke.cpp)
    target_link_libraries(decision_condition_smoke PRIVATE sdp_sql)
    add_test(NAME decision_condition COMMAND decision_condition_smoke "${CMAKE_CURRENT_BINARY_DIR}/decision-condition")
    add_test(NAME decision_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/decision-rpc")
    add_test(NAME decision_inspection_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_inspection_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/decision-inspection-rpc")
    set_tests_properties(decision_condition decision_rpc decision_inspection_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME history_restore_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/history_restore_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/restore-rpc")
    set_tests_properties(history_restore_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME history_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/history_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/history-rpc")
    set_tests_properties(history_rpc PROPERTIES TIMEOUT 120)
    add_executable(history_journal_smoke tests/history_journal_smoke.cpp)
    target_compile_definitions(history_journal_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(history_journal_smoke PRIVATE sdp_sql)
    add_test(NAME history_journal_storage COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/history_journal_storage.py" --runner $<TARGET_FILE:history_journal_smoke>
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/history-journal")
    set_tests_properties(history_journal_storage PROPERTIES TIMEOUT 120)
    add_test(NAME debugger_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/debugger_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/debugger-rpc")
    set_tests_properties(debugger_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME triggers_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/triggers_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/triggers-rpc")
    set_tests_properties(triggers_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME plsql_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/plsql_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/plsql-rpc")
    set_tests_properties(plsql_rpc PROPERTIES TIMEOUT 120)
    add_test(NAME security_rpc COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/security_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/security-rpc")
    set_tests_properties(security_rpc PROPERTIES TIMEOUT 90)
    add_test(NAME sql_rpc COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/sql_rpc.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/sql-rpc")
    set_tests_properties(sql_rpc PROPERTIES TIMEOUT 90)
    add_test(NAME table_editor_ddl COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/table_editor_ddl.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --openssl "${SDP_OPENSSL_TOOL}"
        --scratch "${CMAKE_CURRENT_BINARY_DIR}/table-editor-ddl")
    set_tests_properties(table_editor_ddl PROPERTIES TIMEOUT 120)
    add_test(NAME sql_checker COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/sql_checker.py"
        --server $<TARGET_FILE:StarDataParadiseServer> --scratch "${CMAKE_CURRENT_BINARY_DIR}/sql-checker")
endif()

option(SDP_STORAGE_ASAN "Instrument the Server storage and SQL implementation with MSVC AddressSanitizer" OFF)
if(SDP_STORAGE_ASAN)
    target_compile_options(sdp_storage PRIVATE /fsanitize=address /Z7)
    target_compile_options(sdp_sql PRIVATE /fsanitize=address /Z7)
    target_compile_options(sdp_server_config PRIVATE /fsanitize=address /Z7)
    foreach(storage_consumer StarDataParadiseServer storage_smoke transaction_smoke recovery_driver sql_smoke ledger_smoke relations_smoke query_smoke)
        if(TARGET ${storage_consumer})
            target_link_options(${storage_consumer} PRIVATE /DEBUG /INCREMENTAL:NO)
            target_compile_options(${storage_consumer} PRIVATE /fsanitize=address /Z7)
        endif()
    endforeach()
endif()
