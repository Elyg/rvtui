#include "app/App.h"

#include "image/Loader.h"
#include "term/Kitty.h"
#include "term/Output.h"
#include "util/Log.h"

#include <ftxui/component/component.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

namespace
{
// How often the disk is checked for changes.
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1000);
} // namespace

App::App(const AppOptions& opts, ScreenInteractive& screen)
    : m_opts(opts), m_screen(screen),
      m_caps(detectCaps(opts.m_graphics, opts.m_transfer)),
      m_kitty(m_caps.m_tmux, m_caps.m_transfer, &term::writeRaw),
      m_svc([this] { m_screen.PostEvent(Event::Custom); },
            static_cast<size_t>(opts.m_cacheBytes)),
      m_annPath(Annotations::defaultStatePath()),
      m_colour(ColourManager::defaultStatePath()),
      m_ctx{.m_svc = m_svc,
            .m_caps = m_caps,
            .m_kitty = m_kitty,
            .m_ann = m_ann,
            .m_colour = m_colour,
            .m_redraw = [this] { m_screen.PostEvent(Event::Custom); },
            .m_post = [this](std::function<void()> task)
            { m_screen.Post(std::move(task)); },
            .m_quit = [this] { m_screen.ExitLoopClosure()(); },
            .m_message = {}},
      m_browser(m_ctx,
                opts.m_chooserFile,
                [this](std::vector<Entry> entries, bool tile)
                {
	                m_viewer.open(std::move(entries), tile);
	                m_mode = Mode::VIEWER;
                }),
      m_viewer(m_ctx, [this] { m_mode = Mode::BROWSER; }),
      m_watcher(POLL_INTERVAL,
                [this](std::vector<fs::path> changed)
                {
	                m_screen.Post([this, changed = std::move(changed)]
	                              { filesChanged(changed); });
                })
{
	spdlog::info("terminal '{}', graphics {} ({}), cell {}x{}px, tmux {}",
	             m_caps.m_terminalName,
	             graphicsModeName(m_caps.m_graphics),
	             transferName(m_caps.m_transfer),
	             m_caps.m_cellW,
	             m_caps.m_cellH,
	             m_caps.m_tmux);
	if(!m_caps.m_note.empty())
	{
		spdlog::warn("{}", m_caps.m_note); // stays in the status bar
	}
	if(!m_annPath.empty() && m_ann.load(m_annPath))
	{
		spdlog::info("annotations loaded from {}", m_annPath.string());
	}
	m_colour.loadDefault(); // $OCIO, else the config picked last time
	if(!m_colour.error().empty())
	{
		m_ctx.m_message = "OCIO: " + m_colour.error();
	}

	std::error_code ec;
	const auto& paths = opts.m_paths;
	if(paths.empty() || (paths.size() == 1 && fs::is_directory(paths[0], ec)))
	{
		m_browser.openDirectory(
		    paths.empty() ? fs::current_path()
		                  : fs::absolute(paths[0], ec).lexically_normal());
	}
	else
	{
		std::vector<std::string> missing;
		std::vector<Entry> entries = entriesForArgs(paths, missing);
		if(entries.empty())
		{
			m_browser.openDirectory(fs::current_path());
			m_ctx.m_message = missing.empty() ? "no images to open"
			                                  : "not found: " + missing.front();
		}
		else
		{
			// Browse the first image's directory with the cursor on it (or
			// on the sequence it belongs to), so `q` lands somewhere useful.
			const fs::path first = entries.front().m_path;
			m_browser.openDirectory(first.parent_path(),
			                        entryForPath(first).m_name);
			m_viewer.open(std::move(entries));
			m_mode = Mode::VIEWER;
			if(!missing.empty())
			{
				m_ctx.m_message = "not found: " + missing.front();
			}
		}
	}
	updateWatch();
}

App::~App()
{
	m_watcher.stop();
	m_viewer.finishTyping();
	if(!m_annPath.empty() && m_ann.save(m_annPath))
	{
		spdlog::info("annotations saved to {}", m_annPath.string());
	}
	if(!m_opts.m_cwdFile.empty())
	{
		std::ofstream(m_opts.m_cwdFile) << m_browser.cwd().string();
	}
}

