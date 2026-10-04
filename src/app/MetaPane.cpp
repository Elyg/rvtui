#include "app/MetaPane.h"

#include "util/Clipboard.h"
#include "util/Ui.h"

#include <ftxui/component/event.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

MetaPane::MetaPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx)
{
}

std::vector<MetaPane::Item> MetaPane::items(const ImageInfoPtr& info) const
{
	using K = Item::Kind;
	std::vector<Item> items;
	if(!info)
	{
		return items;
	}
	items.push_back(
	    {K::TITLE, m_state.currentFramePath().filename().string(), ""});
	items.push_back({K::PATH, "path", m_state.currentFramePath().string()});
	for(size_t p = 0; p < info->m_parts.size(); ++p)
	{
		const auto& part = info->m_parts[p];
		items.push_back({K::PART, fmt::format("part {}", p), part.m_name});
		for(const auto& a : part.m_attributes)
		{
			items.push_back({K::ATTRIBUTE, a.m_name, a.m_value});
		}
	}
	items.push_back({K::LAYERS, "layers", ""});
	for(const auto& l : info->m_layers)
	{
		std::string chans;
		for(const auto& c : l.m_channels)
		{
			chans += (chans.empty() ? "" : " ") + LayerInfo::shortName(c);
		}
		items.push_back({K::LAYER, l.label(), chans});
	}
	return items;
}

Element MetaPane::render(const ImageInfoPtr& info)
{
	using K = Item::Kind;
	const auto all = items(info);
	const int total = static_cast<int>(all.size());
	const bool focus = focused();
	m_cursor = std::clamp(m_cursor, 0, std::max(0, total - 1));

	// Keep the cursor on screen (rows from the last layout, minus the title
	// and footer).
	const int rows = std::max(1, m_box.y_max - m_box.y_min - 1);
	if(focus)
	{
		if(m_cursor < m_scroll)
		{
			m_scroll = m_cursor;
		}
		else if(m_cursor >= m_scroll + rows)
		{
			m_scroll = m_cursor - rows + 1;
		}
	}
	m_scroll = std::clamp(m_scroll, 0, std::max(0, total - 1));

	const int selLo = m_anchor < 0 ? m_cursor : std::min(m_anchor, m_cursor);
	const int selHi = m_anchor < 0 ? m_cursor : std::max(m_anchor, m_cursor);
	Elements lines;
	for(int i = m_scroll; i < total; ++i)
	{
		const Item& it = all[i];
		Element line;
		switch(it.m_kind)
		{
			case K::TITLE:
				line = text(" " + it.m_name) | bold;
				break;
			case K::PATH:
			{
				// Keep the tail (the interesting end) when it does not fit.
				const size_t room =
				    static_cast<size_t>(std::max(8, sidePanelWidth() - 3));
				std::string p = it.m_value;
				if(p.size() > room)
				{
					p = "…" + p.substr(p.size() - room + 1);
				}
				line = text(" " + p) | dim;
				break;
			}
			case K::PART:
				line = text(" " + it.m_name +
				            (it.m_value.empty() ? "" : " · " + it.m_value)) |
				       bold | color(Color::Cyan);
				break;
			case K::LAYERS:
				line = text(" layers") | bold | color(Color::Cyan);
				break;
			case K::ATTRIBUTE:
				line = hbox({
				    text("  " + it.m_name) | color(Color::Yellow) |
				        size(WIDTH, EQUAL, 20),
				    text(it.m_value),
				});
				break;
			case K::LAYER:
				line = hbox({
				    text("  " + it.m_name) |
				        (it.m_name == m_state.m_layerLabel ? bold : nothing) |
				        size(WIDTH, EQUAL, 20),
				    text(it.m_value) | dim,
				});
				break;
		}
		if(focus && i >= selLo && i <= selHi)
		{
			line = line | inverted;
		}
		lines.push_back(line);
	}
	Element footer =
	    focus
	        ? ui::paneHints(fmt::format("{}/{}", m_cursor + 1, total),
	                        {{"v", "select"}, {"y/Y", "copy"}, {"2", "close"}})
	        : ui::paneHints(fmt::format("{}/{}", m_scroll + 1, total),
	                        {{"2", "focus"}});
	// lazygit-style title: `2` focuses this pane.
	const std::string title = ui::paneTitle("─[2]─Metadata", sidePanelWidth());
	return vbox({
	           text(title) | (focus ? color(Color::Green) | bold : dim),
	           vbox(std::move(lines)) | flex,
	           footer,
	       }) |
	       reflect(m_box);
}

