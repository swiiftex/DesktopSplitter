// Config.h - loads %ProgramData%\DesktopSplitter\config.json.
#pragma once

#include "Common.h"

namespace ds {

// Returns the default config path: %ProgramData%\DesktopSplitter\config.json
std::wstring DefaultConfigPath();

// Reads and parses the config file. Returns false on I/O or schema error.
bool LoadConfig(const std::wstring& path, AppConfig& out);

} // namespace ds
