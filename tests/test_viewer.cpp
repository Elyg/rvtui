#include "TestContext.h"
#include "app/Sheet.h"
#include "app/Viewer.h"
#include "image/Sequence.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

using namespace rv;
using namespace rvtest;
using ftxui::Event;

namespace
{

// A multi-layer image and a 3-frame sequence (8x4 px each), viewed on an
// 80x24 half-block screen.
struct ViewerTest : ::testing::Test
{
	Context m_c;
	fs::path m_dir = freshDir("viewer");
	fs::path m_layers = writeExr(m_dir / "layers.exr",
	                             {"R",
	                              "G",
	                              "B",
	                              "diffuse.R",
	                              "diffuse.G",
	                              "diffuse.B",
	                              "spec.R",
	                              "spec.G",
	                              "spec.B"});
	int m_closed = 0;
	Viewer m_v{m_c.m_ctx, [this] { ++m_closed; }};

	ViewerTest()
	{
		for(int i = 1; i <= 3; ++i)
		{
			writeExr(m_dir / ("seq.000" + std::to_string(i) + ".exr"),
			         {"R", "G", "B"});
		}
	}
	Entry layersEntry() const
	{
		return entryForPath(m_layers);
	}
	Entry sequenceEntry() const
	{
		return entryForPath(m_dir / "seq.0001.exr");
	}
	void press(const Event& e)
	{
		EXPECT_TRUE(m_v.event(e));
	}
	const ViewerState& state() const
	{
		return m_v.state();
	}
	ImageInfoPtr info()
	{
		ImageInfoPtr i;
		waitFor(
		    [&]
		    {
			    return (i = m_c.m_svc.info(state().currentFramePath())) !=
			           nullptr;
		    });
		return i;
	}
	// Draw a tiled sheet until every tile's decode has landed (tiles of
	// layers ask for each layer, not the selected one draw() waits for).
	void drawSheet()
	{
		ASSERT_TRUE(info());
		ftxui::Screen screen(80, 24);
		ftxui::Render(screen, m_v.render());
		ASSERT_TRUE(waitFor([&] { return m_c.m_svc.pendingJobs() == 0; }));
		ftxui::Render(screen, m_v.render());
	}
	// Draw until the image itself is on screen (decoded and laid out).
	void draw()
	{
		ASSERT_TRUE(info());
		ftxui::Screen screen(80, 24);
		ftxui::Render(screen, m_v.render());
		const fs::path path = state().currentFramePath();
		ASSERT_TRUE(waitFor(
		    [&] { return m_c.m_svc.hasLayer(path, state().m_layerLabel, 1); }));
		ftxui::Render(screen, m_v.render());
	}
};

} // namespace

TEST_F(ViewerTest, OpenStartsFromTheFirstFrameFittedAndUntiled)
{
	m_v.open({sequenceEntry()});
	draw();
	press(key("."));
	press(key("+"));
	press(key("t"));
	EXPECT_EQ(state().m_frame, 1);
	EXPECT_TRUE(m_v.tiled());
	EXPECT_FALSE(state().m_view.m_fit);

	m_v.open({sequenceEntry(), layersEntry()});
	EXPECT_EQ(state().m_sources.size(), 2u);
	EXPECT_EQ(state().m_current, 0);
	EXPECT_EQ(state().m_frame, 0);
	EXPECT_FALSE(m_v.tiled());
	EXPECT_TRUE(state().m_view.m_fit);
	EXPECT_FALSE(state().m_picked);

	m_v.open({layersEntry()}, true);
	EXPECT_TRUE(m_v.tiled());
}

