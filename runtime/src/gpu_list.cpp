#include "gpu_list.h"

#include <cstdio>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(list_gpus, false, "Azura",
                    "Print the graphics cards the game can run on, as JSON, and exit (used by the "
                    "launcher)");

namespace azura {

namespace {

// A string as a JSON string literal.
std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out += char(c);
        }
        break;
    }
  }
  return out + "\"";
}

const char* DeviceTypeName(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "cpu";
    default:
      return "other";
  }
}

}  // namespace

bool GpuListRequested() { return REXCVAR_GET(list_gpus); }

int PrintGpuList() {
  std::vector<rex::ui::vulkan::VulkanProvider::DeviceInfo> devices;
  std::string automatic_uuid;
  if (!rex::ui::vulkan::VulkanProvider::ListDevices(devices, automatic_uuid)) {
    std::printf("{\"devices\":[],\"automatic\":\"\"}\n");
    std::fflush(stdout);
    return 1;
  }
  std::string json = "{\"devices\":[";
  for (size_t i = 0; i < devices.size(); ++i) {
    const auto& device = devices[i];
    json += (i ? "," : "") + std::string("{\"name\":") + JsonString(device.name) +
            ",\"uuid\":" + JsonString(device.uuid) + ",\"type\":\"" +
            DeviceTypeName(device.type) + "\",\"vendorId\":" + std::to_string(device.vendor_id) +
            "}";
  }
  json += "],\"automatic\":" + JsonString(automatic_uuid) + "}\n";
  std::fputs(json.c_str(), stdout);
  std::fflush(stdout);
  return 0;
}

}  // namespace azura