void App::updateWatch()
{
	std::vector<fs::path> dirs = m_browser.shownDirs();
	if(m_mode == Mode::VIEWER)
	{
		for(auto& d : m_viewer.shownDirs())
		{
			if(std::ranges::find(dirs, d) == dirs.end())
			{
				dirs.push_back(std::move(d));
			}
		}
	}
	if(dirs != m_watched)
	{
		m_watched = dirs;
		m_watcher.watch(std::move(dirs));
	}
}

void App::filesChanged(const std::vector<fs::path>& changed)
{
	bool redraw = m_browser.refresh();
	if(m_mode == Mode::VIEWER)
	{
		redraw |= m_viewer.rescan();
	}
	redraw |= m_svc.checkForChanges(changed);
	if(redraw)
	{
		m_screen.PostEvent(Event::Custom);
	}
}

Element App::render()
{
	if(refreshCellSize(m_caps))
	{
		spdlog::info("cell size now {}x{}px", m_caps.m_cellW, m_caps.m_cellH);
	}
	Element body =
	    m_mode == Mode::BROWSER ? m_browser.render() : m_viewer.render();
	if(m_help)
	{
		body = dbox({body, renderHelp() | clear_under | center});
	}
	updateWatch();
	if(m_caps.m_graphics == GraphicsMode::KITTY)
	{
		body = drawAfter(body, compactPlaceholders);
	}
	return body;
}

Element App::renderHelp()
{
	auto row = [](const std::string& k, const std::string& d)
	{
		return hbox(
		    {text(k) | bold | color(Color::Yellow) | size(WIDTH, EQUAL, 16),
		     text(d)});
	};
	auto section = [](const std::string& name)
	{ return text(name) | bold | color(Color::Cyan); };
	// Only the keys of the mode you are in.
	std::string title;
	Element body;
	if(m_mode == Mode::BROWSER)
	{
		title = " rvtui browser keys ";
		body = vbox({
		    row("j/k ↑/↓", "move"),
		    row("h/l ←/→", "parent / enter dir or view image"),
		    row("gg / G", "top / bottom"),
		    row("/", "fuzzy filter (Enter keep, Esc clear)"),
		    row("e", "expand sequence into frames / collapse"),
		    row(".", "toggle hidden files"),
		    row("space", "mark (view marked together)"),
		    row("o", "choose → --chooser-file, quit"),
		    row("q / Q", "quit"),
		});
	}
	else
	{
		title = " rvtui viewer keys ";
		Element left = vbox({
		    section("Image"),
		    row("[ / ]", "previous / next layer (AOV)"),
		    row("n / N", "next / previous image (marked files)"),
		    row("+ / - / wheel", "zoom"),
		    row("f / z", "fit / 1:1"),
		    row("hjkl ←↓↑→ HJKL", "pan (shift = faster)"),
		    row("t", "tile: all layers, or all marked images"),
		    row("  +- wheel hjkl", "tile: zoom / pan the sheet (f fit, z 1:1)"),
		    row("click", "select tile / focus pane"),
		    row("Enter (tile)", "open the selected tile"),
		    row("Ctrl+click", "pick colour (right click works too)"),
		    text(""),
		    section("Sequence"),
		    row("space", "play / pause"),
		    row(", .  < >", "step frame / first, last"),
		    row(":", "go to frame (file number, nearest)"),
		    row("F", "cycle playback fps"),
		    row("P", "playback: capped resolution (keeps up) / full res"),
		});
		Element right = vbox({
		    section("Display"),
		    row("c r g b a u", "colour / R / G / B / alpha / luma"),
		    row("e / E", "exposure -/+ 0.5 stop"),
		    row("y / Y", "gamma -/+ 0.1"),
		    row("s", "view transform (sRGB / OCIO view) / raw"),
		    row("w", "outlines (off): frame + data (dashed) / frame / off"),
		    row("w (tile)", "selection / frame + selection / frame / off"),
		    row("0", "reset exposure, gamma, channel"),
		    row("T", "add annotation text (burn-in style)"),
		    row("A", "show / hide annotations"),
		    text(""),
		    section("Panes"),
		    row("2 3 4 5 6", "metadata / files / inspector / layers / colour:"),
		    row("m o i /", "the same, as letters"),
		    row("", "open + focus; again while focused closes"),
		    row("1", "focus the image"),
		    row("Tab", "hide all panes / bring them back"),
		    row("  j k  y  Y", "inspector: move, copy line / value"),
		    row("  wheel { }", "metadata: scroll"),
		    row("  J / K  x", "files: move down / up, drop"),
		    row("  e", "files: sequence ⇄ its frames (up to 500)"),
		    row("  l / h", "files: into a row's annotations / back"),
		    row("  D D", "files: clear all annotations (in a row: its own)"),
		    row("  o O Enter x",
		        "annotations: add below / above, edit, delete"),
		    row("  J / K  g / s", "annotations: reorder, to global / source"),
		    row("  Tab ↑↓ C-n", "typing: next slot, history, [#key] / [#@key]"),
		    row("  j k gg G", "metadata: move (Ctrl-d/u half page)"),
		    row("  h/l  Enter", "colour: change the row / pick from a list"),
		    row("  v  y  Y", "select lines / copy line / copy value"),
		    text(""),
		    row("q / Esc", "back to browser"),
		    row("Q", "quit rvtui"),
		});
		// Two columns when they fit side by side, else one (scrollable).
		constexpr int TWO_COLUMN_WIDTH = 118;
		body = Terminal::Size().dimx >= TWO_COLUMN_WIDTH
		           ? hbox({left, text("   "), right})
		           : vbox({left, text(""), right});
	}
	// Scrollable when the terminal is still too short: j/k ↑/↓ or wheel.
	const int maxH = std::max(5, Terminal::Size().dimy - 6);
	m_helpScroll = std::clamp(m_helpScroll, 0, 100);
	return vbox({
	           text(title) | bold | hcenter,
	           separator(),
	           body | focusPositionRelative(0, m_helpScroll / 100.0f) |
	               vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, maxH),
	           separator(),
	           text(" ? / Esc close   j/k scroll") | dim,
	       }) |
	       border | bgcolor(Color::Black);
}

