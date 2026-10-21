#pragma once

#if defined(__APPLE__)
#include <cstdint>
#include <string>

void SetMacAppIcon(const std::string& icon_path);
void AutoConfigureMacAppIcon(const std::string& exe_dir = "");
void GetMacGpuMemoryStats(uint64_t& used_bytes, uint64_t& budget_bytes);
#endif
