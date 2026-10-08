cmake_minimum_required(VERSION 3.19)
project(
        sts_lightspeed
        VERSION 0.1
        DESCRIPTION "High Performance, rng accurate implementation of Slay the Spire with tools for tree search and ml."
        LANGUAGES CXX)

file (GLOB_RECURSE sts_lightspeed_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS  "-Wno-shift-count-overflow -O3 -include algorithm")

add_subdirectory(json)
add_subdirectory(pybind11)

pybind11_add_module(slaythespire bindings/slaythespire.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_include_directories(slaythespire PUBLIC include)
target_include_directories(slaythespire PUBLIC json/single_include)
target_include_directories(slaythespire PUBLIC bindings)


add_executable(main apps/main.cpp ${sts_lightspeed_SOURCES})
target_link_directories(main PRIVATE json::nlohmann_json)
target_include_directories(main PUBLIC include)
target_include_directories(main PUBLIC json/include)

add_executable(test apps/test.cpp ${sts_lightspeed_SOURCES})
target_include_directories(test PUBLIC include)
target_link_directories(test PUBLIC json::nlohmann_json)
target_include_directories(test PUBLIC json/include)

add_executable(small-test apps/small-test.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_link_directories(small-test PRIVATE json::nlohmann_json)
target_include_directories(small-test PUBLIC include)
target_include_directories(small-test PUBLIC json/include)
target_include_directories(small-test PUBLIC bindings)

add_executable(test-public-battle-state-semantics EXCLUDE_FROM_ALL
        apps/test_public_battle_state_semantics.cpp
        bindings/bindings-util.cpp
        ${sts_lightspeed_SOURCES})
target_compile_definitions(test-public-battle-state-semantics PRIVATE
        STS_LIGHTSPEED_NO_PYBIND_MODULE)
target_include_directories(test-public-battle-state-semantics PUBLIC include)
target_include_directories(test-public-battle-state-semantics PUBLIC json/single_include)
target_include_directories(test-public-battle-state-semantics PUBLIC bindings)
target_link_libraries(test-public-battle-state-semantics PRIVATE pybind11::embed)

add_executable(test-public-noncombat-projection EXCLUDE_FROM_ALL
        apps/test_public_noncombat_projection.cpp
        bindings/bindings-util.cpp
        ${sts_lightspeed_SOURCES})
target_compile_definitions(test-public-noncombat-projection PRIVATE
        STS_LIGHTSPEED_NO_PYBIND_MODULE)
target_include_directories(test-public-noncombat-projection PUBLIC include)
target_include_directories(test-public-noncombat-projection PUBLIC json/single_include)
target_include_directories(test-public-noncombat-projection PUBLIC bindings)
target_link_libraries(test-public-noncombat-projection PRIVATE pybind11::embed)

add_executable(study-issue23-natural-search EXCLUDE_FROM_ALL
        apps/study_issue23_natural_search.cpp
        bindings/bindings-util.cpp
        ${sts_lightspeed_SOURCES})
target_compile_definitions(study-issue23-natural-search PRIVATE
        STS_LIGHTSPEED_NO_PYBIND_MODULE)
target_include_directories(study-issue23-natural-search PUBLIC include)
target_include_directories(study-issue23-natural-search PUBLIC json/single_include)
target_include_directories(study-issue23-natural-search PUBLIC bindings)
target_link_libraries(study-issue23-natural-search PRIVATE pybind11::embed)

execute_process(
        COMMAND git rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE STS_LIGHTSPEED_TESTED_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE STS_LIGHTSPEED_GIT_RESULT
        ERROR_QUIET)
if(NOT STS_LIGHTSPEED_GIT_RESULT EQUAL 0 OR NOT STS_LIGHTSPEED_TESTED_SOURCE_COMMIT)
    set(STS_LIGHTSPEED_TESTED_SOURCE_COMMIT "unknown")
endif()
target_compile_definitions(test-public-noncombat-projection PRIVATE
        STS_LIGHTSPEED_TESTED_SOURCE_COMMIT="${STS_LIGHTSPEED_TESTED_SOURCE_COMMIT}")