TEST_F(ViewerTest, TileSheetZoomsPansAndOpensWhereItWas)
{
	m_v.open({layersEntry()}, true); // a tile per layer: rgba, diffuse, spec
	drawSheet();
	ASSERT_EQ(m_v.sheet().m_count, 3);
	EXPECT_EQ(m_v.tilesShown(), 3);
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
	press(key("+"));
	EXPECT_GT(m_v.sheetView().m_zoom, 1.0);
	press(key("f"));
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
	// The wheel zooms the sheet too (it did nothing on tiles before).
	ftxui::Mouse wheel;
	wheel.button = ftxui::Mouse::WheelUp;
	wheel.motion = ftxui::Mouse::Pressed;
	wheel.x = 40;
	wheel.y = 12;
	press(Event::Mouse("", wheel));
	EXPECT_GT(m_v.sheetView().m_zoom, 1.0);
	press(key("f"));

	// Zoomed in on the selected tile, panning selects what is in the middle.
	const std::string first = state().m_layerLabel;
	for(int i = 0; i < 8; ++i)
	{
		press(key("+"));
	}
	drawSheet();
	EXPECT_EQ(state().m_layerLabel, first); // keys zoom about the selection
	EXPECT_LT(m_v.tilesShown(), 3);         // the others are off screen
	for(int i = 0; i < 20 && state().m_layerLabel == first; ++i)
	{
		press(key("L"));
	}
	EXPECT_NE(state().m_layerLabel, first);

	// Enter opens it at the zoom it had on the sheet.
	drawSheet();
	press(Event::Return);
	EXPECT_FALSE(m_v.tiled());
	EXPECT_FALSE(state().m_view.m_fit);
	EXPECT_GT(state().m_view.m_zoom, 0.0);

	// Back to tiles: a fresh, fitted sheet.
	press(key("t"));
	drawSheet();
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
}

TEST_F(ViewerTest, FoldingAnExpandedSequenceLaysTheSheetOutAfresh)
{
	// The sequence and an image, tiled beside the files pane.
	m_v.open({sequenceEntry(), layersEntry()}, true);
	press(key("3"));
	draw();
	const SheetLayout fresh = m_v.sheet();
	ASSERT_EQ(fresh.m_count, 2);
	press(key("e")); // the sequence's 3 frames: 4 tiles
	draw();
	EXPECT_EQ(m_v.sheet().m_count, 4);
	press(key("e")); // fold them back
	draw();
	EXPECT_EQ(m_v.sheet(), fresh);
	EXPECT_EQ(m_v.tilesShown(), 2);
}

TEST_F(ViewerTest, LayersCycleBothWaysAndWrap)
{
	m_v.open({layersEntry()});
	draw();
	const auto i = info();
	ASSERT_EQ(i->m_layers.size(), 3u);
	auto shown = [&] { return i->findLayer(state().m_layerLabel); };
	EXPECT_EQ(shown(), 0);
	press(key("]"));
	EXPECT_EQ(shown(), 1);
	press(key("["));
	press(key("["));
	EXPECT_EQ(shown(), 2);
	press(key("]"));
	EXPECT_EQ(shown(), 0);
}

TEST_F(ViewerTest, FrameSteppingWrapsRound)
{
	m_v.open({sequenceEntry()});
	EXPECT_EQ(state().frameCount(), 3);
	press(key(","));
	EXPECT_EQ(state().m_frame, 2);
	press(key("."));
	EXPECT_EQ(state().m_frame, 0);
	press(key(">"));
	EXPECT_EQ(state().m_frame, 2);
	press(key("<"));
	EXPECT_EQ(state().m_frame, 0);

	m_v.open({layersEntry()}); // one frame: stays
	press(key("."));
	EXPECT_EQ(state().m_frame, 0);
	press(key(" "));
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
	EXPECT_FALSE(m_v.playing());
}

