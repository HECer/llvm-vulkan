// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Isolated allocator fault injection: fail only the warmed result allocation.
#include "lv/compute.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<size_t> fail_bytes{0};
const std::thread::id test_thread = std::this_thread::get_id();
lv::Compute *active_compute = nullptr;
} // namespace
void *operator new(size_t bytes) {
  size_t expected = fail_bytes.load();
  // MSVC's vector allocator adds up to 39 bytes for large-block alignment.
  // Restrict the injection to our caller thread and the post-submit phase.
  if (expected && bytes >= expected && bytes - expected <= 64 &&
      std::this_thread::get_id() == test_thread && active_compute &&
      active_compute->last_stats().submissions == 1 &&
      fail_bytes.compare_exchange_strong(expected, 0)) {
    throw std::bad_alloc();
  }
  if (void *p = std::malloc(bytes ? bytes : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

int main() {
  try {
    lv::Compute compute;
    std::vector<lv::FoldRequest> requests(
        16384, {uint32_t(lv::FoldOp::Add), 32, 5, 7});
    compute.fold(requests);
    active_compute = &compute;
    fail_bytes = requests.size() * sizeof(lv::FoldResult);
    bool failed = false;
    try {
      compute.fold(requests);
    } catch (const std::bad_alloc &) {
      failed = true;
    }
    fail_bytes = 0;
    if (!failed || compute.last_stats().submissions != 1)
      throw std::runtime_error(
          "result allocation did not fail after submission");
    auto result = compute.fold(requests);
    for (const auto &r : result)
      if (!r.valid || r.value != 12)
        throw std::runtime_error("recovery result mismatch");
    if (compute.last_stats().submissions != 1 ||
        compute.last_stats().memory_allocations)
      throw std::runtime_error("recovery did not reuse completed buffers");
    std::cout
        << "PASS allocation failure joins submitted work before pooled reuse\n";
    return 0;
  } catch (const std::exception &e) {
    fail_bytes = 0;
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
