#include "TestContext.h"
#include "app/Sheet.h"
#include "app/Viewer.h"
#include "image/Sequence.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
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
