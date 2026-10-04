#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace rv::log
{

/// Plain stdout logger (pattern "%v") for user-facing CLI output such as --dump.
spdlog::logger& out();

/// Default logger → stderr.
void initConsole();
/// Call before running the TUI: from then on everything goes to `file` (the
/// terminal belongs to FTXUI). Empty `file` → $RVTUI_LOG or
/// ~/.cache/rvtui/rvtui.log.
void redirectToFile(std::string file = {});

} // namespace rv::log