TEST_F(ViewerTest, ColonGoesToAFrameNumberOrTheNearest)
{
	for(int n : {1001, 1002, 1005})
	{
		writeExr(m_dir / ("gap." + std::to_string(n) + ".exr"), {"R"});
	}
	m_v.open({entryForPath(m_dir / "gap.1001.exr")});
	auto go = [&](const std::string& digits)
	{
		press(key(":"));
		EXPECT_TRUE(m_v.typing());
		for(char c : digits)
		{
			press(key(std::string(1, c)));
		}
		press(Event::Return);
		EXPECT_FALSE(m_v.typing());
	};
	go("1005");
	EXPECT_EQ(state().m_frame, 2);
	EXPECT_EQ(m_c.m_ctx.m_message, "");
	go("1004"); // a gap: the nearest
	EXPECT_EQ(state().m_frame, 2);
	EXPECT_EQ(m_c.m_ctx.m_message, "no frame 1004: showing 1005");
	go("1003"); // a tie: the earlier
	EXPECT_EQ(state().m_frame, 1);
	go("9999"); // past the end: the last
	EXPECT_EQ(state().m_frame, 2);
	go("1"); // before the start: the first
	EXPECT_EQ(state().m_frame, 0);

	// Letters are not typed (nor run as commands); Esc leaves it as it was.
	press(key(":"));
	press(key("q"));
	press(key("1"));
	press(Event::Escape);
	EXPECT_FALSE(m_v.typing());
	EXPECT_EQ(state().m_frame, 0);
	EXPECT_EQ(m_closed, 0);
	go(""); // nothing typed: nothing happens
	EXPECT_EQ(state().m_frame, 0);

	m_v.open({layersEntry()});
	press(key(":"));
	EXPECT_FALSE(m_v.typing());
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
}

TEST_F(ViewerTest, PLiftsThePlaybackResolutionCap)
{
	m_v.open({sequenceEntry()});
	ASSERT_EQ(m_c.m_caps.m_transfer, Transfer::DIRECT); // as over ssh
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET);
	press(key(" "));
	ASSERT_TRUE(m_v.playing());
	// Half for playing, half again for the inline transfer.
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET / 4);
	press(key("P"));
	EXPECT_FALSE(m_v.playbackCapped());
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET);
	EXPECT_EQ(m_c.m_ctx.m_message, "playback: full res");
	press(key("P"));
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET / 4);
	EXPECT_EQ(m_c.m_ctx.m_message, "playback: capped at 1 Mpx (full 4)");
	press(key(" "));
}

TEST_F(ViewerTest, ZoomStepsBy125PercentAndPanMovesByCells)
{
	m_v.open({layersEntry()});
	press(key("+")); // not drawn yet: fit has no zoom to start from
	EXPECT_TRUE(state().m_view.m_fit);
	draw();
	press(key("+"));
	EXPECT_FALSE(state().m_view.m_fit);
	const double z1 = state().m_view.m_zoom;
	press(key("+"));
	EXPECT_NEAR(state().m_view.m_zoom, z1 * 1.25, 1e-9);
	press(key("-"));
	EXPECT_NEAR(state().m_view.m_zoom, z1, 1e-9);

	press(key("z")); // 1:1
	EXPECT_EQ(state().m_view.m_zoom, 1.0);
	const double cx = state().m_view.m_centerX;
	const double cy = state().m_view.m_centerY;
	// Half-block cells are 1x2 terminal px; at 1:1 that many image px.
	press(key("l"));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 4, 1e-9);
	press(key("j"));
	EXPECT_NEAR(state().m_view.m_centerY, cy + 4, 1e-9);
	press(key("H"));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 4 - 16, 1e-9);

	press(key("f"));
	EXPECT_TRUE(state().m_view.m_fit);
}

TEST_F(ViewerTest, LeftOrMiddleDragPansLikeHjkl)
{
	m_v.open({layersEntry()});
	draw();
	press(key("z")); // 1:1: a cell is 1x2 image px
	auto mouse =
	    [&](ftxui::Mouse::Button b, int x, int y, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = b;
		m.motion = motion;
		m.x = x;
		m.y = y;
		press(Event::Mouse("", m));
	};
	const double cx = state().m_view.m_centerX;
	const double cy = state().m_view.m_centerY;
	// The image follows the mouse: dragging right looks further left.
	mouse(ftxui::Mouse::Left, 40, 12, ftxui::Mouse::Pressed);
	mouse(ftxui::Mouse::Left, 44, 13, ftxui::Mouse::Moved);
	mouse(ftxui::Mouse::Left, 44, 13, ftxui::Mouse::Released);
	EXPECT_NEAR(state().m_view.m_centerX, cx - 4, 1e-9);
	EXPECT_NEAR(state().m_view.m_centerY, cy - 2, 1e-9);
	mouse(ftxui::Mouse::Middle, 40, 12, ftxui::Mouse::Pressed);
	mouse(ftxui::Mouse::Middle, 30, 12, ftxui::Mouse::Moved);
	mouse(ftxui::Mouse::Middle, 30, 12, ftxui::Mouse::Released);
	EXPECT_NEAR(state().m_view.m_centerX, cx - 4 + 10, 1e-9);
	// Released: moving leaves the view be.
	ftxui::Mouse move;
	move.button = ftxui::Mouse::None;
	move.motion = ftxui::Mouse::Moved;
	(void)m_v.event(Event::Mouse("", move));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 6, 1e-9);
}

