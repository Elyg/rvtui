#include "app/ViewerState.h"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>

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

const AnnotationSet* ViewerState::sourceAnnotations(const Annotations& ann,
                                                    int i) const
{
	if(i < 0 || i >= static_cast<int>(m_sources.size()))
	{
		return nullptr;
	}
	return ann.findSource(sourceKey(m_sources[i]));
}

int leftPanelWidth()
{
	return std::max(26, ftxui::Terminal::Size().dimx * 20 / 100);
}

int sidePanelWidth()
{
	return std::max(32, ftxui::Terminal::Size().dimx * 30 / 100);
}

} // namespace rv
