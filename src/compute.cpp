// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "lv/compute.hpp"
#ifdef LV_EMBEDDED_SHADERS
#include "lv/embedded_shaders.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vulkan/vulkan.h>
#ifndef LV_SHADER_DIR
#define LV_SHADER_DIR "shaders"
#endif

namespace lv {
static_assert(sizeof(FoldRequest) == 16,
              "FoldRequest must match std430 uvec4 layout");
static_assert(sizeof(FoldResult) == 8,
              "FoldResult must match std430 uvec2 layout");
static_assert(sizeof(Fold64Request) == 24 && alignof(Fold64Request) == 8 &&
                  offsetof(Fold64Request, lhs) == 8 &&
                  offsetof(Fold64Request, rhs) == 16,
              "Fold64Request must match std430 scalar layout");
static_assert(sizeof(Fold64Result) == 16 && alignof(Fold64Result) == 8 &&
                  offsetof(Fold64Result, valid) == 8 &&
                  offsetof(Fold64Result, reserved) == 12,
              "Fold64Result must match std430 scalar layout");
namespace {
void check(VkResult r, const char *what) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed with VkResult " +
                             std::to_string(r));
}
std::string lower(std::string s) {
  for (char &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
uint32_t checked_count(uint64_t n, const char *what) {
  if (n > UINT32_MAX)
    throw std::length_error(std::string(what) + " exceeds 32-bit indexing");
  return uint32_t(n);
}
VkInstance new_instance(bool validation, void *user,
                        PFN_vkDebugUtilsMessengerCallbackEXT callback) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "LLVM Vulkan experimental";
  app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app.pEngineName = "lv";
  app.apiVersion = VK_API_VERSION_1_1;
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkDebugUtilsMessengerCreateInfoEXT debug{};
  debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  debug.pfnUserCallback = callback;
  debug.pUserData = user;
  if (validation) {
    uint32_t n = 0;
    check(vkEnumerateInstanceLayerProperties(&n, nullptr), "enumerate layers");
    std::vector<VkLayerProperties> ls(n);
    check(vkEnumerateInstanceLayerProperties(&n, ls.data()),
          "enumerate layers");
    bool found = false;
    for (auto &l : ls)
      if (std::strcmp(l.layerName, layer) == 0)
        found = true;
    if (!found)
      throw std::runtime_error(
          "LV_VALIDATION=1 requires VK_LAYER_KHRONOS_validation");
  }
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  if (validation) {
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = &layer;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &extension;
    ci.pNext = &debug;
  }
  VkInstance instance = VK_NULL_HANDLE;
  check(vkCreateInstance(&ci, nullptr, &instance), "create Vulkan instance");
  return instance;
}
DeviceInfo info(VkPhysicalDevice p) {
  VkPhysicalDeviceProperties d{};
  vkGetPhysicalDeviceProperties(p, &d);
  VkPhysicalDeviceFeatures f{};
  vkGetPhysicalDeviceFeatures(p, &f);
  DeviceInfo result{d.deviceName,
                    d.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU,
                    d.apiVersion,
                    d.vendorID,
                    d.deviceID,
                    d.limits.maxStorageBufferRange,
                    f.shaderInt64 == VK_TRUE,
                    d.driverVersion};
  if (d.apiVersion >= VK_API_VERSION_1_1) {
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(p, &properties);
    result.subgroup_size = subgroup.subgroupSize;
    result.supports_subgroup_arithmetic =
        (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
        (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) &&
        (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);
  }
  return result;
}
std::vector<VkPhysicalDevice> physicals(VkInstance i) {
  uint32_t n = 0;
  check(vkEnumeratePhysicalDevices(i, &n, nullptr), "enumerate Vulkan devices");
  std::vector<VkPhysicalDevice> ds(n);
  check(vkEnumeratePhysicalDevices(i, &n, ds.data()),
        "enumerate Vulkan devices");
  return ds;
}
Graph reverse_graph(const Graph &g) {
  Graph out;
  out.offsets.assign(g.nodes() + 1, 0);
  out.edges.resize(g.edges.size());
  for (auto v : g.edges)
    ++out.offsets[size_t(v) + 1];
  for (size_t v = 1; v < out.offsets.size(); ++v)
    out.offsets[v] += out.offsets[v - 1];
  auto pos = out.offsets;
  for (uint32_t v = 0; v < g.nodes(); ++v)
    for (uint32_t e = g.offsets[v]; e < g.offsets[v + 1]; ++e)
      out.edges[pos[g.edges[e]]++] = v;
  return out;
}
} // namespace

struct Compute::Impl {
  Options options;
  DeviceInfo device_info;
  Stats stats;
  std::chrono::steady_clock::time_point started;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  std::atomic<uint32_t> validation_errors{0};
  uint64_t allocated_bytes = 0;
  bool recording = false;
  bool memory_budget_supported = false;
  uint32_t timestamp_bits = 0;
  VkQueryPool timestamp_pool = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
  struct Pipeline {
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  std::map<std::string, Pipeline> pipelines;
  // Allocations outlive API calls. A lease only holds a logical buffer range;
  // recorded marks protect resources until their command buffer has completed.
  struct Allocation {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0, allocation = 0;
    VkMemoryPropertyFlags flags = 0;
    uint64_t *accounting = nullptr;
    void *mapped = nullptr;
    unsigned role = 0;
    bool leased = false, recorded = false;
    ~Allocation() {
      if (mapped)
        vkUnmapMemory(device, memory);
      if (buffer)
        vkDestroyBuffer(device, buffer, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
      if (accounting)
        *accounting -= allocation;
    }
  };
  std::vector<std::unique_ptr<Allocation>> allocations;
  struct Buffer {
    Allocation *resource = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    VkMemoryPropertyFlags flags = 0;
    void *mapped = nullptr;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&b) noexcept { *this = std::move(b); }
    Buffer &operator=(Buffer &&b) noexcept {
      if (this != &b) {
        clear();
        resource = b.resource;
        buffer = b.buffer;
        memory = b.memory;
        bytes = b.bytes;
        flags = b.flags;
        mapped = b.mapped;
        b.resource = nullptr;
        b.buffer = VK_NULL_HANDLE;
        b.memory = VK_NULL_HANDLE;
      }
      return *this;
    }
    ~Buffer() { clear(); }
    void clear() {
      if (resource)
        resource->leased = false;
      resource = nullptr;
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
    }
  };
  std::unique_ptr<Buffer> dummy;
  static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
      VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    static_cast<Impl *>(user)->validation_errors.fetch_add(
        1, std::memory_order_relaxed);
    std::cerr << "[lv Vulkan validation] "
              << (d && d->pMessage ? d->pMessage : "unknown error") << '\n';
    return VK_FALSE;
  }
  explicit Impl(Options o) : options(std::move(o)) {
    if (options.backend == Backend::Cpu) {
      device_info.name = "C++ scalar reference";
      device_info.is_cpu = true;
      device_info.supports_int64 = true;
      return;
    }
    try {
      init();
    } catch (...) {
      cleanup();
      throw;
    }
  }
  ~Impl() { cleanup(); }
  void cleanup() {
    if (device)
      vkDeviceWaitIdle(device);
    dummy.reset();
    allocations.clear();
    if (timestamp_pool)
      vkDestroyQueryPool(device, timestamp_pool, nullptr);
    for (auto &p : pipelines) {
      if (p.second.pipeline)
        vkDestroyPipeline(device, p.second.pipeline, nullptr);
      if (p.second.shader)
        vkDestroyShaderModule(device, p.second.shader, nullptr);
    }
    pipelines.clear();
    if (pipeline_cache)
      vkDestroyPipelineCache(device, pipeline_cache, nullptr);
    if (descriptor_pool)
      vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    if (pipeline_layout)
      vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout)
      vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (fence)
      vkDestroyFence(device, fence, nullptr);
    if (command_pool)
      vkDestroyCommandPool(device, command_pool, nullptr);
    if (device)
      vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    if (messenger && instance) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy)
        destroy(instance, messenger, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
  }
  void init() {
    const char *validation_env = std::getenv("LV_VALIDATION");
    bool validation = validation_env && std::string(validation_env) == "1";
    instance = new_instance(validation, this, debug_callback);
    if (validation) {
      auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      if (!create)
        throw std::runtime_error("Vulkan debug utils unavailable");
      VkDebugUtilsMessengerCreateInfoEXT ci{};
      ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      ci.pfnUserCallback = debug_callback;
      ci.pUserData = this;
      check(create(instance, &ci, nullptr, &messenger),
            "create validation messenger");
    }
    int best = -1;
    for (auto d : physicals(instance)) {
      VkPhysicalDeviceProperties prop{};
      vkGetPhysicalDeviceProperties(d, &prop);
      bool cpu = prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
      if (options.device == DevicePreference::Cpu && !cpu)
        continue;
      if (options.device == DevicePreference::Gpu &&
          (cpu || (prop.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                   prop.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
                   prop.deviceType != VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)))
        continue;
      if (!options.device_name.empty() &&
          lower(prop.deviceName).find(lower(options.device_name)) ==
              std::string::npos)
        continue;
      if (prop.apiVersion < VK_API_VERSION_1_1)
        continue;
      uint32_t n = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
      std::vector<VkQueueFamilyProperties> qs(n);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &n, qs.data());
      for (uint32_t q = 0; q < n; ++q)
        if (qs[q].queueCount && (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
          int score = prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                          ? 300
                      : cpu ? 100
                            : 200;
          if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            score += 10;
          if (score > best) {
            best = score;
            physical = d;
            queue_family = q;
            properties = prop;
            timestamp_bits = qs[q].timestampValidBits;
          }
        }
    }
    if (!physical)
      throw std::runtime_error("no Vulkan 1.1 compute device matches requested "
                               "type/name; no CPU fallback performed");
    if (properties.limits.maxPerStageDescriptorStorageBuffers < 8 ||
        properties.limits.maxDescriptorSetStorageBuffers < 8 ||
        properties.limits.maxComputeWorkGroupInvocations < 128 ||
        properties.limits.maxComputeWorkGroupSize[0] < 128 ||
        properties.limits.maxPushConstantsSize < 32)
      throw std::runtime_error(
          "Vulkan device lacks required compute/storage limits");
    device_info = info(physical);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical, &available);
    VkPhysicalDeviceFeatures enabled{};
    enabled.robustBufferAccess = available.robustBufferAccess;
    enabled.shaderInt64 = available.shaderInt64;
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.pEnabledFeatures = &enabled;
    uint32_t extension_count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr,
                                               &extension_count, nullptr),
          "enumerate device extensions");
    std::vector<VkExtensionProperties> extensions(extension_count);
    check(vkEnumerateDeviceExtensionProperties(
              physical, nullptr, &extension_count, extensions.data()),
          "enumerate device extensions");
    for (const auto &e : extensions)
      if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) ==
          0)
        memory_budget_supported = true;
    const char *budget_extension = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    if (memory_budget_supported) {
      ci.enabledExtensionCount = 1;
      ci.ppEnabledExtensionNames = &budget_extension;
    }
    check(vkCreateDevice(physical, &ci, nullptr, &device),
          "create Vulkan device");
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(device, &pool, nullptr, &command_pool),
          "create command pool");
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = command_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &ca, &command),
          "allocate command buffer");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(vkCreateFence(device, &fi, nullptr, &fence), "create fence");
    VkPipelineCacheCreateInfo pc{};
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    check(vkCreatePipelineCache(device, &pc, nullptr, &pipeline_cache),
          "create pipeline cache");
    if (options.enable_timestamps && timestamp_bits &&
        properties.limits.timestampPeriod > 0) {
      VkQueryPoolCreateInfo qp{};
      qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qp.queryCount = 2;
      check(vkCreateQueryPool(device, &qp, nullptr, &timestamp_pool),
            "create timestamp pool");
    }
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = uint32_t(bindings.size());
    sl.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout),
          "create descriptor layout");
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = 32;
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &pl, nullptr, &pipeline_layout),
          "create pipeline layout");
    VkDescriptorPoolSize ds{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1024;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ds;
    check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptor_pool),
          "create descriptor pool");
    dummy = std::make_unique<Buffer>(make_buffer(4, false));
    uint32_t zero = 0;
    upload({{dummy.get(), &zero, 4}});
    submit();
  }
  void preflight_storage(uint64_t bytes) const {
    if (std::max<uint64_t>(bytes, 4) > properties.limits.maxStorageBufferRange)
      throw std::length_error(
          "storage buffer exceeds Vulkan maxStorageBufferRange");
  }
  void trim_idle() {
    allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
                                     [](const auto &a) {
                                       return !a->leased && !a->recorded;
                                     }),
                      allocations.end());
  }
  void preflight_budget(uint64_t minimum_bytes) {
    if (minimum_bytes > options.memory_limit_bytes)
      throw std::length_error(
          "configured Vulkan memory budget exceeded before host preparation");
    if (allocated_bytes > options.memory_limit_bytes - minimum_bytes)
      trim_idle();
    if (allocated_bytes > options.memory_limit_bytes - minimum_bytes)
      throw std::length_error(
          "configured Vulkan memory budget exceeded before host preparation");
  }
  void preflight_working_set(uint64_t minimum_bytes) const {
    // Idle pool entries may satisfy this operation. Counting them as additional
    // live memory would evict useful buffers on every large repeated call.
    uint64_t live = 0;
    for (const auto &a : allocations)
      if (a->leased || a->recorded)
        live += a->allocation;
    if (minimum_bytes > options.memory_limit_bytes ||
        live > options.memory_limit_bytes - minimum_bytes)
      throw std::length_error(
          "configured Vulkan working set exceeds memory budget");
  }
  Buffer make_buffer(uint64_t n, bool host, bool readback = false) {
    n = std::max<uint64_t>(n, 4);
    if (!host)
      preflight_storage(n);
    const unsigned role = host ? (readback ? 2u : 1u) : 0u;
    Allocation *selected = nullptr;
    for (auto &a : allocations)
      if (!a->leased && !a->recorded && a->role == role && a->bytes >= n &&
          a->bytes <= std::max<uint64_t>(n * 2, 4096) &&
          (!selected || a->bytes < selected->bytes))
        selected = a.get();
    if (selected) {
      ++stats.buffer_reuses;
    } else {
      auto a = std::make_unique<Allocation>();
      a->device = device;
      a->bytes = n;
      a->role = role;
      VkBufferCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      ci.size = n;
      ci.usage =
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      if (!host)
        ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      check(vkCreateBuffer(device, &ci, nullptr, &a->buffer), "create buffer");
      VkMemoryRequirements req{};
      vkGetBufferMemoryRequirements(device, a->buffer, &req);
      preflight_budget(req.size);
      if (allocations.size() >= properties.limits.maxMemoryAllocationCount)
        trim_idle();
      if (allocations.size() >= properties.limits.maxMemoryAllocationCount)
        throw std::length_error("Vulkan allocation count limit exceeded");
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
      budget.sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
      if (memory_budget_supported) {
        VkPhysicalDeviceMemoryProperties2 mp{};
        mp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mp.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physical, &mp);
      }
      int chosen = -1, best = -1;
      for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i)))
          continue;
        auto f = memory.memoryTypes[i].propertyFlags;
        if (host && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
          continue;
        auto heap = memory.memoryTypes[i].heapIndex;
        if (memory_budget_supported && budget.heapBudget[heap] &&
            (budget.heapUsage[heap] >= budget.heapBudget[heap] ||
             req.size > budget.heapBudget[heap] - budget.heapUsage[heap]))
          continue;
        int score =
            host ? ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 4 : 0) +
                       ((readback && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                            ? 8
                            : 0) +
                       (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 2 : 0)
                 : ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0);
        if (score > best) {
          best = score;
          chosen = int(i);
        }
      }
      if (chosen < 0)
        throw std::runtime_error(
            "no suitable Vulkan memory type within the device budget");
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = uint32_t(chosen);
      check(vkAllocateMemory(device, &ai, nullptr, &a->memory),
            "allocate Vulkan memory");
      a->allocation = req.size;
      a->flags = memory.memoryTypes[chosen].propertyFlags;
      a->accounting = &allocated_bytes;
      allocated_bytes += req.size;
      ++stats.memory_allocations;
      check(vkBindBufferMemory(device, a->buffer, a->memory, 0),
            "bind Vulkan memory");
      if (host)
        check(vkMapMemory(device, a->memory, 0, VK_WHOLE_SIZE, 0, &a->mapped),
              "map persistent staging memory");
      selected = a.get();
      allocations.push_back(std::move(a));
    }
    selected->leased = true;
    Buffer b;
    b.resource = selected;
    b.buffer = selected->buffer;
    b.memory = selected->memory;
    b.bytes = n;
    b.flags = selected->flags;
    b.mapped = selected->mapped;
    return b;
  }
  void retain(Buffer &b) { b.resource->recorded = true; }
  void begin() {
    if (recording)
      return;
    check(vkResetCommandBuffer(command, 0), "reset command buffer");
    check(vkResetDescriptorPool(device, descriptor_pool, 0),
          "reset descriptor pool");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
    recording = true;
    if (timestamp_pool) {
      vkCmdResetQueryPool(command, timestamp_pool, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          timestamp_pool, 0);
    }
  }
  template <class F> void submit(F prepare_host) {
    if (!recording)
      return;
    if (timestamp_pool)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          timestamp_pool, 1);
    check(vkEndCommandBuffer(command), "end command buffer");
    check(vkResetFences(device, 1, &fence), "reset fence");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &si, fence), "submit compute queue");
    ++stats.submissions;
    // Preparation touches independent host memory while transfers/compute run.
    // Even if allocation fails, join the submission before recycling buffers.
    std::exception_ptr preparation_error;
    try {
      prepare_host();
    } catch (...) {
      preparation_error = std::current_exception();
    }
    const auto wait_start = std::chrono::steady_clock::now();
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
          "wait compute fence");
    stats.fence_wait_ms += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - wait_start)
                               .count();
    recording = false;
    for (auto &a : allocations)
      a->recorded = false;
    if (timestamp_pool) {
      uint64_t ticks[2]{};
      check(vkGetQueryPoolResults(device, timestamp_pool, 0, 2, sizeof(ticks),
                                  ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT),
            "read device timestamps");
      const uint64_t mask = timestamp_bits == 64
                                ? UINT64_MAX
                                : (uint64_t(1) << timestamp_bits) - 1;
      stats.device_ms += double((ticks[1] - ticks[0]) & mask) *
                         properties.limits.timestampPeriod / 1e6;
      stats.gpu_timestamps = true;
    }
    if (validation_errors.load(std::memory_order_relaxed))
      throw std::runtime_error(
          "Vulkan validation error reported; results rejected");
    if (preparation_error)
      std::rethrow_exception(preparation_error);
  }
  void submit() {
    submit([] {});
  }
  void barrier(VkPipelineStageFlags src, VkAccessFlags sa,
               VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier m{};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    m.srcAccessMask = sa;
    m.dstAccessMask = da;
    vkCmdPipelineBarrier(command, src, dst, 0, 1, &m, 0, nullptr, 0, nullptr);
  }
  void compute_barrier() {
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
  struct Upload {
    Buffer *b;
    const void *data;
    size_t bytes;
  };
  void upload(std::initializer_list<Upload> items) {
    size_t total = 0;
    for (auto x : items) {
      if (x.bytes > x.b->bytes)
        throw std::logic_error("upload exceeds destination");
      if (x.bytes)
        total += (x.bytes + 3) & ~size_t(3);
    }
    if (!total)
      return;
    auto staging = make_buffer(total, true);
    void *mapped = staging.mapped;
    size_t off = 0;
    for (auto x : items)
      if (x.bytes) {
        std::memcpy(static_cast<char *>(mapped) + off, x.data, x.bytes);
        off += (x.bytes + 3) & ~size_t(3);
        stats.bytes_uploaded += x.bytes;
      }
    if (!(staging.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = staging.memory;
      range.size = VK_WHOLE_SIZE;
      check(vkFlushMappedMemoryRanges(device, 1, &range), "flush upload");
    }
    begin();
    retain(staging);
    off = 0;
    for (auto x : items)
      if (x.bytes) {
        retain(*x.b);
        VkBufferCopy c{off, 0, x.bytes};
        vkCmdCopyBuffer(command, staging.buffer, x.b->buffer, 1, &c);
        off += (x.bytes + 3) & ~size_t(3);
      }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
  void copy_to_host(Buffer &src, Buffer &host, size_t bytes,
                    size_t offset = 0) {
    if (offset > src.bytes || bytes > src.bytes - offset || bytes > host.bytes)
      throw std::logic_error("readback range exceeds buffer");
    retain(src);
    retain(host);
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c{offset, 0, bytes};
    vkCmdCopyBuffer(command, src.buffer, host.buffer, 1, &c);
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  }
  void mapped_read(Buffer &b, void *out, size_t bytes) {
    void *mapped = b.mapped;
    if (!(b.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = b.memory;
      range.size = VK_WHOLE_SIZE;
      check(vkInvalidateMappedMemoryRanges(device, 1, &range),
            "invalidate readback");
    }
    std::memcpy(out, mapped, bytes);
    stats.bytes_downloaded += bytes;
  }
  template <class T> std::vector<T> download(Buffer &b, size_t count) {
    std::vector<T> out;
    if (!count)
      return out;
    auto staging = make_buffer(count * sizeof(T), true, true);
    begin();
    copy_to_host(b, staging, count * sizeof(T));
    if (count * sizeof(T) >= 65536)
      submit([&] { out.resize(count); });
    else {
      out.resize(count);
      submit();
    }
    mapped_read(staging, out.data(), count * sizeof(T));
    return out;
  }
  VkPipeline pipeline(const std::string &name) {
    auto found = pipelines.find(name);
    if (found != pipelines.end())
      return found->second.pipeline;
    std::string dir = options.shader_dir;
    if (dir.empty()) {
      const char *env = std::getenv("LV_SHADER_DIR");
      if (env)
        dir = env;
    }
    const uint32_t *shader_code = nullptr;
    size_t shader_words = 0;
    std::vector<uint32_t> external_code;
#ifdef LV_EMBEDDED_SHADERS
    if (dir.empty()) {
      shader_code = embedded::shader(name, shader_words);
      if (!shader_code || shader_words < 5 || shader_code[0] != 0x07230203)
        throw std::runtime_error("invalid or missing embedded SPIR-V shader: " +
                                 name);
    } else
#else
    if (dir.empty())
      dir = LV_SHADER_DIR;
#endif
    {
      std::string path = dir + "/" + name + ".spv";
      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (!f)
        throw std::runtime_error("cannot open SPIR-V shader: " + path);
      auto end = f.tellg();
      if (end < 20 || uint64_t(end) % 4 || uint64_t(end) > 64 * 1024 * 1024)
        throw std::runtime_error("invalid SPIR-V shader length: " + path);
      external_code.resize(size_t(end) / 4);
      f.seekg(0);
      f.read(reinterpret_cast<char *>(external_code.data()), end);
      if (!f || external_code.front() != 0x07230203)
        throw std::runtime_error("invalid SPIR-V shader: " + path);
      shader_code = external_code.data();
      shader_words = external_code.size();
    }
    Pipeline p;
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = shader_words * 4;
    sm.pCode = shader_code;
    check(vkCreateShaderModule(device, &sm, nullptr, &p.shader),
          "create SPIR-V shader module");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = p.shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = pipeline_layout;
    VkResult result = vkCreateComputePipelines(device, pipeline_cache, 1, &ci,
                                               nullptr, &p.pipeline);
    if (result != VK_SUCCESS) {
      vkDestroyShaderModule(device, p.shader, nullptr);
      check(result, "create compute pipeline");
    }
    pipelines.emplace(name, p);
    return p.pipeline;
  }
  void dispatch(const std::string &shader, std::initializer_list<Buffer *> args,
                std::array<uint32_t, 8> params, uint32_t threads) {
    if (!threads)
      return;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    check(vkAllocateDescriptorSets(device, &ai, &set),
          "allocate descriptor set");
    std::array<VkDescriptorBufferInfo, 8> bis{};
    std::array<VkWriteDescriptorSet, 8> writes{};
    auto it = args.begin();
    for (uint32_t i = 0; i < 8; ++i) {
      Buffer *b = it != args.end() ? *it++ : dummy.get();
      if (!b)
        b = dummy.get();
      retain(*b);
      bis[i] = {b->buffer, 0, b->bytes};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &bis[i];
    }
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0,
                           nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline(shader));
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout, 0, 1, &set, 0, nullptr);
    uint64_t max_threads =
        uint64_t(properties.limits.maxComputeWorkGroupCount[0]) * 128;
    for (uint64_t base = 0; base < threads; base += max_threads) {
      params[6] = uint32_t(base);
      uint32_t count =
          uint32_t(std::min<uint64_t>(uint64_t(threads) - base, max_threads));
      vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                         0, 32, params.data());
      vkCmdDispatch(command, uint32_t((uint64_t(count) + 127) / 128), 1, 1);
      ++stats.dispatches;
    }
  }
  void start() {
    // A rejected operation may have an unsubmitted recording. Discard it before
    // recycling its buffers; no command from a failed operation may execute
    // later.
    if (recording) {
      check(vkResetCommandBuffer(command, 0), "discard failed recording");
      recording = false;
      for (auto &a : allocations)
        a->recorded = false;
    }
    stats = {};
    stats.backend = options.backend == Backend::Cpu ? "cpu" : "vulkan";
    stats.device_name = device_info.name;
    started = std::chrono::steady_clock::now();
  }
  void finish() {
    stats.allocated_bytes = allocated_bytes;
    stats.wall_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
  }
  template <class F>
  void iterate(Buffer &changed, F record, uint64_t max_rounds,
               uint32_t maximum_batch = 8) {
    auto host = make_buffer(4, true, true);
    uint64_t rounds = 0;
    uint32_t next_batch = 8;
    for (;;) {
      begin();
      vkCmdFillBuffer(command, changed.buffer, 0, maximum_batch * 4, 0);
      barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      uint32_t batch =
          uint32_t(std::min<uint64_t>(next_batch, max_rounds - rounds));
      if (!batch)
        throw std::runtime_error(
            "dataflow failed to reach its monotone iteration bound");
      for (uint32_t i = 0; i < batch; ++i) {
        record(i);
        compute_barrier();
        ++rounds;
        ++stats.iterations;
      }
      // Each round owns a different flag. Only the final round determines
      // whether the current fixed point is stable; earlier productive rounds
      // must not force another full batch and host wait.
      copy_to_host(changed, host, 4, size_t(batch - 1) * 4);
      submit();
      uint32_t flag = 0;
      mapped_read(host, &flag, 4);
      if (!flag)
        return;
      next_batch = std::min(maximum_batch, next_batch * 2);
      if (rounds >= max_rounds)
        throw std::runtime_error(
            "dataflow failed to converge within n+16 Jacobi rounds");
    }
  }
  struct ScanLevel {
    uint32_t count;
    Buffer output, sums;
  };
  std::vector<ScanLevel> scan_workspace(uint32_t n) {
    std::vector<ScanLevel> levels;
    for (uint32_t count = n;;) {
      uint32_t blocks = uint32_t((uint64_t(count) + 127) / 128);
      levels.push_back({count, make_buffer(uint64_t(count) * 4, false),
                        make_buffer(uint64_t(blocks) * 4, false)});
      if (blocks == 1)
        break;
      count = blocks;
    }
    return levels;
  }
  Buffer &record_scan(Buffer &input, std::vector<ScanLevel> &levels) {
    begin();
    const char *shader =
        options.enable_fast_paths && device_info.supports_subgroup_arithmetic
            ? "scan_subgroup"
            : "scan";
    for (size_t l = 0; l < levels.size(); ++l) {
      auto &level = levels[l];
      auto *src = l ? &levels[l - 1].sums : &input;
      dispatch(shader, {src, &level.output, &level.sums},
               {level.count, 0, 0, 0, 0, 0, 0, 0}, level.count);
      compute_barrier();
    }
    for (size_t l = levels.size() - 1; l > 0; --l) {
      auto &level = levels[l - 1];
      dispatch("scan_add", {&level.output, &levels[l].output},
               {level.count, 0, 0, 0, 0, 0, 0, 0}, level.count);
      compute_barrier();
    }
    return levels[0].output;
  }
  bool local_bits(uint32_t cells) const {
    return options.enable_fast_paths && cells <= 4096 &&
           properties.limits.maxComputeSharedMemorySize >= 32772;
  }
  Buffer device_reachable(Buffer &off, Buffer &edge, Buffer initial, uint32_t n,
                          const std::vector<uint32_t> &components = {}) {
    auto next = make_buffer(uint64_t(n) * 4, false);
    bool small_components = n <= 1024;
    if (!components.empty()) {
      small_components =
          components.size() > 1 && components.size() - 1 <= UINT32_MAX / 128;
      for (size_t i = 1; i < components.size(); ++i)
        small_components &= components[i] - components[i - 1] <= 1024;
    }
    if (options.enable_fast_paths && small_components &&
        properties.limits.maxComputeSharedMemorySize >= 8196) {
      const std::vector<uint32_t> single{0, n};
      const auto &parts = components.empty() ? single : components;
      auto ranges = make_buffer(parts.size() * 4, false);
      upload({{&ranges, parts.data(), parts.size() * 4}});
      begin();
      dispatch("reachable_components", {&off, &edge, &initial, &next, &ranges},
               {n, 0, 0, 0, 0, 0, 0, 0}, uint32_t(parts.size() - 1) * 128);
      compute_barrier();
      return next;
    }
    // A single oversized function must not force every small independent
    // function through global convergence. Keep this specialization
    // work-saving: at least half the nodes must be handled by the local solver.
    // More than one oversized component retains the one-dispatch-per-round
    // general path.
    std::vector<uint32_t> local_ranges;
    uint32_t large_first = 0, large_end = n, large_count = 0;
    uint64_t local_nodes = 0;
    if (options.enable_fast_paths &&
        properties.limits.maxComputeSharedMemorySize >= 8196 &&
        !components.empty()) {
      for (size_t i = 1; i < components.size(); ++i) {
        const uint32_t first = components[i - 1], end = components[i];
        if (end - first <= 1024) {
          local_ranges.push_back(first);
          local_ranges.push_back(end);
          local_nodes += end - first;
        } else {
          ++large_count;
          large_first = first;
          large_end = end;
        }
      }
    }
    const bool mixed = large_count == 1 && local_nodes * 2 >= n &&
                       local_ranges.size() / 2 <= UINT32_MAX / 128 &&
                       local_ranges.size() * uint64_t(4) <=
                           properties.limits.maxStorageBufferRange;
    if (mixed) {
      auto ranges = make_buffer(local_ranges.size() * 4, false);
      upload({{&ranges, local_ranges.data(), local_ranges.size() * 4}});
      begin();
      auto copy_state = [&](Buffer &src, Buffer &dst) {
        retain(src);
        retain(dst);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferCopy copy{0, 0, uint64_t(n) * 4};
        vkCmdCopyBuffer(command, src.buffer, dst.buffer, 1, &copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      };
      copy_state(initial, next);
      dispatch("reachable_components", {&off, &edge, &initial, &next, &ranges},
               {n, 1, 0, 0, 0, 0, 0, 0},
               uint32_t(local_ranges.size() / 2) * 128);
      // Preserve the completed local components in both ping-pong buffers.
      copy_state(next, initial);
    } else {
      large_first = 0;
      large_end = n;
    }
    // Sparse, bounded working sets have cheap rounds but expensive host polls.
    // Grow batches only there; large/dense graphs retain eight-round batches.
    const uint32_t batch_limit =
        large_end - large_first <= 65536 && edge.bytes <= uint64_t(n) * 16 ? 64
                                                                           : 8;
    auto changed = make_buffer(batch_limit * 4, false);
    Buffer *cur = &initial, *after = &next;
    iterate(
        changed,
        [&](uint32_t round) {
          dispatch("reachable_rounds", {&off, &edge, cur, after, &changed},
                   {large_end, large_first, 0, 0, 0, 0, 0, round},
                   large_end - large_first);
          std::swap(cur, after);
        },
        uint64_t(large_end - large_first) + 16, batch_limit);
    return std::move(*cur);
  }
};

Compute::Compute(Options o) : p_(std::make_unique<Impl>(std::move(o))) {}
Compute::~Compute() = default;
Compute::Compute(Compute &&) noexcept = default;
Compute &Compute::operator=(Compute &&) noexcept = default;
const DeviceInfo &Compute::device_info() const { return p_->device_info; }
const Stats &Compute::last_stats() const { return p_->stats; }
std::vector<DeviceInfo> Compute::devices() {
  VkInstance i = new_instance(false, nullptr, nullptr);
  std::vector<DeviceInfo> out;
  try {
    for (auto p : physicals(i))
      out.push_back(info(p));
  } catch (...) {
    vkDestroyInstance(i, nullptr);
    throw;
  }
  vkDestroyInstance(i, nullptr);
  return out;
}
std::vector<FoldResult> Compute::fold(const std::vector<FoldRequest> &in) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::fold(in);
    p_->finish();
    return r;
  }
  uint32_t n = checked_count(in.size(), "fold tasks");
  if (!n) {
    p_->finish();
    return {};
  }
  auto a = p_->make_buffer(in.size() * sizeof(FoldRequest), false),
       b = p_->make_buffer(in.size() * sizeof(FoldResult), false);
  p_->upload({{&a, in.data(), in.size() * sizeof(FoldRequest)}});
  p_->begin();
  p_->dispatch("fold", {&a, &b}, {n, 0, 0, 0, 0, 0, 0, 0}, n);
  auto r = p_->download<FoldResult>(b, n);
  p_->finish();
  return r;
}
std::vector<Fold64Result>
Compute::fold64(const std::vector<Fold64Request> &in) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::fold64(in);
    p_->finish();
    return r;
  }
  const uint32_t n = checked_count(in.size(), "fold64 tasks");
  if (!n) {
    p_->finish();
    return {};
  }
  if (!p_->device_info.supports_int64)
    throw std::runtime_error(
        "fold64 requires Vulkan shaderInt64; selected device does not support "
        "it; no CPU fallback performed");
  auto a = p_->make_buffer(in.size() * sizeof(Fold64Request), false);
  auto b = p_->make_buffer(in.size() * sizeof(Fold64Result), false);
  p_->upload({{&a, in.data(), in.size() * sizeof(Fold64Request)}});
  p_->begin();
  p_->dispatch("fold64", {&a, &b}, {n, 0, 0, 0, 0, 0, 0, 0}, n);
  auto r = p_->download<Fold64Result>(b, n);
  p_->finish();
  return r;
}
std::vector<uint32_t> Compute::reachable(const Graph &g,
                                         const std::vector<uint32_t> &seeds) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::reachable(g, seeds);
    p_->finish();
    return r;
  }
  cpu::validate_graph(g);
  if (seeds.size() != g.nodes())
    throw std::invalid_argument("seed flags must match graph nodes");
  uint32_t n = checked_count(g.nodes(), "graph nodes");
  if (!n) {
    p_->finish();
    return {};
  }
  p_->preflight_storage(uint64_t(n + 1ull) * 4);
  p_->preflight_storage(uint64_t(g.edges.size()) * 4);
  p_->preflight_storage(uint64_t(n) * 4);
  p_->preflight_working_set(uint64_t(n) * 20 + uint64_t(g.edges.size()) * 8 +
                            12);
  auto rev = reverse_graph(g);
  std::vector<uint32_t> initial(seeds);
  for (auto &x : initial)
    x = uint32_t(x != 0);
  auto off = p_->make_buffer(rev.offsets.size() * 4, false),
       edge = p_->make_buffer(rev.edges.size() * 4, false),
       initial_buffer = p_->make_buffer(uint64_t(n) * 4, false);
  p_->upload({{&off, rev.offsets.data(), rev.offsets.size() * 4},
              {&edge, rev.edges.data(), rev.edges.size() * 4},
              {&initial_buffer, initial.data(), initial.size() * 4}});
  auto result = p_->device_reachable(off, edge, std::move(initial_buffer), n,
                                     g.components);
  auto r = p_->download<uint32_t>(result, n);
  p_->finish();
  return r;
}
std::vector<uint32_t> Compute::dominators(const Graph &g, uint32_t entry) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::dominators(g, entry);
    p_->finish();
    return r;
  }
  cpu::validate_graph(g);
  uint32_t n = checked_count(g.nodes(), "graph nodes");
  if (!n) {
    if (entry)
      throw std::invalid_argument("entry outside empty graph");
    p_->finish();
    return {};
  }
  if (entry >= n)
    throw std::invalid_argument("dominator entry out of range");
  uint32_t words = uint32_t((uint64_t(n) + 31) / 32);
  uint32_t cells = checked_count(uint64_t(n) * words, "dominator matrix");
  p_->preflight_storage(uint64_t(cells) * 4);
  p_->preflight_storage(uint64_t(n + 1ull) * 4);
  p_->preflight_storage(uint64_t(g.edges.size()) * 4);
  p_->preflight_working_set(uint64_t(cells) * 12 + uint64_t(n) * 16 +
                            uint64_t(g.edges.size()) * 8 + 12);
  auto rev = reverse_graph(g);
  std::vector<uint32_t> seeds(n, 0);
  seeds[entry] = 1;
  auto off = p_->make_buffer(rev.offsets.size() * 4, false),
       edge = p_->make_buffer(rev.edges.size() * 4, false),
       seed_buffer = p_->make_buffer(uint64_t(n) * 4, false);
  p_->upload({{&off, rev.offsets.data(), rev.offsets.size() * 4},
              {&edge, rev.edges.data(), rev.edges.size() * 4},
              {&seed_buffer, seeds.data(), seeds.size() * 4}});
  auto reach = p_->device_reachable(off, edge, std::move(seed_buffer), n);
  auto a = p_->make_buffer(uint64_t(cells) * 4, false),
       b = p_->make_buffer(uint64_t(cells) * 4, false);
  p_->begin();
  p_->dispatch("dom_init", {&reach, &a}, {cells, words, entry, n, 0, 0, 0, 0},
               cells);
  p_->compute_barrier();
  Impl::Buffer *cur = &a, *next = &b;
  if (p_->local_bits(cells)) {
    p_->dispatch(cells >= 1024 ? "dominators_local_banked" : "dominators_local",
                 {&off, &edge, &a, &b, &reach},
                 {cells, words, entry, n, 0, 0, 0, 0}, 128);
    cur = &b;
  } else {
    auto changed = p_->make_buffer(32, false);
    p_->iterate(
        changed,
        [&](uint32_t round) {
          p_->dispatch("dominators_rounds",
                       {&off, &edge, cur, next, &reach, &changed},
                       {cells, words, entry, 0, 0, 0, 0, round}, cells);
          std::swap(cur, next);
        },
        uint64_t(n) + 16);
  }
  auto r = p_->download<uint32_t>(*cur, cells);
  p_->finish();
  return r;
}
std::vector<uint32_t> Compute::liveness(const Graph &g, uint32_t words,
                                        const std::vector<uint32_t> &gen,
                                        const std::vector<uint32_t> &kill) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::liveness(g, words, gen, kill);
    p_->finish();
    return r;
  }
  cpu::validate_graph(g);
  uint32_t n = checked_count(g.nodes(), "graph nodes");
  uint32_t cells = checked_count(uint64_t(n) * words, "liveness matrix");
  if (gen.size() != cells || kill.size() != cells)
    throw std::invalid_argument("liveness bitsets have wrong dimensions");
  if (!cells) {
    p_->finish();
    return {};
  }
  p_->preflight_storage(uint64_t(cells) * 4);
  p_->preflight_storage(uint64_t(n + 1ull) * 4);
  p_->preflight_storage(uint64_t(g.edges.size()) * 4);
  p_->preflight_working_set(uint64_t(cells) * 28 + uint64_t(n + 1ull) * 8 +
                            uint64_t(g.edges.size()) * 8 + 4);
  std::vector<uint32_t> zero(cells, 0);
  auto off = p_->make_buffer(g.offsets.size() * 4, false),
       edge = p_->make_buffer(g.edges.size() * 4, false),
       a = p_->make_buffer(uint64_t(cells) * 4, false),
       b = p_->make_buffer(uint64_t(cells) * 4, false),
       gb = p_->make_buffer(gen.size() * 4, false),
       kb = p_->make_buffer(kill.size() * 4, false),
       changed = p_->make_buffer(32, false);
  p_->upload({{&off, g.offsets.data(), g.offsets.size() * 4},
              {&edge, g.edges.data(), g.edges.size() * 4},
              {&a, zero.data(), zero.size() * 4},
              {&gb, gen.data(), gen.size() * 4},
              {&kb, kill.data(), kill.size() * 4}});
  Impl::Buffer *cur = &a;
  Impl::Buffer *next = &b;
  if (p_->local_bits(cells)) {
    p_->begin();
    p_->dispatch("liveness_local", {&off, &edge, &a, &b, &gb, &kb},
                 {cells, words, n, 0, 0, 0, 0, 0}, 128);
    cur = &b;
  } else {
    p_->iterate(
        changed,
        [&](uint32_t round) {
          p_->dispatch("liveness_rounds",
                       {&off, &edge, cur, next, &gb, &kb, &changed},
                       {cells, words, 0, 0, 0, 0, 0, round}, cells);
          std::swap(cur, next);
        },
        uint64_t(n) + 16);
  }
  auto r = p_->download<uint32_t>(*cur, cells);
  p_->finish();
  return r;
}
std::vector<uint32_t> Compute::exclusive_scan(const std::vector<uint32_t> &in) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::exclusive_scan(in);
    p_->finish();
    return r;
  }
  uint32_t n = checked_count(in.size(), "scan elements");
  if (!n) {
    p_->finish();
    return {};
  }
  auto input = p_->make_buffer(in.size() * 4, false);
  p_->upload({{&input, in.data(), in.size() * 4}});
  auto levels = p_->scan_workspace(n);
  auto &result = p_->record_scan(input, levels);
  auto r = p_->download<uint32_t>(result, n);
  p_->finish();
  return r;
}
std::vector<uint32_t> Compute::sort_keys(const std::vector<uint32_t> &in) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::sort_keys(in);
    p_->finish();
    return r;
  }
  uint32_t original = checked_count(in.size(), "sort elements");
  if (!original) {
    p_->finish();
    return {};
  }
  if (p_->options.enable_fast_paths && original <= 128) {
    auto keys = p_->make_buffer(uint64_t(original) * 4, false);
    p_->upload({{&keys, in.data(), in.size() * 4}});
    p_->begin();
    p_->dispatch("sort_local", {&keys}, {original, 0, 0, 0, 0, 0, 0, 0}, 128);
    auto result = p_->download<uint32_t>(keys, original);
    p_->finish();
    return result;
  }
  if (p_->options.enable_fast_paths &&
      uint64_t(original) * 18 + 65536 < p_->options.memory_limit_bytes) {
    const uint32_t blocks = uint32_t((uint64_t(original) + 127) / 128);
    const uint32_t histogram_count =
        checked_count(uint64_t(blocks) * 16, "radix histograms");
    p_->preflight_storage(uint64_t(histogram_count) * 4);
    auto a = p_->make_buffer(uint64_t(original) * 4, false),
         b = p_->make_buffer(uint64_t(original) * 4, false),
         histogram = p_->make_buffer(uint64_t(histogram_count) * 4, false);
    auto scratch = p_->scan_workspace(histogram_count);
    p_->upload({{&a, in.data(), in.size() * 4}});
    Impl::Buffer *cur = &a, *next = &b;
    for (uint32_t shift = 0; shift < 32; shift += 4) {
      p_->begin();
      p_->dispatch("radix_histogram", {cur, &histogram},
                   {original, shift, blocks, 0, 0, 0, 0, 0}, original);
      p_->compute_barrier();
      auto &offsets = p_->record_scan(histogram, scratch);
      p_->dispatch("radix_scatter", {cur, &offsets, next},
                   {original, shift, blocks, 0, 0, 0, 0, 0}, original);
      p_->compute_barrier();
      std::swap(cur, next);
    }
    auto result = p_->download<uint32_t>(*cur, original);
    p_->finish();
    return result;
  }
  uint64_t size = 1;
  while (size < original)
    size *= 2;
  uint32_t n = checked_count(size, "padded sort elements");
  p_->preflight_storage(uint64_t(n) * 4);
  p_->preflight_working_set(uint64_t(n) * 8);
  std::vector<uint32_t> padded(in);
  padded.resize(n, UINT32_MAX);
  auto a = p_->make_buffer(uint64_t(n) * 4, false);
  p_->upload({{&a, padded.data(), padded.size() * 4}});
  p_->begin();
  for (uint64_t k = 2; k <= n; k *= 2)
    for (uint32_t j = uint32_t(k / 2); j; j /= 2) {
      p_->dispatch("sort", {&a}, {n, j, uint32_t(k), 0, 0, 0, 0, 0}, n);
      p_->compute_barrier();
    }
  auto r = p_->download<uint32_t>(a, original);
  p_->finish();
  return r;
}
std::vector<uint32_t>
Compute::hash_records(const std::vector<uint32_t> &words,
                      const std::vector<uint32_t> &offsets) {
  p_->start();
  if (p_->options.backend == Backend::Cpu) {
    auto r = cpu::hash_records(words, offsets);
    p_->finish();
    return r;
  }
  if (offsets.empty()) {
    if (!words.empty())
      throw std::invalid_argument("hash offsets are empty");
    p_->finish();
    return {};
  }
  checked_count(words.size(), "hash words");
  uint32_t n = checked_count(offsets.size() - 1, "hash records");
  if (offsets.front() != 0 || offsets.back() != words.size())
    throw std::invalid_argument("invalid hash offsets");
  for (size_t i = 1; i < offsets.size(); ++i)
    if (offsets[i] < offsets[i - 1])
      throw std::invalid_argument("nonmonotonic hash offsets");
  if (!n) {
    p_->finish();
    return {};
  }
  auto w = p_->make_buffer(words.size() * 4, false),
       o = p_->make_buffer(offsets.size() * 4, false),
       h = p_->make_buffer(uint64_t(n) * 4, false);
  p_->upload({{&w, words.data(), words.size() * 4},
              {&o, offsets.data(), offsets.size() * 4}});
  p_->begin();
  p_->dispatch("hash", {&w, &o, &h}, {n, 0, 0, 0, 0, 0, 0, 0}, n);
  auto r = p_->download<uint32_t>(h, n);
  p_->finish();
  return r;
}
} // namespace lv
