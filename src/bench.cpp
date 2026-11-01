// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "lv/compute.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
static std::string quote(const std::string &s) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << unsigned(c);
    else
      out << c;
  }
  out << '"';
  return out.str();
}
static bool equal(const std::vector<lv::FoldResult> &a,
                  const std::vector<lv::FoldResult> &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].value != b[i].value || a[i].valid != b[i].valid)
      return false;
  return true;
}
static bool equal(const std::vector<lv::Fold64Result> &a,
                  const std::vector<lv::Fold64Result> &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].value != b[i].value || a[i].valid != b[i].valid ||
        a[i].reserved != b[i].reserved)
      return false;
  return true;
}
template <class T>
static bool equal(const std::vector<T> &a, const std::vector<T> &b) {
  return a == b;
}
template <class F, class R>
static std::string measure(const std::string &name, size_t items,
                           unsigned repeat, lv::Compute &engine, F run,
                           const R &reference) {
  // The first run includes lazy shader pipeline creation; subsequent runs use
  // the same device/pipelines but fresh upload/readback buffers, like the API.
  auto first = Clock::now();
  if (!equal(run(), reference))
    throw std::runtime_error(name + ": differential mismatch");
  double cold = elapsed(first);
  std::vector<double> samples;
  for (unsigned i = 0; i < repeat; ++i) {
    auto start = Clock::now();
    auto output = run();
    double ms = elapsed(start);
    if (!equal(output, reference))
      throw std::runtime_error(name + ": unstable output");
    samples.push_back(ms);
  }
  auto stats = engine.last_stats();
  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  double median = sorted[sorted.size() / 2];
  if (sorted.size() % 2 == 0)
    median = (median + sorted[sorted.size() / 2 - 1]) / 2;
  std::ostringstream out;
  out << std::setprecision(10) << "{\"name\":" << quote(name)
      << ",\"items\":" << items << ",\"correct\":true,\"cold_call_ms\":" << cold
      << ",\"warm_median_ms\":" << median
      << ",\"warm_min_ms\":" << sorted.front()
      << ",\"warm_max_ms\":" << sorted.back()
      << ",\"dispatches_per_call\":" << stats.dispatches
      << ",\"iterations_per_call\":" << stats.iterations
      << ",\"bytes_uploaded_per_call\":" << stats.bytes_uploaded
      << ",\"bytes_downloaded_per_call\":" << stats.bytes_downloaded
      << ",\"samples_ms\":[";
  for (size_t i = 0; i < samples.size(); ++i) {
    if (i)
      out << ',';
    out << samples[i];
  }
  out << "]}";
  return out.str();
}
static uint32_t number(const std::string &s, uint32_t maximum,
                       const char *name) {
  size_t pos = 0;
  auto n = std::stoull(s, &pos);
  if (pos != s.size() || n == 0 || n > maximum)
    throw std::invalid_argument(std::string("Invalid ") + name);
  return static_cast<uint32_t>(n);
}
int main(int argc, char **argv) {
  try {
    lv::Options options;
    uint32_t n = 65536, repeat = 5, seed = 5090, graph_nodes = 257;
    bool list = false;
    std::string json_path;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&]() {
        if (++i == argc)
          throw std::invalid_argument("Missing value for " + arg);
        return std::string(argv[i]);
      };
      if (arg == "--backend") {
        auto s = value();
        if (s == "cpu")
          options.backend = lv::Backend::Cpu;
        else if (s == "vulkan")
          options.backend = lv::Backend::Vulkan;
        else
          throw std::invalid_argument("--backend must be cpu or vulkan");
      } else if (arg == "--device") {
        auto s = value();
        if (s == "cpu")
          options.device = lv::DevicePreference::Cpu;
        else if (s == "gpu")
          options.device = lv::DevicePreference::Gpu;
        else if (s == "any")
          options.device = lv::DevicePreference::Any;
        else
          throw std::invalid_argument("--device must be any, cpu or gpu");
      } else if (arg == "--device-name")
        options.device_name = value();
      else if (arg == "--size")
        n = number(value(), 16777216, "size (max 16777216)");
      else if (arg == "--repeat")
        repeat = number(value(), 1000, "repeat (max 1000)");
      else if (arg == "--seed")
        seed = number(value(), UINT32_MAX, "seed");
      else if (arg == "--graph-nodes")
        graph_nodes = number(value(), 8192, "graph nodes (max 8192)");
      else if (arg == "--shader-dir")
        options.shader_dir = value();
      else if (arg == "--json")
        json_path = value();
      else if (arg == "--list-devices")
        list = true;
      else if (arg == "--help") {
        std::cout
            << "lv-bench [--backend cpu|vulkan] [--device any|cpu|gpu] "
               "[--device-name substring]\n"
            << "  [--size N] [--graph-nodes N] [--repeat N] [--seed N] [--json "
               "path]\n"
            << "  [--list-devices] [--shader-dir path]\n"
            << "JSON timings include host orchestration, transfers and "
               "readback.\n"
            << "CPU is a scalar reference, not an optimized LLVM baseline.\n";
        return 0;
      } else
        throw std::invalid_argument("Unknown option " + arg);
    }
    if (list) {
      std::cout << "[";
      auto devices = lv::Compute::devices();
      for (size_t i = 0; i < devices.size(); ++i) {
        if (i)
          std::cout << ',';
        const auto &d = devices[i];
        std::cout << "{\"name\":" << quote(d.name)
                  << ",\"is_cpu\":" << (d.is_cpu ? "true" : "false")
                  << ",\"vendor_id\":" << d.vendor_id
                  << ",\"vulkan_api_version_raw\":" << d.api_version
                  << ",\"driver_version_raw\":" << d.driver_version
                  << ",\"device_id\":" << d.device_id << ",\"supports_int64\":"
                  << (d.supports_int64 ? "true" : "false")
                  << ",\"max_storage_buffer_bytes\":"
                  << d.max_storage_buffer_bytes << "}";
      }
      std::cout << "]\n";
      return 0;
    }
    std::mt19937 rng(seed);
    std::vector<lv::FoldRequest> requests(n);
    std::vector<lv::Fold64Request> wide_requests(n);
    std::vector<uint32_t> keys(n), offsets(n + 1);
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t op = rng() % 23, width = rng() % 32 + 1;
      uint32_t a = rng(), b = rng();
      if (op >= 6 && op <= 8)
        b %= width;
      requests[i] = {op, width, a, b};
      keys[i] = rng();
      offsets[i] = i;
      uint32_t wide_op = rng() % 23, wide_width = rng() % 32 + 33;
      uint64_t wide_a = (uint64_t(rng()) << 32) | rng();
      uint64_t wide_b = (uint64_t(rng()) << 32) | rng();
      if (wide_op >= 6 && wide_op <= 8)
        wide_b %= wide_width;
      wide_requests[i] = {wide_op, wide_width, wide_a, wide_b};
    }
    offsets[n] = n;
    lv::Graph graph;
    std::vector<uint32_t> seeds(graph_nodes, 0), gen(graph_nodes * 2),
        kill(graph_nodes * 2);
    seeds[0] = 1;
    // A branched CFG with a long dependency chain and backward edges. Not an
    // embarrassingly parallel graph designed to conceal convergence cost.
    for (uint32_t i = 0; i < graph_nodes; ++i) {
      graph.offsets.push_back(static_cast<uint32_t>(graph.edges.size()));
      if (i + 1 < graph_nodes)
        graph.edges.push_back(i + 1);
      if (i + 5 < graph_nodes)
        graph.edges.push_back(i + 5);
      if (i > 10 && i % 7 == 0)
        graph.edges.push_back(i - 10);
      gen[2 * i] = 1u << (i % 32);
      gen[2 * i + 1] = 1u << ((i + 7) % 32);
      kill[2 * i] = rng();
      kill[2 * i + 1] = rng();
    }
    graph.offsets.push_back(static_cast<uint32_t>(graph.edges.size()));
    auto init = Clock::now();
    lv::Compute engine(options);
    double init_ms = elapsed(init);
    std::vector<std::string> rows;
    rows.push_back(measure(
        "integer-fold", n, repeat, engine,
        [&] { return engine.fold(requests); }, lv::cpu::fold(requests)));
    if (engine.device_info().supports_int64)
      rows.push_back(measure(
          "integer-fold64", n, repeat, engine,
          [&] { return engine.fold64(wide_requests); },
          lv::cpu::fold64(wide_requests)));
    else
      rows.push_back("{\"name\":\"integer-fold64\",\"skipped\":true,\"reason\":"
                     "\"device lacks shaderInt64\"}");
    rows.push_back(measure(
        "reachability", graph_nodes, repeat, engine,
        [&] { return engine.reachable(graph, seeds); },
        lv::cpu::reachable(graph, seeds)));
    rows.push_back(measure(
        "dominators", graph_nodes, repeat, engine,
        [&] { return engine.dominators(graph, 0); },
        lv::cpu::dominators(graph, 0)));
    rows.push_back(measure(
        "liveness", graph_nodes, repeat, engine,
        [&] { return engine.liveness(graph, 2, gen, kill); },
        lv::cpu::liveness(graph, 2, gen, kill)));
    rows.push_back(measure(
        "exclusive-scan", n, repeat, engine,
        [&] { return engine.exclusive_scan(keys); },
        lv::cpu::exclusive_scan(keys)));
    rows.push_back(measure(
        "sort-keys", n, repeat, engine, [&] { return engine.sort_keys(keys); },
        lv::cpu::sort_keys(keys)));
    rows.push_back(measure(
        "hash-records", n, repeat, engine,
        [&] { return engine.hash_records(keys, offsets); },
        lv::cpu::hash_records(keys, offsets)));
    std::ostringstream out;
    out << std::setprecision(10)
        << "{\n  \"schema_version\":1,\n  \"project\":\"llvm-vulkan\",\n  "
           "\"status\":\"experimental\",\n"
        << "  \"timing_scope\":\"host wall time including upload, dispatch, "
           "convergence and readback; input creation and independent "
           "verification excluded\",\n"
        << "  \"cpu_baseline\":\"scalar reference, not upstream LLVM "
           "optimization passes\",\n"
        << "  \"backend\":"
        << quote(options.backend == lv::Backend::Cpu ? "cpu" : "vulkan")
        << ",\n  \"device\":" << quote(engine.device_info().name)
        << ",\n  \"is_cpu_device\":"
        << (engine.device_info().is_cpu ? "true" : "false")
        << ",\n  \"supports_int64\":"
        << (engine.device_info().supports_int64 ? "true" : "false")
        << ",\n  \"vendor_id\":" << engine.device_info().vendor_id
        << ",\n  \"device_id\":" << engine.device_info().device_id
        << ",\n  \"vulkan_api_version_raw\":"
        << engine.device_info().api_version
        << ",\n  \"driver_version_raw\":" << engine.device_info().driver_version
        << ",\n  \"device_initialization_ms\":" << init_ms
        << ",\n  \"seed\":" << seed << ",\n  \"workloads\":[\n";
    for (size_t i = 0; i < rows.size(); ++i)
      out << "    " << rows[i] << (i + 1 == rows.size() ? "\n" : ",\n");
    out << "  ]\n}\n";
    if (!json_path.empty()) {
      std::ofstream file(json_path);
      if (!file)
        throw std::runtime_error("Cannot open report " + json_path);
      file << out.str();
      if (!file)
        throw std::runtime_error("Cannot write report " + json_path);
    }
    std::cout << out.str();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "lv-bench: " << e.what() << '\n';
    return 1;
  }
}
