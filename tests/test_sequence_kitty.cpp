#include "image/Sequence.h"
#include "term/Kitty.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>
#include <zlib.h>

TEST(Sequence, SplitFrame)
{
	std::string pre, dig, suf;
	ASSERT_TRUE(rv::splitFrame("shot.1001.exr", pre, dig, suf));
	EXPECT_EQ(pre, "shot.");
	EXPECT_EQ(dig, "1001");
	EXPECT_EQ(suf, ".exr");
	ASSERT_TRUE(rv::splitFrame("plate_0001.png", pre, dig, suf));
	EXPECT_EQ(pre, "plate_");
	EXPECT_FALSE(
	    rv::splitFrame("v2.exr", pre, dig, suf)); // no separator before digits
	EXPECT_FALSE(rv::splitFrame("1001.exr", pre, dig, suf));
	EXPECT_FALSE(rv::splitFrame("noext", pre, dig, suf));
}

TEST(Sequence, Groups)
{
	auto entries = rv::groupFiles("/d",
	                              {"a.1001.exr",
	                               "a.1002.exr",
	                               "a.1003.exr",
	                               "a.1005.exr",
	                               "b.0001.png",
	                               "c.exr",
	                               "notes.txt",
	                               "x.9.jpg",
	                               "x.10.jpg"});
	ASSERT_EQ(entries.size(), 5u);
	EXPECT_EQ(entries[0].m_name, "a.####.exr [1001-1003,1005]");
	EXPECT_EQ(entries[0].m_frames.size(), 4u);
	EXPECT_EQ(entries[1].m_name, "b.0001.png"); // singleton stays a file
	EXPECT_EQ(entries[2].m_name, "c.exr");
	EXPECT_EQ(entries[3].m_name, "notes.txt");
	EXPECT_EQ(entries[4].m_name, "x.@.jpg [9-10]"); // unpadded
	EXPECT_EQ(entries[4].m_frameNumbers, (std::vector<int>{9, 10}));
}

TEST(Kitty, Base64)
{
	auto b = [](const std::string& s)
	{
		return rv::kitty::base64(reinterpret_cast<const uint8_t*>(s.data()),
		                         s.size());
	};
	EXPECT_EQ(b(""), "");
	EXPECT_EQ(b("f"), "Zg==");
	EXPECT_EQ(b("fo"), "Zm8=");
	EXPECT_EQ(b("foo"), "Zm9v");
	EXPECT_EQ(b("foobar"), "Zm9vYmFy");
}

TEST(Kitty, TransmitChunks)
{
	rv::Rgba8Image img;
	img.m_width = 64;
	img.m_height = 64;
	img.m_pixels.resize(64 * 64 * 4);
	uint32_t x = 2463534242u; // xorshift noise: incompressible → several chunks
	for(auto& p : img.m_pixels)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p = static_cast<uint8_t>(x);
	}
	rv::kitty::TransmitOptions opt;
	opt.m_id = 42;
	opt.m_cols = 10;
	opt.m_rows = 5;
	opt.m_chunk = 1024;
	std::string s = rv::kitty::transmit(img, opt);
	EXPECT_EQ(
	    s.rfind("\x1b_Ga=T,f=32,o=z,q=2,s=64,v=64,i=42,U=1,p=1,c=10,r=5,m=1;",
	            0),
	    0u);
	// Every chunk terminated; last chunk has m=0.
	EXPECT_NE(s.find("\x1b_Gm=0;"), std::string::npos);
	EXPECT_EQ(s.substr(s.size() - 2), "\x1b\\");
}

TEST(Kitty, TmuxWrapDoublesEscapes)
{
	EXPECT_EQ(rv::kitty::tmuxWrap("\x1b_Gx\x1b\\"),
	          "\x1bPtmux;\x1b\x1b_Gx\x1b\x1b\\\x1b\\");
}

TEST(Kitty, PlaceholderCell)
{
	// U+10EEEE, row 0 → U+0305, col 1 → U+030D
	EXPECT_EQ(rv::kitty::placeholderCell(0, 1),
	          "\xF4\x8E\xBB\xAE\xCC\x85\xCC\x8D");
}

TEST(Kitty, PlaceholderRowColReadsBackPlaceholderCell)
{
	using rv::kitty::placeholderCell, rv::kitty::placeholderRowCol;
	for(auto [r, c] : {std::pair{0, 0}, {0, 1}, {41, 155}, {296, 296}})
	{
		EXPECT_EQ(placeholderRowCol(placeholderCell(r, c)), std::pair(r, c));
	}
	EXPECT_EQ(placeholderRowCol(rv::kitty::barePlaceholderCell()),
	          std::nullopt);
	EXPECT_EQ(placeholderRowCol("x"), std::nullopt);
	EXPECT_EQ(placeholderRowCol(placeholderCell(1, 2) + "x"), std::nullopt);
}

TEST(Kitty, ZlibRoundTripsAsOneStream)
{
	// Several pieces (> 384 KiB) of mixed compressible / noisy data.
	std::vector<uint8_t> src(3'000'001);
	uint32_t x = 12345u;
	for(size_t i = 0; i < src.size(); ++i)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		src[i] = (i / 50'000) % 2 ? static_cast<uint8_t>(x)
		                          : static_cast<uint8_t>(i / 1000);
	}
	std::string z = rv::kitty::zlibCompress(src.data(), src.size());
	std::vector<uint8_t> back(src.size());
	uLongf len = back.size();
	ASSERT_EQ(uncompress(back.data(),
	                     &len,
	                     reinterpret_cast<const Bytef*>(z.data()),
	                     z.size()),
	          Z_OK);
	ASSERT_EQ(len, src.size());
	EXPECT_EQ(back, src);
	// No sync-flush points: an empty stored block (00 00 FF FF) between
	// pieces crashes Ghostty's Zig decoder.
	EXPECT_EQ(z.find(std::string("\x00\x00\xff\xff", 4)), std::string::npos);
}