bool App::helpEvent(Event e)
{
	// j/k / arrows / wheel scroll the help (in 10% steps); any other key
	// closes it.
	int step = 0;
	if(e.is_mouse())
	{
		auto b = e.mouse().button;
		step = b == Mouse::WheelDown ? 1 : b == Mouse::WheelUp ? -1 : 0;
		if(step == 0)
		{
			return false;
		}
	}
	else if(e == Event::Character('j') || e == Event::ArrowDown)
	{
		step = 1;
	}
	else if(e == Event::Character('k') || e == Event::ArrowUp)
	{
		step = -1;
	}
	if(step != 0)
	{
		m_helpScroll = std::clamp(m_helpScroll + step * 10, 0, 100);
		return true;
	}
	m_help = false;
	return true;
}

bool App::onEvent(Event e)
{
	if(e == Event::Custom)
	{
		if(m_mode == Mode::VIEWER)
		{
			m_viewer.tick();
		}
		return true;
	}
	if(e.is_mouse())
	{
		m_viewer.setMouse(e.mouse().x, e.mouse().y);
	}
	if(m_help)
	{
		return helpEvent(e);
	}
	// Shift+Q quits from anywhere (except while typing a filter or text).
	const bool typing = m_browser.typing() || m_viewer.typing();
	if(e == Event::Character('Q') && !typing)
	{
		m_ctx.m_quit();
		return true;
	}
	if(e == Event::Character('?') && !typing)
	{
		m_help = true;
		m_helpScroll = 0;
		return true;
	}
	if(!e.is_mouse())
	{
		m_ctx.m_message.clear(); // messages stay until the next key, not mouse
	}
	return m_mode == Mode::BROWSER ? m_browser.event(e) : m_viewer.event(e);
}

int runApp(const AppOptions& opts)
{
	log::redirectToFile();
	auto screen = ScreenInteractive::Fullscreen();
	// Placeholder cells encode the kitty image id in a 24-bit foreground
	// colour, so FTXUI must never downgrade colours. Ghostty/kitty are
	// truecolour anyway.
	if(detectCaps(opts.m_graphics).m_graphics == GraphicsMode::KITTY)
	{
		Terminal::SetColorSupport(Terminal::Color::TrueColor);
	}

	{
		App app(opts, screen);
		auto root = Renderer([&] { return app.render(); });
		root |= CatchEvent([&](Event e) { return app.onEvent(e); });
		screen.Loop(root);
	}
	spdlog::info("exit");
	return 0;
}

} // namespace rv
