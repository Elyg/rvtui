#pragma once

#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// "[x, y]", with ≈ when read from a reduced resolution.
std::string sampleCoord(const Sample& s);
/// The sample as name/value rows: what the inspector's cursor moves over and
/// `y` / `Y` copy.
std::vector<std::pair<std::string, std::string>> sampleItems(const Sample& s);
/// "(0.500, 0.250, 0.125, 1.000)", each number in its channel's colour.
ftxui::Element rgbaValues(const Sample& s);

/// [4] pixel inspector: the live readout under the mouse, and the picked
/// (ctrl+clicked) pixel as rows to move over and copy.
class InspectorPane
{
public:
	InspectorPane(ViewerState& state, AppContext& ctx);

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
		return m_state.m_focus == Focus::INSPECT;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}

	/// @param hover the sample under the mouse.
	[[nodiscard]] ftxui::Element render(const Sample& hover);
	/// Focused: move, y/Y copy, 4 closes.
	[[nodiscard]] bool event(const ftxui::Event& e);

private:
	ViewerState& m_state;
	AppContext& m_ctx;
	bool m_open = false;
	int m_cursor = 0; ///< row in the picked readout
	bool m_pendingG = false;
	ftxui::Box m_box{};
};

} // namespace rv