TEST(Kitty, ParallelBase64MatchesAcrossPieces)
{
	std::vector<uint8_t> src(3 * 128 * 1024 * 2 +
	                         2); // two pieces + 2-byte tail
	for(size_t i = 0; i < src.size(); ++i)
	{
		src[i] = static_cast<uint8_t>(i * 31);
	}
	std::string b = rv::kitty::base64(src.data(), src.size());
	ASSERT_EQ(b.size(), (src.size() + 2) / 3 * 4);
	EXPECT_EQ(b.substr(b.size() - 1), "=");
	// Group straddling the piece boundary encodes the same as on its own.
	size_t at = 3 * 128 * 1024;
	EXPECT_EQ(b.substr(at / 3 * 4, 4), rv::kitty::base64(&src[at], 3));
}

TEST(Sequence, EntriesForArgs)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "rvtui_args_test";
	fs::remove_all(dir);
	fs::create_directories(dir);
	for(const char* n : {"shot.0001.exr", "shot.0002.exr", "shot.0003.exr"})
	{
		std::ofstream(dir / n) << "x";
	}
	auto run = [](std::vector<std::string> args)
	{
		std::vector<std::string> missing;
		auto e = rv::entriesForArgs(args, missing);
		return std::make_pair(e, missing);
	};
	const std::string d = dir.string() + "/";

	// One frame opens only that frame.
	auto [single, m1] = run({d + "shot.0002.exr"});
	ASSERT_EQ(single.size(), 1u);
	EXPECT_EQ(single[0].m_kind, rv::Entry::Kind::FILE);
	EXPECT_EQ(single[0].m_name, "shot.0002.exr");

	// Quoted globs and #### padding open the sequence.
	for(const char* pattern : {"shot.*.exr", "shot.####.exr"})
	{
		auto [seq, m] = run({d + pattern});
		ASSERT_EQ(seq.size(), 1u) << pattern;
		EXPECT_EQ(seq[0].m_kind, rv::Entry::Kind::SEQUENCE);
		EXPECT_EQ(seq[0].m_frames.size(), 3u);
	}

	// A lone # takes any padding; ## and up are exact.
	auto [lone, m4] = run({d + "shot.#.exr"});
	ASSERT_EQ(lone.size(), 1u);
	EXPECT_EQ(lone[0].m_frames.size(), 3u);
	auto [exact, m5] = run({d + "shot.##.exr"});
	EXPECT_TRUE(exact.empty());
	EXPECT_EQ(m5.size(), 1u);
	for(const char* n : {"plate.1.exr", "plate.2.exr", "plate.10.exr"})
	{
		std::ofstream(dir / n) << "x";
	}
	std::ofstream(dir / "plate.final.exr") << "x"; // not a frame
	auto [unpadded, m6] = run({d + "plate.#.exr"});
	ASSERT_EQ(unpadded.size(), 1u);
	EXPECT_EQ(unpadded[0].m_frames.size(), 3u);
	EXPECT_EQ(unpadded[0].m_frameNumbers, (std::vector<int>{1, 2, 10}));

	// What the shell passes for an unquoted glob: grouped the same way.
	auto [expanded, m2] =
	    run({d + "shot.0001.exr", d + "shot.0002.exr", d + "shot.0003.exr"});
	ASSERT_EQ(expanded.size(), 1u);
	EXPECT_EQ(expanded[0].m_kind, rv::Entry::Kind::SEQUENCE);

	// Separate files keep the order they were given in.
	std::ofstream(dir / "b.exr") << "x";
	std::ofstream(dir / "a.exr") << "x";
	auto [ordered, m3] = run({d + "b.exr", d + "a.exr"});
	ASSERT_EQ(ordered.size(), 2u);
	EXPECT_EQ(ordered[0].m_name, "b.exr");
	EXPECT_EQ(ordered[1].m_name, "a.exr");

	auto [none, missing] = run({d + "nope.exr", d + "nope.*.exr"});
	EXPECT_TRUE(none.empty());
	EXPECT_EQ(missing.size(), 2u);
	fs::remove_all(dir);
}

TEST(Kitty, ImageIdsNeverCollideWithLiveOnes)
{
	// The bug: ids wrapped after 255 allocations onto slots still on screen.
	rv::kitty::Transmitter tx(false,
	                          rv::Transfer::DIRECT,
	                          [](std::string_view, bool) {});
	const uint32_t view = tx.allocId();
	const uint32_t preview = tx.allocId();
	for(int i = 0; i < 1000; ++i) // tile slots created and destroyed
	{
		const uint32_t id = tx.allocId();
		ASSERT_GE(id, 1u);
		ASSERT_LE(id, 255u);
		ASSERT_NE(id, view);
		ASSERT_NE(id, preview);
		tx.releaseId(id);
	}
	tx.releaseId(view);
	tx.releaseId(preview);
}

#include "util/Fuzzy.h"

TEST(Fuzzy, SubstringFirstThenSubsequence)
{
	auto sub = rv::fuzzyMatch("rgba_right", "RIG");
	ASSERT_TRUE(sub);
	EXPECT_EQ(*sub, (std::vector<size_t>{5, 6, 7})); // contiguous, case-blind
	auto seq = rv::fuzzyMatch("depth_left", "dpl");
	ASSERT_TRUE(seq);
	EXPECT_EQ(*seq, (std::vector<size_t>{0, 2, 6}));
	EXPECT_FALSE(rv::fuzzyMatch("depth_left", "zz"));
	EXPECT_TRUE(rv::fuzzyMatch("anything", "")->empty());
}
