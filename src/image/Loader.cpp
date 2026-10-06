#include "image/Loader.h"

#include <algorithm>
#include <cctype>

namespace rv
{

namespace
{
std::string lowerExt(const std::filesystem::path& p)
{
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(), ::tolower);
	return e;
}
} // namespace

bool isSupportedImage(const std::filesystem::path& p)
{
	auto e = lowerExt(p);
	return e == ".exr" || e == ".png" || e == ".jpg" || e == ".jpeg";
}

ImageInfo probeImage(const std::filesystem::path& p)
{
	return lowerExt(p) == ".exr" ? probeExr(p) : probeStb(p);
}

LayerImage loadLayer(const ImageInfo& info, int layerIndex)
{
	if(layerIndex < 0 || layerIndex >= static_cast<int>(info.m_layers.size()))
	{
		throw LoadError("layer index out of range");
	}
	LayerImage img = info.m_format == "exr" ? loadExrLayer(info, layerIndex)
	                                        : loadStbLayer(info, layerIndex);
	scanLayer(img);
	return img;
}

} // namespace rv
