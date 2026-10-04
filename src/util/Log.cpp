#include "util/Log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <cstdlib>
#include <filesystem>

namespace rv::log
{

spdlog::logger& out()
{
	static auto logger = []
	{
		auto l = spdlog::stdout_logger_mt("out");
		l->set_pattern("%v");
		return l;
	}();
	return *logger;
}

void initConsole()
{
	auto l = spdlog::stderr_logger_mt("rvtui");
	l->set_pattern("rvtui: %^%l%$: %v");
	spdlog::set_default_logger(l);
}

void redirectToFile(std::string file)
{
	if(file.empty())
	{
		if(const char* env = std::getenv("RVTUI_LOG"))
		{
			file = env;
		}
		else if(const char* home = std::getenv("HOME"))
		{
			file = std::string(home) + "/.cache/rvtui/rvtui.log";
		}
		else
		{
			file = "/tmp/rvtui.log";
		}
	}
	std::error_code ec;
	std::filesystem::create_directories(
	    std::filesystem::path(file).parent_path(), ec);
	try
	{
		// Kept across runs (each line carries the pid), capped at 2 x 2 MB.
		auto l =
		    spdlog::rotating_logger_mt("rvtui-file", file, 2 * 1024 * 1024, 1);
		l->set_pattern("[%m-%d %H:%M:%S.%e] [%l] [%P] %v"); // %P: pid per run
		l->flush_on(spdlog::level::info);
		// RVTUI_LOG_LEVEL=debug for per-frame detail.
		if(const char* lvl = std::getenv("RVTUI_LOG_LEVEL"))
		{
			l->set_level(spdlog::level::from_str(lvl));
			l->flush_on(l->level());
		}
		spdlog::set_default_logger(l);
	}
	catch(const spdlog::spdlog_ex&)
	{
		spdlog::set_level(spdlog::level::off); // never write onto the TUI
	}
}

} // namespace rv::log
