# Optional standalone build for hotswap library (for testing outside ROCR)
cmake_minimum_required(VERSION 3.16)
project(rocr-hotswap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# Map LLVM components to libraries
llvm_map_components_to_libnames(LLVM_LIBS
  AMDGPUDisassembler
  AMDGPUCodeGen
  AMDGPUDesc
  AMDGPUInfo
  AMDGPUAsmParser
  MC
  MCParser
  MCDisassembler
  Support
)

add_library(rocr-hotswap STATIC
  hotswap.cpp
  hotswap_rules.cpp
  trampoline.cpp
  transpiler.cpp
)

target_include_directories(rocr-hotswap PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc
)

target_link_libraries(rocr-hotswap PUBLIC ${LLVM_LIBS})
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
target_compile_definitions(b0a0_hsaco_test PRIVATE
  HSACO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/hsaco_data"
  LLVM_OBJDUMP="${LLVM_DIR}/../../../bin/llvm-objdump"
)

