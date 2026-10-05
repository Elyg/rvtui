#include "app/Sheet.h"

#include <gtest/gtest.h>

#include <vector>

using namespace rv;

namespace
{

// HD frames, `n` of them.
std::vector<std::pair<double, double>> hd(int n)
{
	return std::vector<std::pair<double, double>>(n, {1920.0, 1080.0});
}

// A 160 x 45 cell area of 8 x 16 px cells (1280 x 720 px).
constexpr int AW = 160, AH = 45;
constexpr double PX = 8, PY = 16;

} // namespace

TEST(Sheet, LayoutPicksTheColumnsShowingTheMostImage)
{
	const SheetLayout one = layoutSheet(hd(1), AW, AH, PX, PY, 200);
	EXPECT_EQ(one.m_cols, 1);
	EXPECT_EQ(one.m_rows, 1);
	EXPECT_EQ(one.m_tileW, 160); // 1280 px wide, 720 tall: fills the area
	EXPECT_EQ(one.m_tileH, 45);

	const SheetLayout four = layoutSheet(hd(4), AW, AH, PX, PY, 200);
	EXPECT_EQ(four.m_cols, 2);
	EXPECT_EQ(four.m_rows, 2);
	EXPECT_LE(four.width(), AW);
	EXPECT_LE(four.height(), AH);

	EXPECT_EQ(layoutSheet({}, AW, AH, PX, PY, 200).m_count, 0);
}

TEST(Sheet, LayoutSizesTilesToTheLargestImage)
{
	// A tall image next to a wide one: the tile is as wide as the wide one
	// and as tall as the tall one, at the shared scale.
	const SheetLayout l =
	    layoutSheet({{1920.0, 1080.0}, {1080.0, 1920.0}}, AW, AH, PX, PY, 200);
	EXPECT_EQ(l.m_count, 2);
	EXPECT_EQ(l.m_cols, 2);
	EXPECT_EQ(l.m_tileW, AW / 2);
	EXPECT_EQ(l.m_tileH, AH);
}

TEST(Sheet, LayoutPastMaxVisibleRunsOffTheBottom)
{
	const SheetLayout capped = layoutSheet(hd(500), AW, AH, PX, PY, 180);
	const SheetLayout first = layoutSheet(hd(180), AW, AH, PX, PY, 180);
	// Tiles sized as for 180, the rest below.
	EXPECT_EQ(capped.m_cols, first.m_cols);
	EXPECT_EQ(capped.m_tileW, first.m_tileW);
	EXPECT_EQ(capped.m_tileH, first.m_tileH);
	EXPECT_EQ(capped.m_rows, (500 + capped.m_cols - 1) / capped.m_cols);
	EXPECT_GT(capped.height(), AH);

	// Fitted, it shows the top, and no more than ~180 tiles.
	const SheetView v = fitSheetView(capped, {AW, AH});
	int visible = 0;
	for(int i = 0; i < capped.m_count; ++i)
	{
		visible += placeTile(capped, v, {AW, AH}, i).has_value();
	}
	// The rows sized for 180, plus the one cut by the bottom edge: still well
	// inside kitty's 255 image ids.
	const int rows180 = (180 + capped.m_cols - 1) / capped.m_cols;
	EXPECT_LE(visible, (rows180 + 1) * capped.m_cols);
	EXPECT_LT(visible, 240);
	ASSERT_TRUE(placeTile(capped, v, {AW, AH}, 0));
	EXPECT_EQ(placeTile(capped, v, {AW, AH}, 0)->m_boxY0, 0);
	EXPECT_FALSE(placeTile(capped, v, {AW, AH}, 499));
}

TEST(Sheet, TileAtFindsTheTileUnderAPoint)
{
	SheetLayout l;
	l.m_count = 3;
	l.m_cols = 2;
	l.m_rows = 2;
	l.m_tileW = 10;
	l.m_tileH = 5;
	EXPECT_EQ(l.tileAt(1, 1), 0);
	EXPECT_EQ(l.tileAt(15, 1), 1);
	EXPECT_EQ(l.tileAt(5, 7), 2);
	EXPECT_EQ(l.tileAt(15, 7), -1); // no 4th tile
	EXPECT_EQ(l.tileAt(-1, 1), -1);
	EXPECT_EQ(l.tileAt(25, 1), -1);
}

