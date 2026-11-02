// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lv {
enum class Backend { Cpu, Vulkan };
enum class DevicePreference { Any, Cpu, Gpu };
struct Options {
  Backend backend = Backend::Vulkan;
  DevicePreference device = DevicePreference::Any;
  // Empty uses embedded SPIR-V (or legacy LV_SHADER_DIR).
  std::string shader_dir;
  // Optional case-insensitive substring, in addition to device type.
  std::string device_name;
  uint64_t memory_limit_bytes = 512ull * 1024 * 1024;
  // Capability-gated shaders can be disabled to exercise the baseline paths.
  bool enable_fast_paths = true;
  // Timestamp support is queried; devices without it still execute normally.
  bool enable_timestamps = false;
};
enum class FoldOp : uint32_t {
  Add = 0,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  UDiv,
  SDiv,
  URem,
  SRem,
  Eq,
  Ne,
  Ult,
  Ule,
  Ugt,
  Uge,
  Slt,
  Sle,
  Sgt,
  Sge
};
// Modular integer arithmetic only. LLVM poison-generating flags must be handled
// by the caller. Invalid width/op, excessive shift, zero divisor or signed
// division/remainder overflow produce {0,0}; callers must not replace those
// instructions.
struct FoldRequest {
  uint32_t op, width, lhs, rhs;
};
struct FoldResult {
  uint32_t value, valid;
};
// Separate std430 scalar layouts; keeps the existing 32-bit ABI unchanged.
// fold64 accepts widths 1..64. Vulkan requires shaderInt64 for nonempty input.
struct alignas(8) Fold64Request {
  uint32_t op, width;
  uint64_t lhs, rhs;
};
struct alignas(8) Fold64Result {
  uint64_t value;
  uint32_t valid, reserved; // reserved is always zero
};
struct Graph {
  std::vector<uint32_t> offsets; // n+1 monotonically increasing; starts at zero
  std::vector<uint32_t> edges; // indices in [0,n); offsets.back()==edges.size()
  // Optional independent contiguous components: starts at 0, ends at n.
  // Edges must stay within a component. Enables one GPU workgroup per function.
  std::vector<uint32_t> components;
  size_t nodes() const { return offsets.empty() ? 0 : offsets.size() - 1; }
};
struct Stats {
  std::string backend;
  std::string device_name;
  uint64_t dispatches = 0;
  uint64_t iterations = 0;
  uint64_t bytes_uploaded = 0;
  uint64_t bytes_downloaded = 0;
  uint64_t submissions = 0;
  uint64_t memory_allocations = 0;
  uint64_t buffer_reuses = 0;
  uint64_t allocated_bytes = 0;
  // GPU-local solvers converge internally; iterations counts host-recorded
  // rounds.
  bool gpu_timestamps = false;
  double device_ms = 0;
  double fence_wait_ms = 0;
  double wall_ms = 0;
};
struct DeviceInfo {
  std::string name;
  bool is_cpu = false;
  uint32_t api_version = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint64_t max_storage_buffer_bytes = 0;
  bool supports_int64 = false;
  uint32_t driver_version = 0; // Raw Vulkan vendor-specific encoding.
  uint32_t subgroup_size = 0;
  bool supports_subgroup_arithmetic = false;
};

// Independent, deterministic host reference implementations. No Vulkan calls.
namespace cpu {
std::vector<FoldResult> fold(const std::vector<FoldRequest> &);
std::vector<Fold64Result> fold64(const std::vector<Fold64Request> &);
std::vector<uint32_t> reachable(const Graph &,
                                const std::vector<uint32_t> &seed_flags);
// Dense per-node bitset, n * ceil(n/32) words. Unreachable nodes have all-zero
// dominator sets; reachable nodes include themselves. Entry dominates itself.
std::vector<uint32_t> dominators(const Graph &, uint32_t entry);
// Backward may-analysis: in[B]=gen[B] | ((union in[succ]) & ~kill[B]).
// Inputs/result have n*words words. Empty graph/zero words returns empty.
std::vector<uint32_t> liveness(const Graph &, uint32_t words,
                               const std::vector<uint32_t> &gen,
                               const std::vector<uint32_t> &kill);
// Unsigned modular arithmetic: overflow wraps modulo 2^32.
std::vector<uint32_t> exclusive_scan(const std::vector<uint32_t> &);
std::vector<uint32_t> sort_keys(const std::vector<uint32_t> &);
// FNV-1a over each record's uint32 words, low byte first; offsets size
// records+1.
std::vector<uint32_t> hash_records(const std::vector<uint32_t> &words,
                                   const std::vector<uint32_t> &offsets);
void validate_graph(const Graph &);
} // namespace cpu

// One instance is deliberately not thread-safe. Each compiler worker must own
// an instance or serialize calls. A selected Vulkan backend never falls back to
// host algorithms. Construction/dispatch errors are exceptions.
class Compute {
public:
  explicit Compute(Options options = {});
  ~Compute();
  Compute(Compute &&) noexcept;
  Compute &operator=(Compute &&) noexcept;
  Compute(const Compute &) = delete;
  Compute &operator=(const Compute &) = delete;
  static std::vector<DeviceInfo> devices();
  const DeviceInfo &device_info() const;
  const Stats &last_stats() const;
  std::vector<FoldResult> fold(const std::vector<FoldRequest> &);
  std::vector<Fold64Result> fold64(const std::vector<Fold64Request> &);
  std::vector<uint32_t> reachable(const Graph &,
                                  const std::vector<uint32_t> &seed_flags);
  std::vector<uint32_t> dominators(const Graph &, uint32_t entry);
  std::vector<uint32_t> liveness(const Graph &, uint32_t words,
                                 const std::vector<uint32_t> &gen,
                                 const std::vector<uint32_t> &kill);
  std::vector<uint32_t> exclusive_scan(const std::vector<uint32_t> &);
  std::vector<uint32_t> sort_keys(const std::vector<uint32_t> &);
  std::vector<uint32_t> hash_records(const std::vector<uint32_t> &words,
                                     const std::vector<uint32_t> &offsets);

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
} // namespace lv
