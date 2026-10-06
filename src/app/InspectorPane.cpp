#include "app/InspectorPane.h"

#include "util/Ui.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

using namespace ftxui;

namespace rv
{

// "(0.500, 0.250, 0.125, 1.000)", each number in its channel's colour (as
// the HUD's R G B A letters). Alpha is always shown; dim 1.000 when the
// image has none.
Element rgbaValues(const Sample& s)
{
	const Color colors[] = {Color::Red, Color::Green, Color::Blue, Color::White};
	Elements parts{text("(") | dim};
	for(int c = 0; c < 4; ++c)
	{
		if(c)
		{
			parts.push_back(text(", ") | dim);
		}
		const bool implied = c == 3 && !s.m_hasAlpha;
		parts.push_back(text(fmt::format("{:.3f}", s.m_rgba[c])) |
		                (implied ? dim : color(colors[c])));
	}
	parts.push_back(text(")") | dim);
	return hbox(std::move(parts));
}

std::string sampleCoord(const Sample& s)
{
	return fmt::format("{}[{}, {}]", s.m_exact ? "" : "≈ ", s.m_x, s.m_y);
}

std::vector<std::pair<std::string, std::string>> sampleItems(const Sample& s)
{
	std::vector<std::pair<std::string, std::string>> items{
	    {"pixel", fmt::format("[{}, {}]", s.m_x, s.m_y)},
	    {"layer", s.m_layer},
	    {"rgba",
	     fmt::format("({:.6g}, {:.6g}, {:.6g}, {:.6g})",
	                 s.m_rgba[0],
	                 s.m_rgba[1],
	                 s.m_rgba[2],
	                 s.m_rgba[3])},
	    {"display", fmt::format("#{:02x}{:02x}{:02x}", s.m_r, s.m_g, s.m_b)},
	};
	for(const auto& [name, v] : s.m_values)
	{
		items.emplace_back(name, fmt::format("{:.6g}", v));
	}
	items.emplace_back("luma", fmt::format("{:.6g}", s.m_luma));
	if(s.m_layer.empty()) // single-layer image: nothing to say
	{
		std::erase_if(items,
		              [](const auto& it) { return it.first == "layer"; });
	}
	return items;
}

InspectorPane::InspectorPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx)
{
}

Element InspectorPane::render(const Sample& s)
{
	using St = Sample::State;
	const bool focus = focused();
	auto swatch = [](const Sample& s)
	{
		return vbox({text(""), text("")}) |
		       bgcolor(Color::RGB(s.m_r, s.m_g, s.m_b)) | xflex;
	};
	auto valueRow = [](const std::string& name, Element value)
	{ return hbox({text(" " + name) | bold | size(WIDTH, EQUAL, 10), value}); };

	Elements rows{text(ui::paneTitle("─[4]─Inspector",
	                                 m_state.sidePanelWidth())) |
	                  (focus ? color(Color::Green) | bold : dim),
	              text(" hover") | bold | color(Color::Yellow)};

	// Live readout under the mouse.
	switch(s.m_state)
	{
		case St::NONE:
			rows.push_back(text(" hover the image") | dim);
			break;
		case St::LOADING:
			rows.push_back(text(" " + sampleCoord(s) + "  loading…") | dim);
			break;
		case St::OUTSIDE:
			rows.push_back(text(" " + sampleCoord(s)) | bold);
			rows.push_back(text(" outside the data window") | dim);
			break;
		case St::OK:
			rows.push_back(swatch(s));
			rows.push_back(hbox({text(" "), rgbaValues(s)}));
			rows.push_back(hbox({text(" " + sampleCoord(s)) | bold,
			                     text("  " + s.m_layer) | color(Color::Cyan)}));
			break;
	}

	// Picked pixel: navigable (4), copyable (y / Y).
	rows.push_back(separatorLight());
	rows.push_back(text(" picked (ctrl+click)") | bold | color(Color::Yellow));
	if(const auto& picked = m_state.m_picked)
	{
		rows.push_back(swatch(*picked));
		const auto items = sampleItems(*picked);
		m_cursor = std::clamp(m_cursor, 0, static_cast<int>(items.size()) - 1);
		for(size_t i = 0; i < items.size(); ++i)
		{
			const auto& [name, value] = items[i];
			Element v = name == "rgba"    ? rgbaValues(*picked)
			            : name == "pixel" ? text(sampleCoord(*picked))
			            : name == "layer" ? text(value) | color(Color::Cyan)
			                              : text(value);
			Element row = valueRow(name, v);
			if(focus && static_cast<int>(i) == m_cursor)
			{
				row = row | inverted;
			}
			rows.push_back(row);
		}
	}
	else
	{
		rows.push_back(text(" ctrl+click the image") | dim);
	}
	rows.push_back(focus ? ui::paneHints("",
	                                     {{"y", "copy line"},
	                                      {"Y", "copy value"},
	                                      {"4", "close"}})
	                     : ui::paneHints("", {{"4", "focus"}}));
	return vbox(std::move(rows)) | reflect(m_box);
}

bool InspectorPane::event(const Event& e)
{
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	const auto& picked = m_state.m_picked;
	const int n = picked ? static_cast<int>(sampleItems(*picked).size()) : 0;
	const bool wasG = m_pendingG;
	m_pendingG = false;
	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(std::max(0, n - 1), m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
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
		m_cursor = std::max(0, n - 1);
	}
	else if(ch("y") || ch("Y"))
	{
		if(!picked)
		{
			m_ctx.m_message = "ctrl+click the image to pick a pixel first";
			return true;
		}
		const auto items = sampleItems(*picked);
		const auto& [name, value] = items[std::clamp(m_cursor, 0, n - 1)];
		m_ctx.copyText(ch("y") ? name + ": " + value : value);
	}
	else if(ch("1") || ch("q") || ch("h") || e == Event::Escape)
	{
		m_state.m_focus = Focus::IMAGE;
	}
	else if(ch("4"))
	{
		m_open = false; // focused: its number closes it
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false;
	}
	return true;
}

} // namespace rv
