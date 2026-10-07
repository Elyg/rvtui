#include "TestContext.h"
#include "app/Tutorial.h"
#include "image/Loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{

constexpr rv::tutorial::Options SMALL{.m_first = 1001,
                                      .m_frames = 3,
                                      .m_width = 48,
                                      .m_height = 27};

std::vector<std::string> layerLabels(const fs::path& p)
{
	std::vector<std::string> out;
	for(const auto& l : rv::probeImage(p).m_layers)
	{
		out.push_back(l.label());
	}
	return out;
}

} // namespace

TEST(Tutorial, WritesTheSequenceStillsAndReadme)
{
	const fs::path dir = rvtest::freshDir("tutorial");
	int last = 0, total = 0;
	rv::tutorial::write(dir,
	                    SMALL,
	                    [&](int done, int n)
	                    {
		                    last = done;
		                    total = n;
	                    });
	EXPECT_EQ(last, total);
	EXPECT_EQ(total, 3 + 5);
	for(int f = 1001; f <= 1003; ++f)
	{
		EXPECT_TRUE(fs::exists(dir / ("shot." + std::to_string(f) + ".exr")));
	}
	EXPECT_FALSE(fs::exists(dir / "shot.1004.exr"));
	for(const char* still : {"chart.exr",
	                         "hdr-sky.exr",
	                         "luma-ramp.exr",
	                         "overscan.exr",
	                         "nan-inf.exr",
	                         "README.txt"})
	{
		EXPECT_TRUE(fs::exists(dir / still)) << still;
	}

	const auto layers = layerLabels(dir / "shot.1002.exr");
	for(const char* want : {"albedo", "diffuse", "specular", "N", "mask"})
	{
		EXPECT_NE(std::find_if(layers.begin(),
		                       layers.end(),
		                       [&](const std::string& l)
		                       { return l.find(want) != std::string::npos; }),
		          layers.end())
		    << want;
	}
	for(const char* still : {"chart.exr",
	                         "hdr-sky.exr",
	                         "luma-ramp.exr",
	                         "overscan.exr",
	                         "nan-inf.exr"})
	{
		EXPECT_GE(layerLabels(dir / still).size(), 3u) << still;
	}

	const rv::ImageInfo overscan = rv::probeImage(dir / "overscan.exr");
	EXPECT_LT(overscan.dataWindow().m_x0, overscan.displayWindow().m_x0);
}

TEST(Tutorial, BrokenRenderHasNanAndInf)
{
	const fs::path dir = rvtest::freshDir("tutorial");
	rv::tutorial::write(dir, SMALL);
	const rv::ImageInfo info = rv::probeImage(dir / "nan-inf.exr");
	const rv::LayerImage img = rv::loadLayer(info, 0);
	int nans = 0, infs = 0;
	for(const rv::Plane& plane : img.m_planes)
	{
		for(size_t i = 0; i < plane.size(); ++i)
		{
			const float v = plane[i];
			nans += std::isnan(v);
			infs += std::isinf(v);
		}
	}
	EXPECT_GT(nans, 0);
	EXPECT_GT(infs, 0);
}

TEST(Tutorial, EnsureRendersOnceThenAgainWhenTheSetChanges)
{
	const fs::path dir = rvtest::freshDir("tutorial");
	EXPECT_TRUE(rv::tutorial::ensure(dir, SMALL));
	EXPECT_FALSE(rv::tutorial::ensure(dir, SMALL));
	rv::tutorial::Options fewer = SMALL;
	fewer.m_frames = 2;
	EXPECT_TRUE(rv::tutorial::ensure(dir, fewer));
	EXPECT_FALSE(fs::exists(dir / "shot.1003.exr")); // stale frame gone
	EXPECT_TRUE(fs::exists(dir / "shot.1002.exr"));
}
