#pragma once

#include "app/Annotations.h"
#include "app/Player.h"
#include "image/Render.h"
#include "image/Sequence.h"

#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// One image or sequence open in the viewer.
struct Source
{
	Entry m_entry;
	/// Frame `i` (clamped); the file itself for a single image.
	std::filesystem::path frame(int i) const;
	int frameCount() const;
	std::string frameLabel(int i) const; ///< number from the file name
	/// The frame whose file number is `number`, else the nearest one (the
	/// earlier on a tie); 0 for a single image.
	int indexForFrameNumber(int number) const;
};

/// Key of a source in the annotations state file: absolute dir / display name
/// ("/shots/a.####.exr").
std::string sourceKey(const Source& s);

/// Keyboard focus inside the viewer, lazygit style: the image [1] or a pane.
enum class Focus
{
	IMAGE,
	META,
	FILES,
	INSPECT,
	LAYERS,
	COLOUR
};

/// A pixel read from whatever image is under a terminal cell (the main view, or
/// a tile), for the inspector and the click colour picker.
struct Sample
{
	enum class State
	{
		NONE,    ///< no image under the cell
		LOADING, ///< no pixels decoded yet
		OUTSIDE, ///< outside the data window
		OK
	};
	State m_state = State::NONE;
	std::filesystem::path m_path; ///< the frame read
	int m_x = 0, m_y = 0; ///< image (file) coordinates: y down from the top
	Box2i m_frame;        ///< the display window, for nukeX() / nukeY()
	bool m_exact = false; ///< full resolution (else what is on screen, ≈)
	std::string m_layer;
	std::vector<std::pair<std::string, float>> m_values;
	float m_luma = 0.0f;
	int m_r = 0, m_g = 0, m_b = 0;  ///< display-transformed, 8-bit
	float m_rgba[4] = {0, 0, 0, 1}; ///< file values (linear), for readouts
	bool m_hasAlpha = false;

	/// Nuke's coordinates, which every readout shows: (0, 0) at the frame's
	/// bottom-left, y up. File coordinates when the frame is unknown.
	int nukeX() const noexcept
	{
		return m_frame.width() > 0 ? m_x - m_frame.m_x0 : m_x;
	}
	int nukeY() const noexcept
	{
		return m_frame.height() > 0 ? m_frame.m_y1 - m_y : m_y;
	}
};

/// What the viewer's panes read and change. Owned by Viewer; the panes hold a
/// reference, never a pointer back to the Viewer.
struct ViewerState
{
	std::vector<Source> m_sources;
	int m_current = 0; ///< active source
	int m_frame = 0;
	/// `I` / `O`: where playback starts and ends (frame indices).
	std::optional<int> m_in, m_out;
	std::string m_layerLabel;
	DisplayParams m_disp;
	ViewParams m_view;
	Focus m_focus = Focus::IMAGE;
	std::optional<Sample> m_picked; ///< last colour clicked (until viewer exit)
	/// The side columns' widths as fractions of the terminal, once their
	/// dividers have been dragged; 0 = the default (see leftPanelWidth()).
	double m_leftFrac = 0, m_rightFrac = 0;

	/// Width of the left column (layers / colour / files): 20% of the
	/// terminal, at least 26 cells, until its divider is dragged elsewhere.
	int leftPanelWidth() const;
	/// Width of the right column (inspector / metadata): 30%, at least 32
	/// cells, until dragged. Dragged columns leave the image some room; the
	/// left one wins when both want it.
	int sidePanelWidth() const;
	/// The current frame of the active source.
	std::filesystem::path currentFramePath() const;
	int frameCount() const; ///< the longest source's
	/// The frames playback loops over: in to out, each defaulting to the
	/// sequence's end; the whole sequence if they cross.
	FrameRange playRange() const;
	/// The sequence `:` frame numbers refer to: the active source, else the
	/// first sequence open; nullptr when none is a sequence.
	const Source* numberedSource() const;
	/// The lines source `i` adds (nullptr: none, or no such source).
	const AnnotationSet* sourceAnnotations(const Annotations& ann, int i) const;
};

/// Whether cell (x, y) is inside `b`.
inline bool inside(const ftxui::Box& b, int x, int y) noexcept
{
	return x >= b.x_min && x <= b.x_max && y >= b.y_min && y <= b.y_max;
}

} // namespace rv