TEST_F(ViewerTest, RightClickPicksAPixelUntilReopened)
{
	m_v.open({layersEntry()});
	draw();
	ftxui::Mouse m;
	m.button = ftxui::Mouse::Right;
	m.motion = ftxui::Mouse::Pressed;
	m.x = 40;
	m.y = 12;
	press(Event::Mouse("", m));
	ASSERT_TRUE(waitFor(
	    [&]
	    {
		    // The exact readout needs the full-resolution layer.
		    return m_v.event(Event::Mouse("", m)) && state().m_picked &&
		           state().m_picked->m_exact;
	    }));
	EXPECT_EQ(state().m_picked->m_state, Sample::State::OK);
	EXPECT_NEAR(state().m_picked->m_rgba[0], 0.5f, 1e-6);
	EXPECT_TRUE(m_c.m_ctx.m_message.starts_with("picked ["))
	    << m_c.m_ctx.m_message;
	m_v.open({layersEntry()});
	EXPECT_FALSE(state().m_picked);
}

TEST_F(ViewerTest, OcioViewChangesWhatIsShownAndSGoesRaw)
{
	m_v.open({layersEntry()}); // every channel 0.5
	draw();
	auto pickRed = [&]
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Right;
		m.motion = ftxui::Mouse::Pressed;
		m.x = 40;
		m.y = 12;
		EXPECT_TRUE(waitFor(
		    [&]
		    {
			    return m_v.event(Event::Mouse("", m)) && state().m_picked &&
			           state().m_picked->m_exact;
		    }));
		return state().m_picked ? state().m_picked->m_r : -1;
	};
	EXPECT_EQ(pickRed(), 188); // sRGB of 0.5
	ASSERT_TRUE(m_c.m_colour.useConfig(ColourManager::STUDIO))
	    << m_c.m_colour.error();
	const int ocio = pickRed();
	EXPECT_NE(ocio, 188); // the config's view, tone mapped
	EXPECT_GT(ocio, 0);
	press(key("s"));
	EXPECT_EQ(pickRed(), 128); // raw
	press(key("6"));
	EXPECT_EQ(state().m_focus, Focus::COLOUR);
}

TEST_F(ViewerTest, NextAndPreviousImageWrap)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("n"));
	EXPECT_EQ(state().m_current, 1);
	press(key("n"));
	EXPECT_EQ(state().m_current, 0);
	press(key("N"));
	EXPECT_EQ(state().m_current, 1);
}

TEST_F(ViewerTest, QUntilesFirstThenCloses)
{
	m_v.open({layersEntry()}, true);
	press(key("q"));
	EXPECT_FALSE(m_v.tiled());
	EXPECT_EQ(m_closed, 0);
	press(key("q"));
	EXPECT_EQ(m_closed, 1);
}

TEST_F(ViewerTest, PaneKeysOpenFocusAndClose)
{
	m_v.open({layersEntry()});
	press(key("m")); // = 2: metadata
	EXPECT_EQ(state().m_focus, Focus::META);
	press(key("m")); // focused: closes
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
	press(key("2"));
	press(key("5"));
	EXPECT_EQ(state().m_focus, Focus::LAYERS);
	press(Event::Tab); // hide all
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
	press(Event::Tab); // and back, with the focus they had
	EXPECT_EQ(state().m_focus, Focus::LAYERS);
	press(key("1"));
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
}

