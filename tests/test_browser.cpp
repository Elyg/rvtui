#include "TestContext.h"
#include "app/Browser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

using namespace rv;
using namespace rvtest;
using ftxui::Event;

namespace
{

// A directory with a sub-directory, a 3-frame sequence, an image, a text
// file and a hidden file.
struct BrowserTest : ::testing::Test
{
	Context m_c;
	fs::path m_dir = freshDir("browser");
	std::optional<std::vector<Entry>> m_opened;
	bool m_openedTiled = false;
	Browser m_b{m_c.m_ctx,
	            "",
	            [this](std::vector<Entry> entries, bool tile)
	            {
		            m_opened = std::move(entries);
		            m_openedTiled = tile;
	            }};

	BrowserTest()
	{
		fs::create_directories(m_dir / "sub");
		touch(m_dir / "sub" / "inner.png");
		for(const char* f : {"a.0001.exr", "a.0002.exr", "a.0003.exr"})
		{
			touch(m_dir / f);
		}
		touch(m_dir / "b.png");
		touch(m_dir / "notes.txt", "hello\n");
		touch(m_dir / ".hidden");
		m_b.openDirectory(m_dir);
	}

	void press(const Event& e)
	{
		EXPECT_TRUE(m_b.event(e));
	}
	void type(const std::string& s)
	{
		for(char c : s)
		{
			press(Event::Character(c));
		}
	}
	std::string selected() const
	{
		const Entry* e = m_b.selectedEntry();
		return e ? e->m_name : "";
	}
	// Move the cursor onto the entry named `name`.
	void select(const std::string& name)
	{
		press(key("g"));
		press(key("g"));
		for(size_t i = 0; i < m_b.entries().size() && selected() != name; ++i)
		{
			press(key("j"));
		}
		ASSERT_EQ(selected(), name);
	}
	std::string sequenceName() const
	{
		for(const auto& e : m_b.entries())
		{
			if(e.m_kind == Entry::Kind::SEQUENCE)
			{
				return e.m_name;
			}
		}
		return "";
	}
};

} // namespace

TEST_F(BrowserTest, ListsDirsFirstThenFilesWithoutHiddenOnes)
{
	const auto& es = m_b.entries();
	ASSERT_EQ(es.size(), 4u);
	EXPECT_EQ(es[0].m_name, "sub");
	EXPECT_EQ(es[1].m_kind, Entry::Kind::SEQUENCE);
	EXPECT_EQ(es[2].m_name, "b.png");
	EXPECT_EQ(es[3].m_name, "notes.txt");
	press(key("."));
	EXPECT_EQ(m_b.entries().size(), 5u); // .hidden shown
}

TEST_F(BrowserTest, MovesWithJkAndJumpsWithGgAndG)
{
	EXPECT_EQ(selected(), "sub");
	press(key("j"));
	press(key("j"));
	EXPECT_EQ(selected(), "b.png");
	press(key("G"));
	EXPECT_EQ(selected(), "notes.txt");
	press(key("j")); // clamped
	EXPECT_EQ(selected(), "notes.txt");
	press(key("g"));
	EXPECT_EQ(selected(), "notes.txt"); // waits for the second g
	press(key("g"));
	EXPECT_EQ(selected(), "sub");
	press(key("k"));
	EXPECT_EQ(selected(), "sub");
}

TEST_F(BrowserTest, EntersADirectoryAndComesBackOntoIt)
{
	press(key("l"));
	EXPECT_EQ(m_b.cwd(), m_dir / "sub");
	EXPECT_EQ(selected(), "inner.png");
	press(key("h"));
	EXPECT_EQ(m_b.cwd(), m_dir);
	EXPECT_EQ(selected(), "sub");
}

TEST_F(BrowserTest, FuzzyFilterNarrowsTheListUntilCleared)
{
	press(key("/"));
	EXPECT_TRUE(m_b.typing());
	type("nts");
	EXPECT_EQ(m_b.filter(), "nts");
	ASSERT_EQ(m_b.visibleIndices().size(), 1u);
	EXPECT_EQ(selected(), "notes.txt");
	press(Event::Return); // keep it, stop typing
	EXPECT_FALSE(m_b.typing());
	EXPECT_EQ(m_b.filter(), "nts");
	press(Event::Escape); // clears it
	EXPECT_EQ(m_b.filter(), "");
	EXPECT_EQ(m_b.visibleIndices().size(), 4u);
	EXPECT_FALSE(m_b.event(key("Q"))); // not a browser key (App quits)
}

