find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(tdm_tests
    http_util_test.cpp
    segment_planner_test.cpp
    retry_policy_test.cpp
    store_test.cpp
    headers_compile_test.cpp
    response_parser_test.cpp
    http_client_test.cpp
    file_writer_test.cpp
    file_writer_sharing_test.cpp
    transfer_job_test.cpp
    engine_test.cpp
    scheduler_test.cpp)
target_link_libraries(tdm_tests PRIVATE
    tdm_core tdm_net tdm_fs tdm_multipart tdm_retry tdm_db tdm_engine tdm_queue tdm_scheduler
    unofficial::sqlite3::sqlite3
    GTest::gtest GTest::gtest_main)
target_compile_definitions(tdm_tests PRIVATE
    TDM_PYTHON="${Python3_EXECUTABLE}"
    TDM_PROBE_SERVER="${PROJECT_SOURCE_DIR}/tools/testserver/probe_server.py"
    TDM_TEST_CERT="${CMAKE_CURRENT_SOURCE_DIR}/data/localhost.crt"
    TDM_TEST_KEY="${CMAKE_CURRENT_SOURCE_DIR}/data/localhost.key")

include(GoogleTest)
# Engine (Qt-free) tests: ctest -L engine
gtest_discover_tests(tdm_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS engine)

# CLI + benchmark runner smoke test (tiny scenarios, SHA-256 verified): ctest -L bench
add_test(NAME bench_quick
    COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/bench/run_bench.py"
            --cli "$<TARGET_FILE:tdm_cli>" --quick --reps 1)
set_tests_properties(bench_quick PROPERTIES LABELS "bench;engine" TIMEOUT 300)

# UI tests (offscreen Qt, real engine + probe server): ctest -L ui
if(TDM_BUILD_UI)
    add_executable(tdm_ui_tests
        ui/ui_test_main.cpp
        ui/ui_format_test.cpp
        ui/ui_model_test.cpp
        ui/ui_e2e_test.cpp
        ui/ui_instance_test.cpp)
    target_include_directories(tdm_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tdm_ui_tests PRIVATE tdm_ui_widgets OpenSSL::Crypto GTest::gtest)
    target_compile_definitions(tdm_ui_tests PRIVATE
        TDM_PYTHON="${Python3_EXECUTABLE}"
        TDM_PROBE_SERVER="${PROJECT_SOURCE_DIR}/tools/testserver/probe_server.py"
        TDM_TEST_CERT="${CMAKE_CURRENT_SOURCE_DIR}/data/localhost.crt"
        TDM_TEST_KEY="${CMAKE_CURRENT_SOURCE_DIR}/data/localhost.key"
        TDM_QT_PLUGIN_DIR="${TDM_QT_PLUGIN_DIR}")
    tdm_deploy_qt(tdm_ui_tests)
    gtest_discover_tests(tdm_ui_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS ui)
endif()
