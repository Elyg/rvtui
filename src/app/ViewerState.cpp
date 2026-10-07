#include "app/ViewerState.h"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace fs = std::filesystem;

namespace rv
{

fs::path Source::frame(int i) const
{
	if(m_entry.m_kind != Entry::Kind::SEQUENCE)
	{
		return m_entry.m_path;
	}
	return m_entry.m_frames[std::clamp(
	    i, 0, static_cast<int>(m_entry.m_frames.size()) - 1)];
}

int Source::frameCount() const
{
	return m_entry.m_kind == Entry::Kind::SEQUENCE
	           ? static_cast<int>(m_entry.m_frames.size())
	           : 1;
}

std::string Source::frameLabel(int i) const
{
	if(m_entry.m_kind != Entry::Kind::SEQUENCE)
	{
		return "";
	}
	return std::to_string(
	    m_entry.m_frameNumbers[std::clamp(i, 0, frameCount() - 1)]);
}

int Source::indexForFrameNumber(int number) const
{
	const auto& nums = m_entry.m_frameNumbers;
	if(m_entry.m_kind != Entry::Kind::SEQUENCE || nums.empty())
	{
		return 0;
	}
	const auto it = std::ranges::lower_bound(nums, number);
	if(it == nums.begin())
	{
		return 0;
	}
	if(it == nums.end())
	{
		return static_cast<int>(nums.size()) - 1;
	}
	const auto before = std::prev(it);
	const auto nearest = *it - number < number - *before ? it : before;
	return static_cast<int>(nearest - nums.begin());
}

std::string sourceKey(const Source& s)
{
	std::error_code ec;
	const fs::path dir = fs::absolute(s.m_entry.m_path, ec).parent_path();
	return (dir / s.m_entry.m_name).lexically_normal().string();
}

fs::path ViewerState::currentFramePath() const
{
	return m_sources[m_current].frame(m_frame);
}

int ViewerState::frameCount() const
{
	int n = 1;
	for(const auto& s : m_sources)
	{
		n = std::max(n, s.frameCount());
	}
	return n;
}

FrameRange ViewerState::playRange() const
{
	const int last = frameCount() - 1;
	FrameRange r{std::clamp(m_in.value_or(0), 0, last),
	             std::clamp(m_out.value_or(last), 0, last)};
	return r.m_first <= r.m_last ? r : FrameRange{0, last};
}

const Source* ViewerState::numberedSource() const
{
	if(m_current >= 0 && m_current < static_cast<int>(m_sources.size()) &&
	   m_sources[m_current].m_entry.m_kind == Entry::Kind::SEQUENCE)
	{
		return &m_sources[m_current];
	}
	for(const auto& s : m_sources)
	{
		if(s.m_entry.m_kind == Entry::Kind::SEQUENCE)
		{
			return &s;
		}
	}
	return nullptr;
}

const AnnotationSet* ViewerState::sourceAnnotations(const Annotations& ann,
                                                    int i) const
{
	if(i < 0 || i >= static_cast<int>(m_sources.size()))
	{
		return nullptr;
	}
	return ann.findSource(sourceKey(m_sources[i]));
}

namespace
{

// Dragged columns keep room for a short name, and leave the image some.
constexpr int MIN_LEFT = 16, MIN_RIGHT = 20, MIN_IMAGE = 10;

// A column `w` cells wide terminal asks for: `frac` of it once dragged (0:
// not), else `fallback`.
int askedWidth(double frac, int w, int fallback, int least)
{
	return frac > 0 ? std::max(least, static_cast<int>(std::lround(frac * w)))
	                : fallback;
}

int askedLeft(double frac, int w)
{
	return askedWidth(frac, w, std::max(26, w * 20 / 100), MIN_LEFT);
}

int askedRight(double frac, int w)
{
	return askedWidth(frac, w, std::max(32, w * 30 / 100), MIN_RIGHT);
}

} // namespace

int ViewerState::leftPanelWidth() const
{
	const int w = ftxui::Terminal::Size().dimx;
	const int asked = askedLeft(m_leftFrac, w);
	if(m_leftFrac <= 0)
	{
		return asked;
	}
	// Against the right column as asked (not as fitted: that is fitted to
	// this one).
	return std::min(asked,
	                std::max(MIN_LEFT,
	                         w - askedRight(m_rightFrac, w) - 2 - MIN_IMAGE));
}

int ViewerState::sidePanelWidth() const
{
	const int w = ftxui::Terminal::Size().dimx;
	const int asked = askedRight(m_rightFrac, w);
	if(m_rightFrac <= 0)
	{
		return asked;
	}
	return std::min(asked,
	                std::max(MIN_RIGHT, w - leftPanelWidth() - 2 - MIN_IMAGE));
}

} // namespace rv