TEST_F(BrowserTest, MarksOnlyImagesAndKeepsThemAcrossDirectories)
{
	press(key(" ")); // on "sub"
	EXPECT_EQ(m_c.m_ctx.m_message, "only images can be marked");
	EXPECT_TRUE(m_b.marks().empty());
	select("b.png");
	press(key(" "));
	select(sequenceName());
	press(key(" "));
	ASSERT_EQ(m_b.marks().size(), 2u);
	select("sub");
	press(key("l"));
	press(key(" ")); // inner.png
	press(key("h"));
	EXPECT_EQ(m_b.marks().size(), 3u);
	select("b.png");
	press(key(" ")); // again: unmarked
	ASSERT_EQ(m_b.marks().size(), 2u);
	EXPECT_EQ(m_b.marks()[0].m_kind, Entry::Kind::SEQUENCE); // marking order
	EXPECT_EQ(m_b.marks()[1].m_name, "inner.png");
	press(Event::Escape); // no filter: Esc clears the marks
	EXPECT_TRUE(m_b.marks().empty());
}

TEST_F(BrowserTest, OpensTheMarkedImagesElseTheSelectedOne)
{
	select("b.png");
	press(key("l"));
	ASSERT_TRUE(m_opened);
	ASSERT_EQ(m_opened->size(), 1u);
	EXPECT_EQ(m_opened->front().m_name, "b.png");
	EXPECT_FALSE(m_openedTiled);

	select(sequenceName());
	press(key(" "));
	select("b.png");
	press(key(" "));
	press(key("t")); // all marked, as a contact sheet
	ASSERT_EQ(m_opened->size(), 2u);
	EXPECT_EQ(m_opened->front().m_kind, Entry::Kind::SEQUENCE);
	EXPECT_TRUE(m_openedTiled);
}

TEST_F(BrowserTest, TextFilesFocusThePreviewInsteadOfOpening)
{
	select("notes.txt");
	press(key("l"));
	EXPECT_FALSE(m_opened);
	press(key("j")); // scrolls the preview, not the list
	EXPECT_EQ(selected(), "notes.txt");
	press(key("h")); // back out of the preview
	press(key("k"));
	EXPECT_EQ(selected(), "b.png");
}

TEST_F(BrowserTest, ExpandsASequenceIntoFramesAndFoldsItBack)
{
	const std::string seq = sequenceName();
	select(seq);
	press(key("e"));
	ASSERT_EQ(m_b.entries().size(), 6u);
	EXPECT_EQ(selected(), "a.0001.exr");
	EXPECT_FALSE(m_b.selectedEntry()->m_expandedFrom.empty());
	press(key("j"));
	press(key("e")); // on a frame: fold back, onto the sequence
	EXPECT_EQ(m_b.entries().size(), 4u);
	EXPECT_EQ(selected(), seq);
	select("b.png");
	press(key("e"));
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
}

TEST_F(BrowserTest, RefreshPicksUpNewFilesAndKeepsTheSelection)
{
	select("b.png");
	EXPECT_FALSE(m_b.refresh());
	touch(m_dir / "0first.png"); // sorts before everything else
	EXPECT_TRUE(m_b.refresh());
	EXPECT_EQ(m_b.entries().size(), 5u);
	EXPECT_EQ(selected(), "b.png");
	EXPECT_FALSE(m_b.refresh());
	const auto dirs = m_b.shownDirs();
	ASSERT_FALSE(dirs.empty());
	EXPECT_EQ(dirs.front(), m_dir);
}

TEST_F(BrowserTest, QuitAndChooseWithoutAFile)
{
	press(key("o"));
	EXPECT_EQ(m_c.m_ctx.m_message, "no --chooser-file given");
	EXPECT_EQ(m_c.m_quits, 0);
	press(key("q"));
	EXPECT_EQ(m_c.m_quits, 1);
}

TEST(Browser, ChooserFileGetsTheMarkedPaths)
{
	Context c;
	const fs::path dir = freshDir("browser-chooser");
	touch(dir / "a.png");
	touch(dir / "b.png");
	const fs::path out = dir / "chosen";
	Browser b(c.m_ctx, out.string(), [](std::vector<Entry>, bool) {});
	b.openDirectory(dir);
	EXPECT_TRUE(b.event(key("j")));
	EXPECT_TRUE(b.event(key(" "))); // mark b.png
	EXPECT_TRUE(b.event(key("o")));
	EXPECT_EQ(c.m_quits, 1);
	std::ifstream in(out);
	std::string line;
	std::getline(in, line);
	EXPECT_EQ(line, (dir / "b.png").string());
}
