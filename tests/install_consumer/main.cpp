// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <lv/compute.hpp>
#include <lv/version.hpp>
#include <iostream>
#include <stdexcept>

static_assert(LV_VERSION_MAJOR == 1, "consumer requires the version1 API");

int main() {
  try {
    for (auto backend : {lv::Backend::Cpu, lv::Backend::Vulkan}) {
      lv::Options options;
      options.backend = backend;
      lv::Compute compute(options);
      const auto folded = compute.fold({{uint32_t(lv::FoldOp::Add), 32, 5, 7}});
      if (folded.size() != 1 || !folded[0].valid || folded[0].value != 12 ||
          compute.exclusive_scan({4, 3, 2}) != std::vector<uint32_t>({0, 4, 7}) ||
          compute.sort_keys({9, 2, 7}) != std::vector<uint32_t>({2, 7, 9}))
        throw std::runtime_error("installed runtime result mismatch");
      if (backend == lv::Backend::Vulkan && compute.last_stats().backend != "vulkan")
        throw std::runtime_error("installed Vulkan backend was not used");
    }
    std::cout << "PASS relocated installed CPU/Vulkan consumer " << LV_VERSION_STRING << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