TEST_F(ViewerTest, DraggingTheLeftDividerResizesTheColumn)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("3")); // files
	draw();
	const int before = state().leftPanelWidth();
	auto mouse = [&](int x, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = x;
		m.y = 3;
		press(Event::Mouse("", m));
	};
	mouse(before, ftxui::Mouse::Pressed); // on the separator
	mouse(before + 4, ftxui::Mouse::Moved);
	mouse(before + 4, ftxui::Mouse::Released);
	EXPECT_EQ(state().leftPanelWidth(), before + 4);
	// Narrower than a short name, or over the image: no further.
	mouse(before + 4, ftxui::Mouse::Pressed);
	mouse(1, ftxui::Mouse::Moved);
	EXPECT_EQ(state().leftPanelWidth(), 16);
	mouse(1000, ftxui::Mouse::Moved);
	mouse(1000, ftxui::Mouse::Released);
	EXPECT_LT(state().leftPanelWidth(),
	          ftxui::Terminal::Size().dimx - state().sidePanelWidth());
	// Released: moving the mouse leaves it be.
	const int after = state().leftPanelWidth();
	ftxui::Mouse move;
	move.motion = ftxui::Mouse::Moved;
	move.x = 5;
	move.y = 3;
	(void)m_v.event(Event::Mouse("", move));
	EXPECT_EQ(state().leftPanelWidth(), after);
}

namespace
{

// The screen row and column where `title` is drawn ({-1, -1}: nowhere).
std::pair<int, int> titleAt(Viewer& v, const std::string& title)
{
	ftxui::Screen screen(80, 24);
	ftxui::Render(screen, v.render());
	for(int y = 0; y < screen.dimy(); ++y)
	{
		std::string row;
		std::vector<int> cols; // screen column of each byte of `row`
		for(int x = 0; x < screen.dimx(); ++x)
		{
			const std::string& c = screen.PixelAt(x, y).character;
			row += c.empty() ? " " : c;
			cols.resize(row.size(), x);
		}
		if(const auto at = row.find(title); at != std::string::npos)
		{
			return {y, cols[at]};
		}
	}
	return {-1, -1};
}

} // namespace

TEST_F(ViewerTest, DraggingTheFilesTitleShowsMoreOrFewerRows)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("3"));
	press(key("j"));
	press(key("e")); // the sequence's frames: global + 4 rows
	draw();
	const int y0 = titleAt(m_v, "[3]─Files").first;
	ASSERT_GT(y0, 0);
	auto mouse = [&](int y, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = 4;
		m.y = y;
		press(Event::Mouse("", m));
	};
	// Down two: two rows fewer, the title where the mouse let go.
	mouse(y0, ftxui::Mouse::Pressed);
	mouse(y0 + 2, ftxui::Mouse::Moved);
	mouse(y0 + 2, ftxui::Mouse::Released);
	EXPECT_EQ(titleAt(m_v, "[3]─Files").first, y0 + 2);
	// Up past the list: it shows every row, no more.
	mouse(y0 + 2, ftxui::Mouse::Pressed);
	mouse(1, ftxui::Mouse::Moved);
	mouse(1, ftxui::Mouse::Released);
	EXPECT_EQ(titleAt(m_v, "[3]─Files").first, y0);
}

TEST_F(ViewerTest, DraggingTheRightDividerResizesTheColumn)
{
	m_v.open({layersEntry()});
	press(key("2")); // metadata, on the right
	draw();
	const int w = ftxui::Terminal::Size().dimx;
	const int before = state().sidePanelWidth();
	auto mouse = [&](int x, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = x;
		m.y = 3;
		press(Event::Mouse("", m));
	};
	// The separator just left of the column's title.
	const int divider = titleAt(m_v, "[2]─Metadata").second - 2;
	ASSERT_GE(divider, 0);
	mouse(divider, ftxui::Mouse::Pressed);
	mouse(divider - 4, ftxui::Mouse::Moved); // leftwards: wider
	mouse(divider - 4, ftxui::Mouse::Released);
	EXPECT_EQ(state().sidePanelWidth(), before + 4);
	EXPECT_EQ(state().m_leftFrac, 0); // the left one untouched
	// Past the left column and some image: no further.
	mouse(divider - 4, ftxui::Mouse::Pressed);
	mouse(0, ftxui::Mouse::Moved);
	mouse(0, ftxui::Mouse::Released);
	EXPECT_LE(state().sidePanelWidth(), w - state().leftPanelWidth() - 2 - 10);
}