void MetaPane::copy(const ImageInfoPtr& info, bool valuesOnly)
{
	using K = Item::Kind;
	const auto all = items(info);
	if(all.empty())
	{
		return;
	}
	int lo = m_anchor < 0 ? m_cursor : std::min(m_anchor, m_cursor);
	int hi = m_anchor < 0 ? m_cursor : std::max(m_anchor, m_cursor);
	hi = std::min(hi, static_cast<int>(all.size()) - 1);
	std::string out;
	for(int i = lo; i <= hi; ++i)
	{
		const Item& it = all[i];
		std::string line;
		if(valuesOnly)
		{
			line = it.m_value.empty() ? it.m_name : it.m_value;
		}
		else if(it.m_kind == K::ATTRIBUTE || it.m_kind == K::LAYER)
		{
			line = it.m_name + ": " + it.m_value;
		}
		else
		{
			line = it.m_name + (it.m_value.empty() ? "" : " " + it.m_value);
		}
		out += line + "\n";
	}
	if(hi == lo && !out.empty())
	{
		out.pop_back(); // a single line copys without the newline
	}
	int n = hi - lo + 1;
	if(!copyToClipboard(out, m_ctx.m_caps.m_tmux))
	{
		m_ctx.m_message = "copy failed";
	}
	else if(n == 1)
	{
		constexpr size_t MAX_SHOWN = 60;
		m_ctx.m_message =
		    "copied: " +
		    (out.size() > MAX_SHOWN ? out.substr(0, MAX_SHOWN - 1) + "…" : out);
	}
	else
	{
		m_ctx.m_message = fmt::format("copied {} lines", n);
	}
	m_anchor = -1;
}

bool MetaPane::event(const Event& e, const ImageInfoPtr& info)
{
	const int total = static_cast<int>(items(info).size());
	const int half = std::max(1, (m_box.y_max - m_box.y_min) / 2);
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	bool wasG = m_pendingG;
	m_pendingG = false;

	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(total - 1, m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(e == Event::CtrlD || e == Event::PageDown)
	{
		m_cursor = std::min(total - 1, m_cursor + half);
	}
	else if(e == Event::CtrlU || e == Event::PageUp)
	{
		m_cursor = std::max(0, m_cursor - half);
	}
	else if(ch("g"))
	{
		if(wasG)
		{
			m_cursor = 0;
		}
		else
		{
			m_pendingG = true;
		}
	}
	else if(ch("G"))
	{
		m_cursor = std::max(0, total - 1);
	}
	else if(ch("v") || ch("V"))
	{
		m_anchor = m_anchor < 0 ? m_cursor : -1;
	}
	else if(ch("y"))
	{
		copy(info, false);
	}
	else if(ch("Y"))
	{
		copy(info, true);
	}
	else if(e == Event::Escape)
	{
		if(m_anchor >= 0)
		{
			m_anchor = -1;
		}
		else
		{
			m_state.m_focus = Focus::IMAGE;
		}
	}
	else if(e == Event::CtrlH || ch("q") || ch("1"))
	{
		m_anchor = -1;
		m_state.m_focus = Focus::IMAGE;
	}
	else if(e == Event::CtrlL)
	{
		m_ctx.tmuxSelectPane('R');
	}
	else if(ch("2"))
	{
		m_anchor = -1; // focused: its number closes it
		m_open = false;
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false; // e.g. exposure keys still reach the viewer
	}
	return true;
}

} // namespace rv