TEST(Sheet, FitCentresASheetThatFits)
{
	const SheetLayout l = layoutSheet(hd(4), AW, AH, PX, PY, 200);
	const SheetView v = fitSheetView(l, {AW, AH});
	EXPECT_EQ(v.m_zoom, 1.0);
	EXPECT_DOUBLE_EQ(v.m_cx, l.width() / 2.0);
	EXPECT_DOUBLE_EQ(v.m_cy, l.height() / 2.0);
	// Panning a fitted sheet does nothing: it is all on screen.
	SheetView p = v;
	panSheetView(p, l, {AW, AH}, 10, 10);
	EXPECT_EQ(p, v);
	// Nor can it zoom out past fit.
	zoomSheetView(p, l, {AW, AH}, 0.5, 0, 0);
	EXPECT_EQ(p, v);
}

TEST(Sheet, ZoomKeepsThePointUnderTheMousePut)
{
	const SheetLayout l = layoutSheet(hd(4), AW, AH, PX, PY, 200);
	SheetView v = fitSheetView(l, {AW, AH});
	const double ax = 60, ay = 20;
	const auto before = sheetPointAt(v, {AW, AH}, ax, ay);
	zoomSheetView(v, l, {AW, AH}, 2.0, ax, ay);
	EXPECT_DOUBLE_EQ(v.m_zoom, 2.0);
	const auto after = sheetPointAt(v, {AW, AH}, ax, ay);
	EXPECT_NEAR(after.first, before.first, 1e-9);
	EXPECT_NEAR(after.second, before.second, 1e-9);
}

TEST(Sheet, PanStopsAtTheSheetEdges)
{
	const SheetLayout l = layoutSheet(hd(4), AW, AH, PX, PY, 200);
	SheetView v = fitSheetView(l, {AW, AH});
	zoomSheetView(v, l, {AW, AH}, 4.0, AW / 2.0, AH / 2.0);
	panSheetView(v, l, {AW, AH}, -100000, -100000);
	// The top-left corner of the sheet is at the top-left of the area.
	const auto p = placeTile(l, v, {AW, AH}, 0);
	ASSERT_TRUE(p);
	EXPECT_NEAR(p->m_x0, 0, 1e-9);
	EXPECT_NEAR(p->m_y0, 0, 1e-9);
	panSheetView(v, l, {AW, AH}, 100000, 100000);
	const auto q = placeTile(l, v, {AW, AH}, 3);
	ASSERT_TRUE(q);
	EXPECT_NEAR(q->m_x1, AW, 1e-9);
	EXPECT_NEAR(q->m_y1, AH, 1e-9);
}

TEST(Sheet, PlacementClipsTilesToTheArea)
{
	const SheetLayout l = layoutSheet(hd(4), AW, AH, PX, PY, 200);
	SheetView v = fitSheetView(l, {AW, AH});
	// Zoomed into the middle, every tile shows a part, its box inside the
	// area and its whole extent past it.
	zoomSheetView(v, l, {AW, AH}, 3.0, AW / 2.0, AH / 2.0);
	for(int i = 0; i < 4; ++i)
	{
		const auto p = placeTile(l, v, {AW, AH}, i);
		ASSERT_TRUE(p) << i;
		EXPECT_GE(p->m_boxX0, 0);
		EXPECT_LE(p->m_boxX1, AW);
		EXPECT_GE(p->m_boxY0, 0);
		EXPECT_LE(p->m_boxY1, AH);
		EXPECT_GT(p->m_x1 - p->m_x0, p->m_boxX1 - p->m_boxX0);
	}
	// Neighbours share an edge exactly: no gap, no overlap.
	EXPECT_EQ(placeTile(l, v, {AW, AH}, 0)->m_boxX1,
	          placeTile(l, v, {AW, AH}, 1)->m_boxX0);
}
