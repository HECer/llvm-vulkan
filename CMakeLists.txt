cmake_minimum_required(VERSION 3.20)
project(LLVMVulkan VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/lv/version.hpp" @ONLY)

option(LV_BUILD_LLVM_PLUGIN "Build opt/clang pass plugin against LLVM_DIR" ON)
option(LV_BUILD_TESTS "Build correctness tests" ON)
option(LV_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(LV_REQUIRE_SPIRV_VALIDATION "Require build-time SPIR-V validation" ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
find_program(SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(LV_REQUIRE_SPIRV_VALIDATION AND NOT SPIRV_VAL)
  message(FATAL_ERROR "spirv-val is required. Install SPIRV-Tools, or explicitly set LV_REQUIRE_SPIRV_VALIDATION=OFF for a developer build.")
endif()
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
add_library(LLVMVulkan::compute ALIAS lv_compute)
set_target_properties(lv_compute PROPERTIES EXPORT_NAME compute)
target_include_directories(lv_compute PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(lv_compute PUBLIC cxx_std_17)
target_link_libraries(lv_compute PUBLIC Vulkan::Vulkan)
target_compile_definitions(lv_compute PRIVATE LV_EMBEDDED_SHADERS=1)
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
  add_test(NAME version COMMAND lv-bench --version)
  set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "LLVM Vulkan ${PROJECT_VERSION}")
  add_test(NAME installed-consumer COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.py"
    --build-dir "${CMAKE_CURRENT_BINARY_DIR}" --config "$<CONFIG>"
    --cmake "${CMAKE_COMMAND}" --generator "${CMAKE_GENERATOR}"
    --platform "${CMAKE_GENERATOR_PLATFORM}" --toolset "${CMAKE_GENERATOR_TOOLSET}")
  set_tests_properties(installed-consumer PROPERTIES TIMEOUT 180)
  add_test(NAME documented-benchmarks COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_graphs.py")
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

install(TARGETS lv_compute EXPORT LLVMVulkanTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/lv DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.hpp")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/lv/version.hpp" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lv")
set(LV_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/LLVMVulkan")
configure_package_config_file(cmake/LLVMVulkanConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/LLVMVulkanConfig.cmake" INSTALL_DESTINATION "${LV_PACKAGE_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/LLVMVulkanConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(EXPORT LLVMVulkanTargets NAMESPACE LLVMVulkan:: DESTINATION "${LV_PACKAGE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LLVMVulkanConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/LLVMVulkanConfigVersion.cmake" DESTINATION "${LV_PACKAGE_DIR}")
install(TARGETS lv-bench RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(LV_BUILD_LLVM_PLUGIN)
  install(TARGETS LLVMVulkan LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/llvm-vulkan"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(FILES LICENSE NOTICE README.md CHANGELOG.md upstream-lock.json DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs")
install(DIRECTORY evidence/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/evidence")

set(CPACK_PACKAGE_VENDOR "HECer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Vulkan bulk-compute library and scoped LLVM passes")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_SOURCE_GENERATOR ZIP)
set(CPACK_SOURCE_PACKAGE_FILE_NAME "LLVMVulkan-${PROJECT_VERSION}-source")
set(CPACK_SOURCE_IGNORE_FILES
  "/[.]git/;/[.]omo/;/build[^/]*/;/results/;/reports/;/vendor/;/third_party/;/dist/;__pycache__;[.]pyc$;[.]env($|[.]);")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "LLVMVulkan-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
