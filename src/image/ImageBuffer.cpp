#include "image/ImageBuffer.h"

#include "util/Parallel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>

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

void scanLayer(LayerImage& img)
{
	const size_t planes = img.m_planes.size();
	std::mutex mu;
	int64_t nan = 0, inf = 0;
	std::vector<ChannelStats> stats(planes);
	std::vector<double> sums(planes, 0.0);
	parallelFor(img.m_height,
	            64,
	            [&](int yBegin, int yEnd)
	            {
		            const size_t i0 = static_cast<size_t>(yBegin) * img.m_width;
		            const size_t n =
		                static_cast<size_t>(yEnd - yBegin) * img.m_width;
		            // Per pixel: bit 0 a NaN in some channel, bit 1 an inf. A
		            // plane at a time, so each loop runs over one array of one
		            // type.
		            std::vector<uint8_t> bad(n, 0);
		            std::vector<ChannelStats> st(planes);
		            std::vector<double> sum(planes, 0.0);
		            auto scan = [&](size_t c, auto get)
		            {
			            float lo = INFINITY, hi = -INFINITY;
			            double total = 0;
			            int64_t count = 0;
			            for(size_t k = 0; k < n; ++k)
			            {
				            const float v = get(i0 + k);
				            if(!std::isfinite(v))
				            {
					            bad[k] |= std::isnan(v) ? 1 : 2;
					            continue;
				            }
				            lo = std::min(lo, v);
				            hi = std::max(hi, v);
				            total += v;
				            ++count;
			            }
			            st[c] = {lo, hi, 0, count};
			            sum[c] = total;
		            };
		            for(size_t c = 0; c < planes; ++c)
		            {
			            const Plane& p = img.m_planes[c];
			            if(const uint16_t* h = p.halfData())
			            {
				            scan(c,
				                 [h](size_t i) { return halfToFloat(h[i]); });
			            }
			            else
			            {
				            const float* f = p.floatData();
				            scan(c, [f](size_t i) { return f[i]; });
			            }
		            }
		            int64_t nn = 0, ni = 0;
		            for(uint8_t b : bad)
		            {
			            nn += b & 1;
			            ni += b >> 1;
		            }
		            std::lock_guard lk(mu);
		            nan += nn;
		            inf += ni;
		            for(size_t c = 0; c < planes; ++c)
		            {
			            ChannelStats& to = stats[c];
			            const ChannelStats& from = st[c];
			            if(!from.m_count)
			            {
				            continue;
			            }
			            to.m_min = to.m_count ? std::min(to.m_min, from.m_min)
			                                  : from.m_min;
			            to.m_max = to.m_count ? std::max(to.m_max, from.m_max)
			                                  : from.m_max;
			            to.m_count += from.m_count;
			            sums[c] += sum[c];
		            }
	            });
	for(size_t c = 0; c < planes; ++c)
	{
		if(stats[c].m_count)
		{
			stats[c].m_mean = sums[c] / static_cast<double>(stats[c].m_count);
		}
	}
	img.m_nanPixels = nan;
	img.m_infPixels = inf;
	img.m_stats = std::move(stats);
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
	out.m_nanPixels = src.m_nanPixels;
	out.m_infPixels = src.m_infPixels;
	out.m_stats = src.m_stats;
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
