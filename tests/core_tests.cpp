// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "lv/compute.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
size_t checks = 0;
void require(bool ok, const std::string &what) {
  ++checks;
  if (!ok)
    throw std::runtime_error(what);
}
template <class T>
void expect_equal(const std::vector<T> &a, const std::vector<T> &b,
                  const std::string &what) {
  require(a == b, what + " mismatch");
}
template <class F> void rejects(F f, const std::string &what) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception &) {
    caught = true;
  }
  require(caught, "did not reject " + what);
}
lv::Graph graph(const std::vector<std::vector<uint32_t>> &adj) {
  lv::Graph g;
  g.offsets = {0};
  for (auto &es : adj) {
    g.edges.insert(g.edges.end(), es.begin(), es.end());
    g.offsets.push_back(uint32_t(g.edges.size()));
  }
  return g;
}
void fold_tests(lv::Compute &c, std::mt19937 &rng) {
  using O = lv::FoldOp;
  std::vector<lv::FoldRequest> in;
  for (uint32_t w = 1; w <= 5; ++w)
    for (uint32_t a = 0; a < (1u << w); ++a)
      for (uint32_t b = 0; b < (1u << w); ++b)
        for (uint32_t op = 0; op <= uint32_t(O::Sge); ++op)
          in.push_back({op, w, a, b});
  for (uint32_t w = 1; w <= 32; ++w) {
    uint32_t edge[] = {
        0,  1,           2,           31,          32,
        33, 0x7fffffffu, 0x80000000u, 0xffffffffu, 1u << (w - 1)};
    for (auto a : edge)
      for (auto b : edge)
        for (uint32_t op = 0; op <= uint32_t(O::Sge); ++op)
          in.push_back({op, w, a, b});
  }
  for (uint32_t i = 0; i < 20000; ++i)
    in.push_back({uint32_t(rng() % 25u), uint32_t(rng() % 35u), uint32_t(rng()),
                  uint32_t(rng())});
  auto expected = lv::cpu::fold(in), actual = c.fold(in);
  require(expected.size() == actual.size(), "fold length");
  for (size_t i = 0; i < in.size(); ++i)
    require(expected[i].value == actual[i].value &&
                expected[i].valid == actual[i].valid,
            "fold request " + std::to_string(i));
  std::vector<lv::FoldRequest> special = {
      {uint32_t(O::Add), 8, 255, 1},
      {uint32_t(O::SRem), 32, 0x80000000u, 0xffffffffu},
      {uint32_t(O::SDiv), 32, 0x80000000u, 0xffffffffu},
      {uint32_t(O::AShr), 8, 128, 7},
      {uint32_t(O::LShr), 8, 128, 7},
      {uint32_t(O::Shl), 8, 1, 8},
      {uint32_t(O::Slt), 8, 255, 0},
      {uint32_t(O::SRem), 8, 251, 3},
      {uint32_t(O::SDiv), 8, 251, 3}};
  auto r = c.fold(special);
  require(r[0].valid && r[0].value == 0, "8-bit addition wraps");
  require(!r[1].valid, "LLVM signed remainder overflow rejected");
  require(!r[2].valid, "signed division overflow rejected");
  require(r[3].valid && r[3].value == 255, "arithmetic shift sign extends");
  require(r[4].valid && r[4].value == 1, "logical shift zero extends");
  require(!r[5].valid, "oversized shift rejected");
  require(r[6].valid && r[6].value == 1, "signed comparison interprets width");
  require(r[7].valid && r[7].value == 254,
          "signed remainder keeps dividend sign");
  require(r[8].valid && r[8].value == 255,
          "signed division truncates toward zero");
  require(c.fold({}).empty(), "empty fold");
  std::cout << "PASS integer folding: " << in.size()
            << " differential tasks and semantic edges\n";
}
void fold64_tests(lv::Compute &c, std::mt19937 &rng) {
  using O = lv::FoldOp;
  require(c.fold64({}).empty(),
          "empty fold64 requires no optional device feature");
  if (!c.device_info().supports_int64) {
    rejects([&] { c.fold64({{uint32_t(O::Add), 64, 1, 2}}); },
            "unsupported shaderInt64 explicit request");
    std::cout << "SKIP fold64 differential suite: selected Vulkan device lacks "
                 "optional shaderInt64\n";
    return;
  }
  std::vector<lv::Fold64Request> in;
  for (uint32_t w = 1; w <= 64; ++w) {
    uint64_t mask = w == 64 ? UINT64_MAX : (uint64_t(1) << w) - 1;
    uint64_t edge[] = {
        0, 1, 2, 31, 32, 63, 64, UINT64_MAX, uint64_t(1) << (w - 1), mask >> 1};
    for (auto a : edge)
      for (auto b : edge)
        for (uint32_t op = 0; op <= uint32_t(O::Sge); ++op)
          in.push_back({op, w, a, b});
  }
  std::mt19937_64 wide(rng());
  for (uint32_t i = 0; i < 20000; ++i)
    in.push_back(
        {uint32_t(wide() % 25), uint32_t(wide() % 67), wide(), wide()});
  const auto reference = lv::cpu::fold64(in), actual = c.fold64(in);
  require(reference.size() == actual.size(), "fold64 length");
  for (size_t i = 0; i < in.size(); ++i)
    require(reference[i].value == actual[i].value &&
                reference[i].valid == actual[i].valid &&
                actual[i].reserved == 0,
            "fold64 request " + std::to_string(i));
  // The independent i32 implementation must agree on their shared domain.
  std::vector<lv::FoldRequest> narrow;
  std::vector<lv::Fold64Request> shared;
  for (uint32_t i = 0; i < 4000; ++i) {
    uint32_t w = 1 + rng() % 32, op = rng() % 23, a = rng(), b = rng();
    narrow.push_back({op, w, a, b});
    shared.push_back({op, w, a, b});
  }
  const auto nr = lv::cpu::fold(narrow);
  const auto wr = c.fold64(shared);
  for (size_t i = 0; i < nr.size(); ++i)
    require(nr[i].value == wr[i].value && nr[i].valid == wr[i].valid,
            "independent fold32/fold64 overlap " + std::to_string(i));
  const uint64_t sign = uint64_t(1) << 63;
  auto r = c.fold64({{uint32_t(O::Add), 64, UINT64_MAX, 1},
                     {uint32_t(O::SDiv), 64, sign, UINT64_MAX},
                     {uint32_t(O::SRem), 64, sign, UINT64_MAX},
                     {uint32_t(O::AShr), 64, sign, 63},
                     {uint32_t(O::LShr), 64, sign, 63},
                     {uint32_t(O::Shl), 64, 1, 64},
                     {uint32_t(O::SDiv), 64, UINT64_MAX - 4, 3},
                     {uint32_t(O::SRem), 64, UINT64_MAX - 4, 3},
                     {uint32_t(O::Sub), 33, 0, 1},
                     {uint32_t(O::URem), 64, UINT64_MAX, 2},
                     {uint32_t(O::Slt), 64, sign, sign - 1},
                     {uint32_t(O::UDiv), 64, 1, 0}});
  require(r[0].valid && r[0].value == 0, "64-bit addition wraps");
  require(!r[1].valid && !r[2].valid,
          "64-bit signed division/remainder overflow rejected");
  require(r[3].valid && r[3].value == UINT64_MAX,
          "64-bit arithmetic shift sign extends");
  require(r[4].valid && r[4].value == 1, "64-bit logical shift zero extends");
  require(!r[5].valid, "64-bit excessive shift rejected");
  require(r[6].valid && r[6].value == UINT64_MAX,
          "64-bit signed division truncates");
  require(r[7].valid && r[7].value == UINT64_MAX - 1,
          "64-bit signed remainder dividend sign");
  require(r[8].valid && r[8].value == 0x1ffffffffull, "i33 width mask");
  require(r[9].valid && r[9].value == 1, "64-bit unsigned remainder");
  require(r[10].valid && r[10].value == 1, "64-bit signed ordering");
  require(!r[11].valid, "64-bit zero divisor rejected");
  std::cout << "PASS 64-bit integer folding: " << in.size()
            << " differential tasks + 4000 independent narrow comparisons\n";
}
void graph_tests(lv::Compute &c, std::mt19937 &rng, uint32_t cases) {
  auto diamond = graph({{1, 2}, {3}, {3}, {}, {4}});
  expect_equal(c.reachable(diamond, {1, 0, 0, 0, 0}),
               std::vector<uint32_t>{1, 1, 1, 1, 0}, "diamond reachability");
  expect_equal(c.dominators(diamond, 0), std::vector<uint32_t>{1, 3, 5, 9, 0},
               "diamond dominators");
  auto cycle = graph({{1}, {2}, {0}, {}});
  expect_equal(c.reachable(cycle, {0, 1, 0, 0}),
               std::vector<uint32_t>{1, 1, 1, 0}, "cycle reachability");
  expect_equal(c.liveness(cycle, 1, {0, 0, 1, 2}, {0, 0, 0, 0}),
               std::vector<uint32_t>{1, 1, 1, 2}, "cycle liveness");
  auto chain = graph({{1}, {2}, {3}, {}});
  expect_equal(c.liveness(chain, 1, {0, 0, 0, 3}, {0, 1, 0, 0}),
               std::vector<uint32_t>{2, 2, 3, 3}, "liveness kills");
  for (uint32_t k = 0; k < cases; ++k) {
    uint32_t n = rng() % 97;
    std::vector<std::vector<uint32_t>> adj(n);
    for (uint32_t v = 0; v < n; ++v)
      for (uint32_t j = 0, m = rng() % 9; j < m; ++j)
        adj[v].push_back(rng() % n);
    auto g = graph(adj);
    std::vector<uint32_t> seeds(n, 0);
    for (auto &x : seeds)
      x = (rng() % 13) == 0 ? 2 : 0;
    expect_equal(c.reachable(g, seeds), lv::cpu::reachable(g, seeds),
                 "random reachability " + std::to_string(k));
    uint32_t entry = n ? rng() % n : 0;
    expect_equal(c.dominators(g, entry), lv::cpu::dominators(g, entry),
                 "random dominators " + std::to_string(k));
    uint32_t words = rng() % 5;
    std::vector<uint32_t> gen(size_t(n) * words), kill(gen.size());
    for (size_t i = 0; i < gen.size(); ++i) {
      gen[i] = rng() & rng() & rng();
      kill[i] = rng() & rng();
    }
    expect_equal(c.liveness(g, words, gen, kill),
                 lv::cpu::liveness(g, words, gen, kill),
                 "random liveness " + std::to_string(k));
  }
  // Long diameter exercises convergence beyond the eight-round batch boundary.
  std::vector<std::vector<uint32_t>> adj(257);
  for (uint32_t i = 0; i + 1 < adj.size(); ++i)
    adj[i].push_back(i + 1);
  auto g = graph(adj);
  std::vector<uint32_t> seeds(adj.size(), 0);
  seeds[0] = 1;
  expect_equal(c.reachable(g, seeds), std::vector<uint32_t>(adj.size(), 1),
               "long-chain reachability");
  expect_equal(c.dominators(g, 0), lv::cpu::dominators(g, 0),
               "long-chain dominators");
  std::vector<uint32_t> gen(adj.size(), 0), kill(adj.size(), 0);
  gen.back() = 0x12345678u;
  expect_equal(c.liveness(g, 1, gen, kill),
               std::vector<uint32_t>(adj.size(), 0x12345678u),
               "long-chain liveness");
  std::cout << "PASS CSR reachability, dominators, liveness: " << cases
            << " randomized graphs and cyclic/disconnected/long-chain cases\n";
}
void primitive_tests(lv::Compute &c, std::mt19937 &rng) {
  for (uint32_t n : {0u, 1u, 2u, 3u, 31u, 32u, 33u, 127u, 128u, 129u, 255u,
                     256u, 257u, 1023u, 1024u, 1025u, 16385u}) {
    std::vector<uint32_t> in(n);
    for (auto &x : in)
      x = rng();
    expect_equal(c.exclusive_scan(in), lv::cpu::exclusive_scan(in),
                 "scan " + std::to_string(n));
    expect_equal(c.sort_keys(in), lv::cpu::sort_keys(in),
                 "sort " + std::to_string(n));
  }
  expect_equal(c.sort_keys({UINT32_MAX, 0, UINT32_MAX, 1, 1}),
               std::vector<uint32_t>{0, 1, 1, UINT32_MAX, UINT32_MAX},
               "sort padded sentinels");
  expect_equal(c.exclusive_scan({UINT32_MAX, 1, 2}),
               std::vector<uint32_t>{0, UINT32_MAX, 0}, "scan wraps");
  std::vector<uint32_t> words, offsets = {0};
  for (uint32_t i = 0; i < 2000; ++i) {
    for (uint32_t j = 0, n = rng() % 27; j < n; ++j)
      words.push_back(rng());
    offsets.push_back(uint32_t(words.size()));
  }
  expect_equal(c.hash_records(words, offsets),
               lv::cpu::hash_records(words, offsets), "ragged hashes");
  expect_equal(c.hash_records({}, {0, 0, 0}),
               std::vector<uint32_t>{2166136261u, 2166136261u},
               "empty record hashes");
  expect_equal(c.hash_records({0x04030201u, 0}, {0, 1, 2}),
               std::vector<uint32_t>{0x5734a87du, 0x4b95f515u},
               "FNV-1a byte order known vectors");
  require(c.hash_records({}, {}).empty(), "no hash records");
  std::cout << "PASS exclusive scan, unsigned sort, ragged record hashing\n";
}
void tiled_dispatch_tests(lv::Compute &c) {
  // Exceeds the commonly exposed Vulkan x-dimension limit of 65,535 groups.
  // Opt-in because this case intentionally allocates hundreds of MiB.
  const uint32_t n = 65535u * 128u + 129u;
  std::vector<uint32_t> input(n);
  for (uint32_t i = 0; i < n; ++i)
    input[i] = (i * 2654435761u) ^ (i >> 7);
  expect_equal(c.exclusive_scan(input), lv::cpu::exclusive_scan(input),
               "tiled hierarchical scan");
  input.clear();
  input.shrink_to_fit();
  lv::Graph g;
  g.offsets.assign(size_t(n) + 1, 1);
  g.offsets[0] = 0;
  g.edges = {n - 1};
  std::vector<uint32_t> seeds(n, 0);
  seeds[0] = 1;
  auto result = c.reachable(g, seeds);
  require(result[0] == 1 && result.back() == 1,
          "reachability crosses dispatch tile boundary");
  require(std::count(result.begin(), result.end(), 1u) == 2,
          "tiled reachability exact closure");
  std::cout << "PASS large dispatch-grid tiling at " << n
            << " elements/nodes\n";
}
void invalid_tests(lv::Compute &c) {
  rejects([&] { c.reachable({{1, 1}, {}}, {0}); }, "nonzero CSR start");
  rejects([&] { c.reachable({{0, 1}, {1}}, {1}); }, "out-of-range edge");
  rejects([&] { c.reachable({{0, 2, 1}, {0}}, {1, 0}); },
          "nonmonotone CSR offsets");
  rejects([&] { c.reachable({{0, 0}, {}}, {}); }, "wrong seed count");
  rejects([&] { c.dominators({{0, 0}, {}}, 1); }, "invalid dominator entry");
  rejects([&] { c.liveness({{0, 0}, {}}, 2, {0}, {0}); },
          "wrong bitset dimensions");
  rejects([&] { c.hash_records({1}, {0, 5, 1}); },
          "hash offset overrun before descent");
  rejects([&] { c.hash_records({1}, {}); }, "missing hash offsets");
  if (c.last_stats().backend == "vulkan") {
    lv::Graph huge;
    huge.offsets.assign(100001, 0);
    rejects([&] { c.dominators(huge, 0); },
            "oversized dense dominator matrix before allocation");
  }
  std::cout << "PASS invalid input rejection\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    lv::Options options;
    uint32_t cases = 48, seed = 5090;
    bool large = false;
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i], value;
      auto eq = key.find('=');
      if (eq != std::string::npos) {
        value = key.substr(eq + 1);
        key.resize(eq);
      } else if (i + 1 < argc)
        value = argv[++i];
      else
        throw std::runtime_error("missing option value");
      if (key == "--backend") {
        if (value == "cpu")
          options.backend = lv::Backend::Cpu;
        else if (value == "vulkan")
          options.backend = lv::Backend::Vulkan;
        else
          throw std::runtime_error("unknown backend");
      } else if (key == "--device") {
        if (value == "cpu")
          options.device = lv::DevicePreference::Cpu;
        else if (value == "gpu")
          options.device = lv::DevicePreference::Gpu;
        else if (value == "any")
          options.device = lv::DevicePreference::Any;
        else
          throw std::runtime_error("unknown device");
      } else if (key == "--device-name")
        options.device_name = value;
      else if (key == "--shader-dir")
        options.shader_dir = value;
      else if (key == "--large") {
        if (value != "0" && value != "1")
          throw std::runtime_error("--large must be 0 or 1");
        large = value == "1";
      } else if (key == "--cases")
        cases = uint32_t(std::stoul(value));
      else if (key == "--seed")
        seed = uint32_t(std::stoul(value));
      else
        throw std::runtime_error("unknown option " + key);
    }
    lv::Compute compute(options);
    std::mt19937 rng(seed);
    std::cout << "Device: " << compute.device_info().name << " | backend="
              << (options.backend == lv::Backend::Cpu ? "cpu" : "vulkan")
              << " | seed=" << seed << '\n';
    if (options.backend == lv::Backend::Vulkan &&
        options.device == lv::DevicePreference::Cpu)
      require(compute.device_info().is_cpu, "requested Vulkan CPU device");
    if (options.backend == lv::Backend::Vulkan &&
        options.device == lv::DevicePreference::Gpu)
      require(!compute.device_info().is_cpu, "requested real GPU device");
    fold_tests(compute, rng);
    fold64_tests(compute, rng);
    graph_tests(compute, rng, cases);
    primitive_tests(compute, rng);
    if (large)
      tiled_dispatch_tests(compute);
    invalid_tests(compute);
    require(compute.last_stats().backend ==
                (options.backend == lv::Backend::Cpu ? "cpu" : "vulkan"),
            "backend provenance");
    std::cout << "PASS all " << checks
              << " assertions; hardware performance is not inferred from these "
                 "correctness tests\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
