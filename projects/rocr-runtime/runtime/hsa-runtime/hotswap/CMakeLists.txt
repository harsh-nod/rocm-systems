# Standalone build for hotswap library (LLVM-free, uses COMGR via dlopen)
cmake_minimum_required(VERSION 3.16)
project(rocr-hotswap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rocr-hotswap STATIC
  hotswap.cpp
  hotswap_comgr_client.cpp
  hotswap_core.cpp
  hotswap_rules.cpp
  trampoline.cpp
)

target_include_directories(rocr-hotswap PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc
)

target_link_libraries(rocr-hotswap PUBLIC dl)
target_compile_definitions(rocr-hotswap PUBLIC ROCR_HOTSWAP_ENABLED=1)

# ── Test targets ──────────────────────────────────────────────────────────────

add_executable(hotswap_test tests/hotswap_test.cpp)
target_link_libraries(hotswap_test PRIVATE rocr-hotswap)
target_include_directories(hotswap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(b0a0_retarget_tool tests/test_b0a0_retarget.cpp)
target_link_libraries(b0a0_retarget_tool PRIVATE rocr-hotswap)
target_include_directories(b0a0_retarget_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(b0a0_hsaco_test tests/test_b0a0_hsaco.cpp)
target_link_libraries(b0a0_hsaco_test PRIVATE rocr-hotswap)
target_include_directories(b0a0_hsaco_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_program(LLVM_OBJDUMP_BIN llvm-objdump
  HINTS ENV LLVM_OBJDUMP
        /home/harmenon/dockerx/llvm-project/build/bin
  NO_DEFAULT_PATH)
if(NOT LLVM_OBJDUMP_BIN)
  find_program(LLVM_OBJDUMP_BIN llvm-objdump)
endif()
if(NOT LLVM_OBJDUMP_BIN)
  set(LLVM_OBJDUMP_BIN "llvm-objdump")
endif()

target_compile_definitions(b0a0_hsaco_test PRIVATE
  HSACO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/hsaco_data"
  LLVM_OBJDUMP="${LLVM_OBJDUMP_BIN}"
)

add_executable(dwarf_test tests/test_dwarf.cpp)
target_link_libraries(dwarf_test PRIVATE rocr-hotswap)
target_include_directories(dwarf_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dwarf_test PRIVATE
  HSACO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/hsaco_data"
)

add_executable(dataflow_test tests/test_dataflow.cpp)
target_link_libraries(dataflow_test PRIVATE dl)
target_include_directories(dataflow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

