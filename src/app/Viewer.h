#pragma once

#include "app/AppContext.h"
#include "app/FilesPane.h"
#include "app/InspectorPane.h"
#include "app/LayersPane.h"
#include "app/MetaPane.h"
#include "app/Player.h"
#include "app/ViewerState.h"
#include "image/Overlay.h"
#include "term/ImageView.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// The image viewer: one image (or a contact sheet of tiles), display
/// controls, sequence playback and the side panes. Reports leaving through
/// `onClose`; knows nothing of the browser.
class Viewer
{
public:
	Viewer(AppContext& ctx, std::function<void()> onClose);
	Viewer(const Viewer&) = delete;
	Viewer& operator=(const Viewer&) = delete;

	/// Show `entries` (`tile`: as a contact sheet), from the first frame, fit.
	void open(std::vector<Entry> entries, bool tile = false);
	/// Handle an event; true when consumed.
	[[nodiscard]] bool event(ftxui::Event e);
	[[nodiscard]] ftxui::Element render();
	/// A playback tick (the ticker woke the UI). True if the frame advanced.
	bool tick();

	bool playing() const noexcept
	{
		return m_player.playing();
	}
	/// An annotation line is being typed (keys are text, not commands).
	bool typing() const noexcept
	{
		return m_files.annotations().editing();
	}
	/// Commit a line being typed (before the annotations are saved).
	void finishTyping()
	{
		m_files.annotations().finishEdit(true);
	}
	/// Where the mouse last was, for the inspector's live readout.
	void setMouse(int x, int y) noexcept
	{
		m_mouseX = x;
		m_mouseY = y;
	}

	/// Re-scan the open sequences for frames added or removed. True if any
	/// changed.
	bool rescan();
	/// The directories holding the open images.
	std::vector<std::filesystem::path> shownDirs() const;

	const ViewerState& state() const noexcept
	{
		return m_state;
	}
	bool tiled() const noexcept
	{
		return m_tile;
	}
	const Player& player() const noexcept
	{
		return m_player;
	}

private:
	/// `q`: leave the viewer (playback stops).
	void close();

	// --- view ---
	int reduceFor(const ImageInfo& info, const ImageSlot& slot) const;
	/// Pixels each image slot may send per draw (kitty): the budget split
	/// between tiles, and less while playing.
	int pixelCap() const;
	/// The slot playback decodes for: the first tile, or the main view.
	const ImageSlot& playbackSlot() const;
	/// Whether source `i` is on screen (every source when tiled).
	bool showsSource(int i) const;
	void cycleLayer(const ImageInfoPtr& info, int delta);
	void zoomBy(double factor,
	            std::optional<std::pair<double, double>> anchor = std::nullopt);
	void pan(double dxCells, double dyCells);
	/// `f`: fit the image, into the area left of the side panes if open.
	void fitView(const ImageInfoPtr& info);
	void setFrame(int f);
	void togglePlay();
	void prefetchFrame(int f);
	/// Start the slots on frame `f` (decoded already) before it is due.
	void prepareAhead(int f);
	/// The pixel under terminal cell (x, y), in the view or a tile.
	Sample sampleAt(int cellX, int cellY);

	// --- panes ---
	/// Right column: inspector / metadata. Left column: files / layers.
	bool rightPanelOpen() const noexcept
	{
		return m_inspector.isOpen() || m_meta.isOpen();
	}
	bool leftPanelOpen() const noexcept
	{
		return m_files.isOpen() || m_layers.isOpen();
	}
	bool sidePanelOpen() const noexcept
	{
		return rightPanelOpen() || leftPanelOpen();
	}
	/// Terminal columns the open side columns cover (incl. their separator).
	int coveredLeft() const
	{
		return leftPanelOpen() ? leftPanelWidth() + 1 : 0;
	}
	int coveredRight() const
	{
		return rightPanelOpen() ? sidePanelWidth() + 1 : 0;
	}
	/// `Tab`: hide every open pane; the next `Tab` brings them back.
	void togglePanes();

	// --- annotations ---
	/// Placeholder values for one image (frame path, its header, layer).
	KeyLookup keyLookup(int source,
	                    const std::filesystem::path& frame,
	                    const ImageInfoPtr& info,
	                    const std::string& layer) const;
	/// Resolved lines of `sets` (in order, appended per slot); empty when the
	/// overlay is hidden.
	OverlayText overlayText(std::initializer_list<const AnnotationSet*> sets,
	                        const KeyLookup& lookup) const;

	// --- events / drawing ---
	bool mouseEvent(ftxui::Event e, const ImageInfoPtr& info);
	bool keyEvent(const ftxui::Event& e, const ImageInfoPtr& info);
	ftxui::Element renderHud(const ImageInfoPtr& info);
	ftxui::Element renderTiles(const ImageInfoPtr& info, int width);
	ftxui::Element renderStatus(const ImageInfoPtr& info);

	AppContext& m_ctx;
	std::function<void()> m_onClose;
	ViewerState m_state;
	bool m_tile = false;
	bool m_tileSelection = true;   ///< dashed highlight on the selected tile
	bool m_fitBesidePanel = false; ///< `f` with a pane open: keep re-fitting
	int m_mouseX = -1, m_mouseY = -1;

	ImageSlotPtr m_viewSlot;
	std::vector<ImageSlotPtr> m_tileSlots;
	/// What each tile shows (parallel to m_tileSlots), for the inspector.
	struct TileRef
	{
		std::filesystem::path m_path;
		std::string m_layer;
	};
	std::vector<TileRef> m_tileRefs;

	MetaPane m_meta;
	FilesPane m_files;
	InspectorPane m_inspector;
	LayersPane m_layers;
	ftxui::Box m_sideBox{}; ///< the right column (clicks there skip the image)
	ftxui::Box m_leftBox{}; ///< the left column
	struct HiddenPanes
	{
		bool m_meta = false, m_files = false, m_inspector = false,
		     m_layers = false;
		Focus m_focus = Focus::IMAGE;
	};
	std::optional<HiddenPanes> m_hiddenPanes;

	Player m_player; ///< last: its ticker stops before the rest goes
};

} // namespace rv
