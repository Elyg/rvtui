#include "image/ImageBuffer.h"

#include "util/Parallel.h"

#include <algorithm>
#include <cctype>

namespace rv
{

std::string LayerInfo::shortName(const std::string& full)
{
	auto dot = full.rfind('.');
	return dot == std::string::npos ? full : full.substr(dot + 1);
}

std::string LayerInfo::label() const
{
	if(m_layer.empty())
	{
		return m_partName.empty() ? "rgba" : m_partName;
	}
	if(!m_partName.empty() && m_partName != m_layer)
	{
		return m_partName + "/" + m_layer;
	}
	return m_layer;
}

int ImageInfo::findLayer(const std::string& label) const
{
	for(size_t i = 0; i < m_layers.size(); ++i)
	{
		if(m_layers[i].label() == label || m_layers[i].m_layer == label)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

LayerImage downsample(const LayerImage& src, int factor)
{
	if(factor <= 1)
	{
		return src;
	}
	LayerImage out;
	out.m_info = src.m_info;
	out.m_dataWindow = src.m_dataWindow;
	out.m_displayWindow = src.m_displayWindow;
	out.m_channelNames = src.m_channelNames;
	out.m_reduce = src.m_reduce * factor;
	out.m_width = std::max(1, src.m_width / factor);
	out.m_height = std::max(1, src.m_height / factor);
	const float inv = 1.0f / static_cast<float>(factor * factor);
	for(const auto& plane : src.m_planes)
	{
		const size_t n = static_cast<size_t>(out.m_width) * out.m_height;
		Plane dst = plane.isHalf() ? Plane::halves(n) : Plane::floats(n);
		parallelFor(out.m_height,
		            32,
		            [&](int yBegin, int yEnd)
		            {
			            for(int y = yBegin; y < yEnd; ++y)
			            {
				            for(int x = 0; x < out.m_width; ++x)
				            {
					            float sum = 0;
					            for(int dy = 0; dy < factor; ++dy)
					            {
						            const size_t row =
						                static_cast<size_t>(y * factor + dy) *
						                src.m_width;
						            for(int dx = 0; dx < factor; ++dx)
						            {
							            sum += plane[row + x * factor + dx];
						            }
					            }
					            dst.set(static_cast<size_t>(y) * out.m_width +
					                        x,
					                    sum * inv);
				            }
			            }
		            });
		out.m_planes.push_back(std::move(dst));
	}
	return out;
}

ChannelMap mapChannels(const std::vector<std::string>& names)
{
	auto find = [&](std::initializer_list<const char*> candidates)
	{
		for(const char* c : candidates)
		{
			for(size_t i = 0; i < names.size(); ++i)
			{
				std::string n = names[i];
				if(n == c)
				{
					return static_cast<int>(i);
				}
			}
		}
		for(const char* c : candidates) // case-insensitive second pass
		{
			for(size_t i = 0; i < names.size(); ++i)
			{
				std::string n = names[i], cc = c;
				std::transform(n.begin(), n.end(), n.begin(), ::tolower);
				std::transform(cc.begin(), cc.end(), cc.begin(), ::tolower);
				if(n == cc)
				{
					return static_cast<int>(i);
				}
			}
		}
		return -1;
	};
	ChannelMap m;
	m.m_r = find({"R", "red", "X", "U"});
	m.m_g = find({"G", "green", "Y", "V"});
	m.m_b = find({"B", "blue", "Z", "W"});
	m.m_a = find({"A", "alpha"});
	int present = (m.m_r >= 0) + (m.m_g >= 0) + (m.m_b >= 0);
	if(present == 0)
	{
		// No recognised colour channel: show the first non-alpha channel as grey.
		int first = -1;
		for(size_t i = 0; i < names.size(); ++i)
		{
			if(static_cast<int>(i) != m.m_a)
			{
				first = static_cast<int>(i);
				break;
			}
		}
		m.m_r = m.m_g = m.m_b = first >= 0 ? first : m.m_a;
	}
	else if(present == 1)
	{
		// Single channel (Y luminance, Z depth, ...): grey.
		m.m_r = m.m_g = m.m_b = std::max({m.m_r, m.m_g, m.m_b});
	}
	return m;
}

} // namespace rv
