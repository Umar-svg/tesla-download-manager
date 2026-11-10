# One target per module (see docs/architecture.md). Modules that are still
# interface-only are INTERFACE libraries until their phase lands.

set(TDM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(TDM_GEN_ROOT ${CMAKE_BINARY_DIR}/generated)

# 0. core: shared types and protocol utilities (no Qt, no I/O)
add_library(tdm_core STATIC
    core/http_util.cpp)
target_include_directories(tdm_core PUBLIC ${TDM_SRC_ROOT} ${TDM_GEN_ROOT})
target_link_libraries(tdm_core PUBLIC tdm_options)

# 1+2. net: HTTP client + connection manager
add_library(tdm_net STATIC
    net/response_parser.cpp
    net/tls_context.cpp
    net/connection_manager.cpp
    net/http_client.cpp)
target_compile_definitions(tdm_net PUBLIC ASIO_NO_DEPRECATED)
target_link_libraries(tdm_net PUBLIC tdm_core asio::asio OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(tdm_net PUBLIC crypt32 ws2_32 mswsock)
endif()

# 4. file manager
add_library(tdm_fs STATIC
    fs/file_writer.cpp)
target_link_libraries(tdm_fs PUBLIC tdm_core PRIVATE OpenSSL::Crypto)

# 6. retry / recovery
add_library(tdm_retry STATIC
    retry/retry_policy.cpp)
target_link_libraries(tdm_retry PUBLIC tdm_core)

# 3. multipart controller: planner + transfer driver
add_library(tdm_multipart STATIC
    multipart/segment_planner.cpp
    multipart/transfer_job.cpp)
target_link_libraries(tdm_multipart PUBLIC tdm_core tdm_net tdm_fs tdm_retry tdm_scheduler)

# 7. database
add_library(tdm_db STATIC
    db/store.cpp)
target_link_libraries(tdm_db PUBLIC tdm_core
    PRIVATE unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json)

# 8. queue, 9. scheduler
add_library(tdm_scheduler STATIC
    scheduler/token_bucket.cpp
    scheduler/schedule.cpp)
target_link_libraries(tdm_scheduler PUBLIC tdm_core PRIVATE nlohmann_json::nlohmann_json)
add_library(tdm_queue STATIC
    queue/queue_selector.cpp)
target_link_libraries(tdm_queue PUBLIC tdm_core)

# 5. engine (Phase 5): ties the modules together
add_library(tdm_engine STATIC
    engine/engine.cpp)
target_link_libraries(tdm_engine PUBLIC
    tdm_net tdm_multipart tdm_fs tdm_retry tdm_db tdm_queue tdm_scheduler)

# Command-line front end (Qt-free; used by the benchmarks)
add_executable(tdm_cli cli/tdm_cli.cpp)
set_target_properties(tdm_cli PROPERTIES OUTPUT_NAME "tdm-cli")
target_link_libraries(tdm_cli PRIVATE tdm_options tdm_engine nlohmann_json::nlohmann_json)

# 10. UI
if(TDM_BUILD_UI)
    add_subdirectory(ui)
endif()
