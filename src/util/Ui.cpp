#include "util/Ui.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <string>

using namespace ftxui;

namespace rv::ui
{

Element highlighted(std::string_view name,
                    std::span<const size_t> pos,
                    Decorator base)
{
	Elements runs;
	size_t i = 0, p = 0;
	while(i < name.size())
	{
		const bool hit = p < pos.size() && pos[p] == i;
		size_t j = i;
		while(j < name.size() && (p < pos.size() && pos[p] == j) == hit)
		{
			if(hit)
			{
				++p;
			}
			++j;
		}
		runs.push_back(text(std::string(name.substr(i, j - i))) |
		               (hit ? color(Color::Yellow) | bold | underlined : base));
		i = j;
	}
	return hbox(std::move(runs));
}

Element paneHints(std::string_view prefix,
                  std::initializer_list<PaneHint> hints)
{
	Elements parts{text(" ")};
	if(!prefix.empty())
	{
		parts.push_back(text(std::string(prefix) + " ") | dim);
	}
	for(const auto& [key, what] : hints)
	{
		parts.push_back(text("(") | dim);
		parts.push_back(text(std::string(key)) | bold);
		if(!what.empty())
		{
			parts.push_back(text(" " + std::string(what)) | dim);
		}
		parts.push_back(text(") ") | dim);
	}
	return hbox(std::move(parts));
}

Element statusLine(StatusLine line, int width)
{
	int used = 0;
	for(auto& el : line.m_status)
	{
		el->ComputeRequirement();
		used += el->requirement().min_x;
	}
	// A setup problem stays visible (yellow) until it is fixed.
	if(!line.m_warning.empty())
	{
		const std::string warn = " ⚠ " + std::string(line.m_warning) + " ";
		line.m_status.push_back(text(warn) | color(Color::Yellow));
		used += string_width(warn);
	}
	int room = width - used;
	// Same bar colour as the viewer HUD on top.
	Elements parts = std::move(line.m_status);
	const std::string tailText =
	    line.m_tail.empty() ? "" : " " + std::string(line.m_tail) + " ";
	const int tailW = string_width(tailText);
	if(!line.m_message.empty())
	{
		// The message replaces the hints; the tail only if it still fits.
		// Cut to the room left so the status on the left never shrinks.
		std::string msg;
		int msgW = 0;
		for(const auto& g :
		    Utf8ToGlyphs(" " + std::string(line.m_message) + " "))
		{
			if(g.empty())
			{
				continue; // the second cell of a wide glyph
			}
			const int w = std::max(1, string_width(g));
			if(msgW + w > std::max(0, room - 1))
			{
				msg += "…";
				++msgW;
				break;
			}
			msg += g;
			msgW += w;
		}
		const bool withTail = tailW > 0 && msgW + tailW <= room;
		// Clipboard results stand out: copied in yellow, failures in red.
		const Decorator tone =
		    line.m_message.starts_with("copied")        ? color(Color::Yellow)
		    : line.m_message.starts_with("copy failed") ? color(Color::Red)
		                                                : nothing;
		parts.push_back(text(msg) | bold | tone);
		parts.push_back(filler());
		if(withTail)
		{
			parts.push_back(text(tailText) | dim);
		}
		return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
	}
	parts.push_back(filler());
	const std::string hintText =
	    line.m_hints.empty() ? "" : std::string(line.m_hints) + " ";
	const bool withTail = tailW > 0 && tailW <= room;
	if(withTail)
	{
		room -= tailW;
	}
	if(!hintText.empty() && string_width(hintText) <= room)
	{
		parts.push_back(text(hintText) | dim);
	}
	if(withTail)
	{
		parts.push_back(text(tailText) | dim);
	}
	return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
}

std::string paneTitle(std::string_view head, int width)
{
	std::string title(head);
	for(int i = string_width(title); i < width; ++i)
	{
		title += "─";
	}
	return title;
}

} // namespace rv::ui
