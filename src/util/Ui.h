#pragma once

#include <ftxui/dom/elements.hpp>

#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

namespace rv::ui
{

/// `name` with the bytes at `pos` (fuzzyMatch hits) highlighted.
[[nodiscard]] ftxui::Element highlighted(std::string_view name,
                                         std::span<const size_t> pos,
                                         ftxui::Decorator base);

/// A pane footer hint: key, what it does.
using PaneHint = std::pair<std::string_view, std::string_view>;
/// Pane footer: an optional dim prefix (position), then "(key what)" hints
/// with the key in bold.
[[nodiscard]] ftxui::Element paneHints(std::string_view prefix,
                                       std::initializer_list<PaneHint> hints);

/// Bottom bar, one line of `width` cells, by priority: `status` (frame / fps)
/// always on the left, then `warning` (a setup problem, yellow) if any; a
/// message takes the hints' place while it shows; `tail` (cache) dim at the
/// right end. Short of room, hints go first, then the tail; the message is
/// truncated last (with …).
struct StatusLine
{
	std::string_view m_message;
	std::string_view m_hints;
	ftxui::Elements m_status;
	std::string_view m_tail;
	std::string_view m_warning;
};
/// Render `line` (see StatusLine) in `width` cells.
[[nodiscard]] ftxui::Element statusLine(StatusLine line, int width);

/// "─[n]─Name───…" pane title, `width` cells wide.
[[nodiscard]] std::string paneTitle(std::string_view head, int width);

} // namespace rv::ui
