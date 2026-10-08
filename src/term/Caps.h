#pragma once

#include <string>

namespace rv
{

/// How images are drawn.
enum class GraphicsMode
{
	KITTY,     ///< kitty graphics with Unicode placeholders
	HALF_BLOCK ///< ▀ cells, two pixels each
};

/// How kitty image pixels reach the terminal.
enum class Transfer
{
	DIRECT,        ///< in the escapes: zlib + base64 through the pty (ssh)
	SHARED_MEMORY, ///< t=s: raw RGBA in POSIX shared memory, the name in the pipe
	/// t=f: raw RGBA in a temp file. For tmux: every attached client reads
	/// it, while the first to read shared memory unlinks it and leaves the
	/// other clients blank.
	TEMP_FILE
};

/// What the terminal can do, from detectCaps().
struct TermCaps
{
	GraphicsMode m_graphics = GraphicsMode::HALF_BLOCK;
	Transfer m_transfer = Transfer::DIRECT;
	/// Bumped to send every picture again: tmux doesn't replay them to a
	/// terminal that attaches later.
	int m_resend = 0;
	bool m_tmux = false;
	int m_cellW = 10, m_cellH = 20; ///< pixels per cell
	std::string m_terminalName;
	/// A misconfiguration worth a lasting warning (e.g. kitty graphics off
	/// because tmux has no allow-passthrough). Short; `rvtui --doctor` has the
	/// details. Empty when nothing to report.
	std::string m_note;
};

/// `tmux display -p <format>` for the current client ("" on failure).
std::string tmuxDisplay(const std::string& format);
/// A tmux option as the current pane sees it (`tmux show -Apv <name>`).
std::string tmuxOption(const std::string& name);
/// Whether process `pid` descends from sshd (an ssh login), by `psTable`:
/// `ps -A -o pid=,ppid=,comm=` output.
bool sshAncestor(int pid, const std::string& psTable);
/// Whether a client attached to this pane's tmux session came in over ssh.
/// The pane's own environment can't tell when the server was started on
/// this machine (its desktop) and then also attached to from elsewhere: that
/// client's terminal can't read our temp files or shared memory.
bool tmuxClientOverSsh();
/// The pids of the clients attached to this pane's tmux session, one a line.
std::string tmuxClientPids();
/// tmuxClientOverSsh() for these tmuxClientPids().
bool tmuxClientOverSsh(const std::string& pids);
/// What `--transfer auto` picks. `sshEnv`: SSH_CONNECTION / SSH_TTY set;
/// `tmuxSshClient`: tmuxClientOverSsh(). In tmux only the clients count:
/// direct with one over ssh, else temp files (each terminal attached reads
/// them; shared memory goes once read). Outside it: direct over ssh, else
/// shared memory.
Transfer autoTransfer(bool tmux, bool sshEnv, bool tmuxSshClient);
/// A TERM / TERM_PROGRAM value of a terminal with kitty graphics + Unicode
/// placeholders (ghostty, kitty).
bool isKittyTerminal(const std::string& name);

/// `forced` is "auto" | "kitty" | "halfblock". `transfer` is "auto" | "direct"
/// | "shm" | "file"; auto: see autoTransfer().
TermCaps detectCaps(const std::string& forced = "auto",
                    const std::string& transfer = "auto");

/// Re-read the cell pixel size (call on resize / font change). Returns true if
/// changed.
bool refreshCellSize(TermCaps& caps);

const char* graphicsModeName(GraphicsMode m); ///< "kitty" | "halfblock"
const char* transferName(Transfer t);         ///< "direct" | "shm" | "file"

} // namespace rv
