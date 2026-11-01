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
  return {d.deviceName,
          d.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU,
          d.apiVersion,
          d.vendorID,
          d.deviceID,
          d.limits.maxStorageBufferRange,
          f.shaderInt64 == VK_TRUE,
          d.driverVersion};
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
  struct Pipeline {
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  std::map<std::string, Pipeline> pipelines;
  struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0, allocation = 0;
    VkMemoryPropertyFlags flags = 0;
    uint64_t *accounting = nullptr;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&b) noexcept { *this = std::move(b); }
    Buffer &operator=(Buffer &&b) noexcept {
      if (this != &b) {
        clear();
        device = b.device;
        buffer = b.buffer;
        memory = b.memory;
        bytes = b.bytes;
        allocation = b.allocation;
        flags = b.flags;
        accounting = b.accounting;
        b.buffer = VK_NULL_HANDLE;
        b.memory = VK_NULL_HANDLE;
        b.accounting = nullptr;
      }
      return *this;
    }
    ~Buffer() { clear(); }
    void clear() {
      if (buffer)
        vkDestroyBuffer(device, buffer, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
      if (accounting)
        *accounting -= allocation;
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
      accounting = nullptr;
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
    for (auto &p : pipelines) {
      if (p.second.pipeline)
        vkDestroyPipeline(device, p.second.pipeline, nullptr);
      if (p.second.shader)
        vkDestroyShaderModule(device, p.second.shader, nullptr);
    }
    pipelines.clear();
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
  }
  void preflight_storage(uint64_t bytes) const {
    if (std::max<uint64_t>(bytes, 4) > properties.limits.maxStorageBufferRange)
      throw std::length_error(
          "storage buffer exceeds Vulkan maxStorageBufferRange");
  }
  void preflight_budget(uint64_t minimum_bytes) const {
    if (minimum_bytes > options.memory_limit_bytes ||
        allocated_bytes > options.memory_limit_bytes - minimum_bytes)
      throw std::length_error(
          "configured Vulkan memory budget exceeded before host preparation");
  }
  Buffer make_buffer(uint64_t n, bool host) {
    n = std::max<uint64_t>(n, 4);
    if (!host && n > properties.limits.maxStorageBufferRange)
      throw std::length_error(
          "storage buffer exceeds Vulkan maxStorageBufferRange");
    Buffer b;
    b.device = device;
    b.bytes = n;
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = n;
    ci.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!host)
      ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "create buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    if (req.size > options.memory_limit_bytes ||
        allocated_bytes > options.memory_limit_bytes - req.size)
      throw std::length_error("configured Vulkan memory budget exceeded");
    int chosen = -1, best = -1;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if (req.memoryTypeBits & (1u << i)) {
        auto f = memory.memoryTypes[i].propertyFlags;
        if (host && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
          continue;
        int score = host
                        ? ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                              ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0)
                        : ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0);
        if (score > best) {
          best = score;
          chosen = int(i);
        }
      }
    if (chosen < 0)
      throw std::runtime_error("no suitable Vulkan buffer memory type");
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(chosen);
    check(vkAllocateMemory(device, &ai, nullptr, &b.memory),
          "allocate Vulkan memory");
    b.allocation = req.size;
    b.flags = memory.memoryTypes[chosen].propertyFlags;
    b.accounting = &allocated_bytes;
    allocated_bytes += req.size;
    check(vkBindBufferMemory(device, b.buffer, b.memory, 0),
          "bind Vulkan memory");
    return b;
  }
  void begin() {
    check(vkResetCommandBuffer(command, 0), "reset command buffer");
    check(vkResetDescriptorPool(device, descriptor_pool, 0),
          "reset descriptor pool");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
  }
  void submit() {
    check(vkEndCommandBuffer(command), "end command buffer");
    check(vkResetFences(device, 1, &fence), "reset fence");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &si, fence), "submit compute queue");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
          "wait compute fence");
    if (validation_errors.load(std::memory_order_relaxed))
      throw std::runtime_error(
          "Vulkan validation error reported; results rejected");
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
    void *mapped = nullptr;
    check(vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
          "map upload buffer");
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
    vkUnmapMemory(device, staging.memory);
    begin();
    off = 0;
    for (auto x : items)
      if (x.bytes) {
        VkBufferCopy c{off, 0, x.bytes};
        vkCmdCopyBuffer(command, staging.buffer, x.b->buffer, 1, &c);
        off += (x.bytes + 3) & ~size_t(3);
      }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    submit();
  }
  void copy_to_host(Buffer &src, Buffer &host, size_t bytes) {
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c{0, 0, bytes};
    vkCmdCopyBuffer(command, src.buffer, host.buffer, 1, &c);
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  }
  void mapped_read(Buffer &b, void *out, size_t bytes) {
    void *mapped = nullptr;
    check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
          "map readback");
    if (!(b.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = b.memory;
      range.size = VK_WHOLE_SIZE;
      check(vkInvalidateMappedMemoryRanges(device, 1, &range),
            "invalidate readback");
    }
    std::memcpy(out, mapped, bytes);
    vkUnmapMemory(device, b.memory);
    stats.bytes_downloaded += bytes;
  }
  template <class T> std::vector<T> download(Buffer &b, size_t count) {
    std::vector<T> out(count);
    if (!count)
      return out;
    auto staging = make_buffer(count * sizeof(T), true);
    begin();
    copy_to_host(b, staging, count * sizeof(T));
    submit();
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
    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
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
    stats = {};
    stats.backend = options.backend == Backend::Cpu ? "cpu" : "vulkan";
    stats.device_name = device_info.name;
    started = std::chrono::steady_clock::now();
  }
  void finish() {
    stats.wall_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
  }
  template <class F>
  void iterate(Buffer &changed, F record, uint64_t max_rounds) {
    auto host = make_buffer(4, true);
    uint64_t rounds = 0;
    for (;;) {
      begin();
      vkCmdFillBuffer(command, changed.buffer, 0, 4, 0);
      barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      uint32_t batch = uint32_t(std::min<uint64_t>(8, max_rounds - rounds));
      if (!batch)
        throw std::runtime_error(
            "dataflow failed to reach its monotone iteration bound");
      for (uint32_t i = 0; i < batch; ++i) {
        record();
        compute_barrier();
        ++rounds;
        ++stats.iterations;
      }
      copy_to_host(changed, host, 4);
      submit();
      uint32_t flag = 0;
      mapped_read(host, &flag, 4);
      if (!flag)
        return;
      // One additional all-stable batch is permitted because changed
      // accumulates over the batch, including its first productive rounds.
      if (rounds >= max_rounds)
        throw std::runtime_error(
            "dataflow failed to converge within n+16 Jacobi rounds");
    }
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
  p_->submit();
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
  p_->submit();
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
  p_->preflight_budget(uint64_t(n) * 20 + uint64_t(g.edges.size()) * 8 + 12);
  auto rev = reverse_graph(g);
  std::vector<uint32_t> initial(seeds);
  for (auto &x : initial)
    x = uint32_t(x != 0);
  auto off = p_->make_buffer(rev.offsets.size() * 4, false),
       edge = p_->make_buffer(rev.edges.size() * 4, false),
       a = p_->make_buffer(n * uint64_t(4), false),
       b = p_->make_buffer(n * uint64_t(4), false),
       changed = p_->make_buffer(4, false);
  p_->upload({{&off, rev.offsets.data(), rev.offsets.size() * 4},
              {&edge, rev.edges.data(), rev.edges.size() * 4},
              {&a, initial.data(), initial.size() * 4}});
  Impl::Buffer *cur = &a;
  Impl::Buffer *next = &b;
  p_->iterate(
      changed,
      [&] {
        p_->dispatch("reachable", {&off, &edge, cur, next, &changed},
                     {n, 0, 0, 0, 0, 0, 0, 0}, n);
        std::swap(cur, next);
      },
      uint64_t(n) + 16);
  auto r = p_->download<uint32_t>(*cur, n);
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
  p_->preflight_budget(uint64_t(cells) * 12 + uint64_t(n) * 16 +
                       uint64_t(g.edges.size()) * 8 + 12);
  // GPU reachability is itself part of the measured dominator operation.
  auto begin_time = p_->started;
  std::vector<uint32_t> seeds(n, 0);
  seeds[entry] = 1;
  auto reach = reachable(g, seeds);
  p_->started = begin_time;
  auto rev = reverse_graph(g);
  std::vector<uint32_t> mask(words, 0), initial(cells, 0);
  for (uint32_t v = 0; v < n; ++v)
    if (reach[v])
      mask[v / 32] |= 1u << (v % 32);
  for (uint32_t v = 0; v < n; ++v)
    if (reach[v])
      for (uint32_t w = 0; w < words; ++w)
        initial[size_t(v) * words + w] =
            v == entry ? (w == v / 32 ? 1u << (v % 32) : 0) : mask[w];
  auto off = p_->make_buffer(rev.offsets.size() * 4, false),
       edge = p_->make_buffer(rev.edges.size() * 4, false),
       a = p_->make_buffer(uint64_t(cells) * 4, false),
       b = p_->make_buffer(uint64_t(cells) * 4, false),
       rb = p_->make_buffer(reach.size() * 4, false),
       changed = p_->make_buffer(4, false);
  p_->upload({{&off, rev.offsets.data(), rev.offsets.size() * 4},
              {&edge, rev.edges.data(), rev.edges.size() * 4},
              {&a, initial.data(), initial.size() * 4},
              {&rb, reach.data(), reach.size() * 4}});
  Impl::Buffer *cur = &a;
  Impl::Buffer *next = &b;
  p_->iterate(
      changed,
      [&] {
        p_->dispatch("dominators", {&off, &edge, cur, next, &rb, &changed},
                     {cells, words, entry, 0, 0, 0, 0, 0}, cells);
        std::swap(cur, next);
      },
      uint64_t(n) + 16);
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
  p_->preflight_budget(uint64_t(cells) * 28 + uint64_t(n + 1ull) * 8 +
                       uint64_t(g.edges.size()) * 8 + 4);
  std::vector<uint32_t> zero(cells, 0);
  auto off = p_->make_buffer(g.offsets.size() * 4, false),
       edge = p_->make_buffer(g.edges.size() * 4, false),
       a = p_->make_buffer(uint64_t(cells) * 4, false),
       b = p_->make_buffer(uint64_t(cells) * 4, false),
       gb = p_->make_buffer(gen.size() * 4, false),
       kb = p_->make_buffer(kill.size() * 4, false),
       changed = p_->make_buffer(4, false);
  p_->upload({{&off, g.offsets.data(), g.offsets.size() * 4},
              {&edge, g.edges.data(), g.edges.size() * 4},
              {&a, zero.data(), zero.size() * 4},
              {&gb, gen.data(), gen.size() * 4},
              {&kb, kill.data(), kill.size() * 4}});
  Impl::Buffer *cur = &a;
  Impl::Buffer *next = &b;
  p_->iterate(
      changed,
      [&] {
        p_->dispatch("liveness", {&off, &edge, cur, next, &gb, &kb, &changed},
                     {cells, words, 0, 0, 0, 0, 0, 0}, cells);
        std::swap(cur, next);
      },
      uint64_t(n) + 16);
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
  struct Level {
    uint32_t count;
    Impl::Buffer output, sums;
  };
  std::vector<Level> levels;
  for (uint32_t count = n;;) {
    uint32_t blocks = uint32_t((uint64_t(count) + 127) / 128);
    levels.push_back({count, p_->make_buffer(uint64_t(count) * 4, false),
                      p_->make_buffer(uint64_t(blocks) * 4, false)});
    if (blocks == 1)
      break;
    count = blocks;
  }
  p_->begin();
  for (size_t l = 0; l < levels.size(); ++l) {
    auto &level = levels[l];
    auto *src = l ? &levels[l - 1].sums : &input;
    p_->dispatch("scan", {src, &level.output, &level.sums},
                 {level.count, 0, 0, 0, 0, 0, 0, 0}, level.count);
    p_->compute_barrier();
  }
  for (size_t l = levels.size() - 1; l > 0; --l) {
    auto &level = levels[l - 1];
    p_->dispatch("scan_add", {&level.output, &levels[l].output},
                 {level.count, 0, 0, 0, 0, 0, 0, 0}, level.count);
    p_->compute_barrier();
  }
  p_->submit();
  auto r = p_->download<uint32_t>(levels[0].output, n);
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
  uint64_t size = 1;
  while (size < original)
    size *= 2;
  uint32_t n = checked_count(size, "padded sort elements");
  p_->preflight_storage(uint64_t(n) * 4);
  p_->preflight_budget(uint64_t(n) * 8);
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
  p_->submit();
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
  p_->submit();
  auto r = p_->download<uint32_t>(h, n);
  p_->finish();
  return r;
}
} // namespace lv
