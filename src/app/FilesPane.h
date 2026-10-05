#pragma once

#include "app/AnnotationsPane.h"
#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <map>
#include <string>

namespace rv
{

/// [3] open files: pick, reorder (J/K) and drop (x) the viewer's sources,
/// expand a sequence into its frames (e); `l` goes into a row's annotations.
class FilesPane
{
public:
	/// `e` expands a sequence into at most this many sources (tiles: the
	/// sheet shows ~180 at a time, see MAX_VISIBLE_TILES).
	static constexpr int MAX_EXPAND_FRAMES = 500;

	FilesPane(ViewerState& state, AppContext& ctx);

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
		return m_state.m_focus == Focus::FILES;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	/// Row of the cursor: -1 = the "global" row above the files.
	int cursor() const noexcept
	{
		return m_cursor;
	}
	AnnotationsPane& annotations() noexcept
	{
		return m_ann;
	}
	const AnnotationsPane& annotations() const noexcept
	{
		return m_ann;
	}
	/// Open and focus it, the cursor on the shown source.
	void show();
	/// A click at screen row `y` (over the pane): focus it, pick that row.
	void click(int y);

	[[nodiscard]] ftxui::Element render();
	[[nodiscard]] bool event(const ftxui::Event& e);

private:
	void moveSource(int from, int to);
	/// `e`: a sequence ⇄ one source per frame (like the browser).
	void toggleSourceExpand();

	ViewerState& m_state;
	AppContext& m_ctx;
	AnnotationsPane m_ann;
	bool m_open = false;
	int m_cursor = 0;
	bool m_clearPending = false; ///< `D` once asked; a second `D` clears
	/// Sequences expanded here, by sourceKey, to fold their frames back.
	std::map<std::string, Entry> m_expandedSources;
	ftxui::Box m_box{};
};

} // namespace rv
