#pragma once

#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace rv
{

/// [2] metadata column: the current frame's header (parts, attributes,
/// layers), scrollable, with a vim-style cursor and visual-line copy.
class MetaPane
{
public:
	/// One row of the pane.
	struct Item
	{
		enum class Kind
		{
			TITLE,
			PATH,
			PART,
			ATTRIBUTE,
			LAYERS,
			LAYER
		};
		Kind m_kind;
		std::string m_name;
		std::string m_value;
	};

	MetaPane(ViewerState& state, AppContext& ctx);

	bool isOpen() const noexcept
	{
		return m_open;
	}
	void setOpen(bool open) noexcept
	{
		m_open = open;
	}
	bool focused() const noexcept
	{
		return m_state.m_focus == Focus::META;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}
	/// Scroll by `rows` (wheel, `{` / `}`, PageUp / PageDown).
	void scroll(int rows) noexcept
	{
		m_scroll = std::max(0, m_scroll + rows);
	}
	/// Back to the top (a new set of images).
	void resetScroll() noexcept
	{
		m_scroll = 0;
	}

	/// The rows for `info`'s header.
	std::vector<Item> items(const ImageInfoPtr& info) const;
	[[nodiscard]] ftxui::Element render(const ImageInfoPtr& info);
	/// Focused: j/k gg G ^d/^u, v select, y/Y copy, 2 closes. Other keys
	/// (exposure…) fall through to the viewer.
	[[nodiscard]] bool event(const ftxui::Event& e, const ImageInfoPtr& info);

private:
	/// Cells before an attribute / layer name.
	static constexpr int NAME_INDENT = 2;

	void copy(const ImageInfoPtr& info, bool valuesOnly);
	/// Width of the name column (incl. the gap before the value).
	static int nameColumn(const std::vector<Item>& all);
	/// The screen lines of one row: one, cut to fit, or (`expand`, the
	/// focused cursor row) as many as its whole name and value take.
	ftxui::Elements itemLines(const Item& it, int nameCol, bool expand) const;

	ViewerState& m_state;
	AppContext& m_ctx;
	bool m_open = false;
	int m_cursor = 0;
	int m_anchor = -1; ///< visual-line selection start, -1 = none
	bool m_pendingG = false;
	int m_scroll = 0;   ///< first visible row
	ftxui::Box m_box{}; ///< where it was drawn (wheel scrolling, page size)
};

} // namespace rv