TEST_F(ViewerTest, ColourPaneOpensOnTheLeft)
{
	m_v.open({layersEntry()});
	press(key("6"));
	EXPECT_EQ(state().m_focus, Focus::COLOUR);
	draw();
	const auto [y, x] = titleAt(m_v, "[6]─Colour");
	ASSERT_GE(y, 0);
	EXPECT_LT(x, state().leftPanelWidth());
}

TEST_F(ViewerTest, TabWithNothingOpenSaysSo)
{
	m_v.open({layersEntry()});
	press(Event::Tab);
	EXPECT_EQ(m_c.m_ctx.m_message, "no panes to show");
}

TEST_F(ViewerTest, TTypesAnAnnotationLine)
{
	m_v.open({layersEntry()});
	press(key("T"));
	EXPECT_TRUE(m_v.typing());
	EXPECT_EQ(state().m_focus, Focus::FILES);
	press(key("q")); // text, not "close"
	press(key("m")); // text, not "metadata"
	EXPECT_EQ(m_closed, 0);
	EXPECT_EQ(state().m_focus, Focus::FILES);
	press(Event::Return);
	EXPECT_FALSE(m_v.typing());
	const AnnotationSet* set = state().sourceAnnotations(m_c.m_ann, 0);
	ASSERT_TRUE(set);
	EXPECT_EQ(set->lines(Slot::BL), (std::vector<std::string>{"qm"}));
}

TEST_F(ViewerTest, FpsCyclesUpFromTheFileRate)
{
	m_v.open({sequenceEntry()});
	EXPECT_EQ(m_v.player().fps(), 0.0); // from the file (none: 24)
	press(key("F"));
	EXPECT_EQ(m_v.player().fps(), 25.0);
	press(key("F"));
	EXPECT_EQ(m_v.player().fps(), 30.0);
	press(key(" "));
	EXPECT_TRUE(m_v.playing());
	press(key(" "));
	EXPECT_FALSE(m_v.playing());
}

TEST_F(ViewerTest, RescanFindsFramesAddedToASequence)
{
	m_v.open({sequenceEntry(), layersEntry()});
	EXPECT_FALSE(m_v.rescan());
	writeExr(m_dir / "seq.0004.exr", {"R", "G", "B"});
	EXPECT_TRUE(m_v.rescan());
	EXPECT_EQ(state().frameCount(), 4);
	EXPECT_FALSE(m_v.rescan());
	EXPECT_EQ(m_v.shownDirs(), (std::vector<fs::path>{m_dir}));
}

TEST(AppContext, ReduceForAcceptsSlackWhilePlaying)
{
	// A half-block slot drawn 100x50 cells = 100x100 px.
	Context c;
	auto slot = newSlot(c.m_ctx);
	ftxui::Screen screen(100, 50);
	LayerImage img;
	img.m_width = img.m_height = 2;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 1, 1};
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, Plane(std::vector<float>(4, 0.5f)));
	slot->draw(screen,
	           {0, 99, 0, 49},
	           std::make_shared<const LayerImage>(std::move(img)),
	           {},
	           {});
	// 190 px into 100: 1.9x too many; floor keeps full resolution...
	const Box2i b{0, 0, 189, 189};
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, b, std::nullopt, 4'000'000), 1);
	// ...but up to 25% upscaling halves it.
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, b, std::nullopt, 4'000'000, 1.25), 2);
	// Small enough already: slack never goes below 1.
	const Box2i small{0, 0, 79, 79};
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, small, std::nullopt, 4'000'000, 1.25),
	          1);
}
