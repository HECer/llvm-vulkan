cmake_minimum_required(VERSION 3.20)
project(LLVMVulkan VERSION 0.1.0 LANGUAGES C CXX)

option(LV_BUILD_LLVM_PLUGIN "Build opt/clang pass plugin against LLVM_DIR" ON)
option(LV_BUILD_TESTS "Build correctness tests" ON)
option(LV_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
find_program(SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
set(LV_SHADER_NAMES fold fold64 reachable_rounds reachable_components dominators_rounds dom_init
  dominators_local dominators_local_banked liveness_rounds liveness_local scan scan_subgroup scan_add sort sort_local
  radix_histogram radix_scatter hash)
set(LV_SPIRV_FILES)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(NAME IN LISTS LV_SHADER_NAMES)
  set(SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders/${NAME}.spv")
  if(SPIRV_VAL)
    add_custom_command(OUTPUT "${SPV}"
      COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME}.comp" -o "${SPV}"
      COMMAND "${SPIRV_VAL}" --target-env vulkan1.1 "${SPV}"
      DEPENDS "shaders/${NAME}.comp" VERBATIM)
  else()
    add_custom_command(OUTPUT "${SPV}"
      COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME}.comp" -o "${SPV}"
      DEPENDS "shaders/${NAME}.comp" VERBATIM)
  endif()
  list(APPEND LV_SPIRV_FILES "${SPV}")
endforeach()
set(LV_EMBEDDED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/lv/embedded_shaders.hpp")
add_custom_command(OUTPUT "${LV_EMBEDDED_HEADER}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_shaders.py" --output "${LV_EMBEDDED_HEADER}" ${LV_SPIRV_FILES}
  DEPENDS ${LV_SPIRV_FILES} scripts/embed_shaders.py VERBATIM)
add_custom_target(lv_shaders DEPENDS "${LV_EMBEDDED_HEADER}")
add_library(lv_compute STATIC src/compute.cpp src/cpu.cpp "${LV_EMBEDDED_HEADER}")
add_dependencies(lv_compute lv_shaders)
target_include_directories(lv_compute PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(lv_compute PUBLIC Vulkan::Vulkan)
target_compile_definitions(lv_compute PRIVATE LV_EMBEDDED_SHADERS=1 LV_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
if(MSVC)
  target_compile_options(lv_compute PRIVATE /W4 /permissive-)
else()
  target_compile_options(lv_compute PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(LV_ENABLE_SANITIZERS AND NOT MSVC)
  target_compile_options(lv_compute PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lv_compute PUBLIC -fsanitize=address,undefined)
endif()

add_executable(lv-bench src/bench.cpp)
target_link_libraries(lv-bench PRIVATE lv_compute)
if(LV_BUILD_LLVM_PLUGIN)
  find_package(LLVM REQUIRED CONFIG)
  add_subdirectory(llvm)
endif()

if(LV_BUILD_TESTS)
  enable_testing()
  add_executable(lv-core-tests tests/core_tests.cpp)
  target_link_libraries(lv-core-tests PRIVATE lv_compute)
  add_executable(lv-readback-failure tests/readback_failure.cpp)
  target_link_libraries(lv-readback-failure PRIVATE lv_compute)
  add_test(NAME core-cpu COMMAND lv-core-tests --backend cpu)
  add_test(NAME core-vulkan COMMAND lv-core-tests --backend vulkan)
  add_test(NAME core-vulkan-fallback COMMAND lv-core-tests --backend vulkan --fast-paths 0 --timestamps 0)
  add_test(NAME readback-allocation-failure COMMAND lv-readback-failure)
  add_test(NAME benchmark-smoke COMMAND lv-bench --backend vulkan --size 513 --repeat 2)
  add_test(NAME source-fetch-safety COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source_fetch.py")
  if(LV_BUILD_LLVM_PLUGIN)
    # Do not retain tools from an incompatible LLVM_DIR after reconfiguration.
    unset(LV_OPT CACHE)
    unset(LV_LLI CACHE)
    find_program(LV_OPT NAMES opt HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH)
    find_program(LV_LLI NAMES lli HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH)
    if(LV_OPT AND LV_LLI)
      add_test(NAME llvm-differential
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_llvm_tests.py"
          --opt "${LV_OPT}" --lli "${LV_LLI}" --plugin "$<TARGET_FILE:LLVMVulkan>"
          --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm"
          --output "${CMAKE_CURRENT_BINARY_DIR}/llvm-test-results.json")
      add_test(NAME llvm-constant-oracle
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm/verify_constant_oracle.py"
          --opt "${LV_OPT}" --plugin "$<TARGET_FILE:LLVMVulkan>"
          --backend vulkan --device any --cases 8192
          --output "${CMAKE_CURRENT_BINARY_DIR}/constant-oracle.json")
      add_test(NAME llvm-semantic-differential
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_llvm.py"
          --opt "${LV_OPT}" --lli "${LV_LLI}" --plugin "$<TARGET_FILE:LLVMVulkan>"
          --device any --output "${CMAKE_CURRENT_BINARY_DIR}/semantic-fuzz.json")
    else()
      message(FATAL_ERROR "Matching opt and lli are required for plugin tests. Build/install them from the selected LLVM, or set LV_BUILD_TESTS=OFF for a library-only build.")
    endif()
  endif()
endif()

install(TARGETS lv-bench RUNTIME DESTINATION bin)
if(LV_BUILD_LLVM_PLUGIN)
  install(TARGETS LLVMVulkan LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
endif()
