cmake_minimum_required(VERSION 3.16)
project(EdgeMeshC VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

find_package(Threads REQUIRED)
include(CTest)

add_library(edgemesh_core STATIC src/core.c src/http_response.c)
target_include_directories(edgemesh_core PUBLIC include)
target_compile_options(edgemesh_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(edgemesh_net STATIC src/net.c src/clock.c)
target_include_directories(edgemesh_net PUBLIC include)
target_link_libraries(edgemesh_net PUBLIC Threads::Threads)
target_compile_options(edgemesh_net PRIVATE -Wall -Wextra -Wpedantic)

add_executable(edgemesh
    src/main.c src/proxy.c src/admin_api.c src/health.c src/metrics.c)
target_include_directories(edgemesh PRIVATE include)
target_link_libraries(edgemesh PRIVATE edgemesh_core edgemesh_net Threads::Threads)
target_compile_options(edgemesh PRIVATE -Wall -Wextra -Wpedantic)

add_executable(edgemesh_demo_backend src/demo_backend.c)
target_include_directories(edgemesh_demo_backend PRIVATE include)
target_link_libraries(edgemesh_demo_backend PRIVATE edgemesh_net Threads::Threads)
target_compile_options(edgemesh_demo_backend PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE edgemesh_core)
    target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME core_unit COMMAND test_core)

    add_executable(test_metrics tests/test_metrics.c src/metrics.c)
    target_include_directories(test_metrics PRIVATE include)
    target_link_libraries(test_metrics PRIVATE Threads::Threads)
    target_compile_options(test_metrics PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME metrics_unit COMMAND test_metrics)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME local_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                    --proxy $<TARGET_FILE:edgemesh>
                    --backend $<TARGET_FILE:edgemesh_demo_backend>)
        set_tests_properties(local_integration PROPERTIES TIMEOUT 90)
    endif()
endif()
