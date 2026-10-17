#pragma once

#if defined(__APPLE__)
#include <string>

void SetMacAppIcon(const std::string& icon_path);
void AutoConfigureMacAppIcon(const std::string& exe_dir = "");
#endif
