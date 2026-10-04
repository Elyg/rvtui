#include "term/Caps.h"

#include <sys/ioctl.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unistd.h>

namespace rv
{

namespace
{
// Output of a shell command, trailing whitespace trimmed ("" on failure).
std::string run(const char* cmd)
{
	std::string out;
	if(FILE* f = popen(cmd, "r"))
	{
		char buf[256];
		while(fgets(buf, sizeof buf, f))
		{
			out += buf;
		}
		pclose(f);
	}
	while(!out.empty() && (out.back() == '\n' || out.back() == ' '))
	{
		out.pop_back();
	}
	return out;
}

std::string env(const char* k)
{
	const char* v = std::getenv(k);
	return v ? v : "";
}
} // namespace

std::string tmuxDisplay(const std::string& format)
{
	return run(("tmux display -p '" + format + "' 2>/dev/null").c_str());
}

std::string tmuxOption(const std::string& name)
{
	return run(("tmux show -Apv " + name + " 2>/dev/null").c_str());
}

bool isKittyTerminal(const std::string& name)
{
	return name.find("kitty") != std::string::npos ||
	       name.find("ghostty") != std::string::npos;
}

const char* graphicsModeName(GraphicsMode m)
{
	return m == GraphicsMode::KITTY ? "kitty" : "halfblock";
}

const char* transferName(Transfer t)
{
	switch(t)
	{
		case Transfer::SHARED_MEMORY:
			return "shm";
		case Transfer::TEMP_FILE:
			return "file";
		case Transfer::DIRECT:
			break;
	}
	return "direct";
}

bool refreshCellSize(TermCaps& caps)
{
	struct winsize ws{};
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 ||
	   ws.ws_row == 0)
	{
		return false;
	}
	int w = 0, h = 0;
	if(caps.m_tmux)
	{
		// Inside tmux the pane's pixel size is tmux's estimate, not the outer
		// terminal's; the client's cell size is what Ghostty/kitty actually
		// draw with. Ask again only when the pane is resized (popen is slow).
		static unsigned short lastCols = 0, lastRows = 0;
		static int tmuxW = 0, tmuxH = 0;
		if(ws.ws_col != lastCols || ws.ws_row != lastRows)
		{
			lastCols = ws.ws_col;
			lastRows = ws.ws_row;
			std::string out = run(
			    "tmux display -p '#{client_cell_width} #{client_cell_height}'");
			if(std::sscanf(out.c_str(), "%d %d", &tmuxW, &tmuxH) != 2)
			{
				tmuxW = tmuxH = 0;
			}
		}
		w = tmuxW;
		h = tmuxH;
	}
	if((w <= 0 || h <= 0) && ws.ws_xpixel && ws.ws_ypixel)
	{
		w = ws.ws_xpixel / ws.ws_col;
		h = ws.ws_ypixel / ws.ws_row;
	}
	if(w <= 0 || h <= 0 || (w == caps.m_cellW && h == caps.m_cellH))
	{
		return false;
	}
	caps.m_cellW = w;
	caps.m_cellH = h;
	return true;
}

TermCaps detectCaps(const std::string& forced, const std::string& transfer)
{
	TermCaps caps;
	caps.m_tmux = !env("TMUX").empty();
	std::string term = env("TERM"), prog = env("TERM_PROGRAM");
	caps.m_terminalName = !prog.empty() ? prog : term;

	// Terminals implementing kitty graphics *with Unicode placeholders*.
	bool kitty = isKittyTerminal(term) || isKittyTerminal(prog) ||
	             !env("KITTY_WINDOW_ID").empty() ||
	             !env("GHOSTTY_RESOURCES_DIR").empty();
	// In tmux the pane's environment may not say (a server started from
	// elsewhere, e.g. over ssh); the attached client's terminal does.
	if(!kitty && caps.m_tmux)
	{
		kitty = isKittyTerminal(tmuxDisplay("#{client_termname}"));
	}
	if(forced == "kitty")
	{
		kitty = true;
	}
	else if(forced == "halfblock")
	{
		kitty = false;
	}
	// tmux drops kitty graphics escapes unless allow-passthrough is on; the
	// placeholder cells would then render blank, so fall back instead.
	if(kitty && caps.m_tmux && forced != "kitty")
	{
		std::string pt = tmuxOption("allow-passthrough");
		if(pt != "on" && pt != "all")
		{
			kitty = false;
			caps.m_note = "tmux passthrough off → half-blocks (--doctor)";
		}
	}
	caps.m_graphics = kitty ? GraphicsMode::KITTY : GraphicsMode::HALF_BLOCK;
	// Over ssh the terminal is on another machine and can't see our shared
	// memory. Inside tmux the pane's environment says how the session was
	// started, which is the best guess available.
	const bool remote =
	    !env("SSH_CONNECTION").empty() || !env("SSH_TTY").empty();
	if(transfer == "shm" || transfer == "file" || transfer == "direct")
	{
		caps.m_transfer = transfer == "shm"    ? Transfer::SHARED_MEMORY
		                  : transfer == "file" ? Transfer::TEMP_FILE
		                                       : Transfer::DIRECT;
	}
	else
	{
		caps.m_transfer = remote        ? Transfer::DIRECT
		                  : caps.m_tmux ? Transfer::TEMP_FILE
		                                : Transfer::SHARED_MEMORY;
	}
	refreshCellSize(caps);
	return caps;
}

} // namespace rv
