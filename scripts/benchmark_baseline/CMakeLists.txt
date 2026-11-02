cmake_minimum_required(VERSION 3.20)
project(LLVMVulkanMatchedBenchmark LANGUAGES C CXX)

# Build the current workload generator against an archived runtime, rather than
# comparing different input generators when adding new benchmark cases.
get_filename_component(LV_CURRENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LV_BASELINE_SOURCE "" CACHE PATH "Extracted baseline source archive")
if(NOT EXISTS "${LV_BASELINE_SOURCE}/BASELINE.json" OR
   NOT EXISTS "${LV_BASELINE_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Set LV_BASELINE_SOURCE to the extracted source archive containing BASELINE.json")
endif()
set(LV_BUILD_LLVM_PLUGIN OFF CACHE BOOL "Baseline core only" FORCE)
set(LV_BUILD_TESTS OFF CACHE BOOL "Benchmark fixture only" FORCE)
add_subdirectory("${LV_BASELINE_SOURCE}" runtime EXCLUDE_FROM_ALL)
add_executable(lv-bench-matched "${LV_CURRENT_ROOT}/src/bench.cpp")
target_compile_features(lv-bench-matched PRIVATE cxx_std_17)
target_link_libraries(lv-bench-matched PRIVATE lv_compute)
