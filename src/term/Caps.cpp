#include "term/Caps.h"

#include <sys/ioctl.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>

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

bool sshAncestor(int pid, const std::string& psTable)
{
	std::unordered_map<int, std::pair<int, std::string>> procs; // ppid, comm
	std::istringstream in(psTable);
	std::string line;
	while(std::getline(in, line))
	{
		std::istringstream ls(line);
		int p = 0, pp = 0;
		std::string comm;
		if(ls >> p >> pp && std::getline(ls >> std::ws, comm))
		{
			procs[p] = {pp, comm};
		}
	}
	// sshd, sshd-session (OpenSSH 9.8+), /usr/sbin/sshd (macOS: the path).
	for(int hops = 0; hops < 64 && pid > 1; ++hops)
	{
		const auto it = procs.find(pid);
		if(it == procs.end())
		{
			return false;
		}
		if(it->second.second.find("sshd") != std::string::npos)
		{
			return true;
		}
		pid = it->second.first;
	}
	return false;
}

std::string tmuxClientPids()
{
	return run("tmux list-clients -F '#{client_pid}' "
	           "-t \"$(tmux display -p '#{session_id}')\" 2>/dev/null");
}

bool tmuxClientOverSsh()
{
	return tmuxClientOverSsh(tmuxClientPids());
}

bool tmuxClientOverSsh(const std::string& pids)
{
	if(pids.empty())
	{
		return false;
	}
	const std::string table = run("ps -A -o pid=,ppid=,comm= 2>/dev/null");
	std::istringstream in(pids);
	int pid = 0;
	while(in >> pid)
	{
		if(sshAncestor(pid, table))
		{
			return true;
		}
	}
	return false;
}

Transfer autoTransfer(bool tmux, bool sshEnv, bool tmuxSshClient)
{
	// Over ssh the terminal is on another machine and can't see our shared
	// memory or temp files. Inside tmux the pane's SSH_* variables only say
	// how the server was started (over ssh once, attached at the desk now:
	// inline, many times slower); the clients attached say who is watching.
	if(tmux)
	{
		return tmuxSshClient ? Transfer::DIRECT : Transfer::TEMP_FILE;
	}
	return sshEnv ? Transfer::DIRECT : Transfer::SHARED_MEMORY;
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
			// Quiet when tmux can't answer (its socket in /tmp removed): it
			// would print "error connecting to …" over the terminal.
			std::string out =
			    tmuxDisplay("#{client_cell_width} #{client_cell_height}");
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
	if(transfer == "shm" || transfer == "file" || transfer == "direct")
	{
		caps.m_transfer = transfer == "shm"    ? Transfer::SHARED_MEMORY
		                  : transfer == "file" ? Transfer::TEMP_FILE
		                                       : Transfer::DIRECT;
	}
	else
	{
		caps.m_transfer =
		    autoTransfer(caps.m_tmux,
		                 !env("SSH_CONNECTION").empty() ||
		                     !env("SSH_TTY").empty(),
		                 kitty && caps.m_tmux && tmuxClientOverSsh());
	}
	refreshCellSize(caps);
	return caps;
}

} // namespace rv
