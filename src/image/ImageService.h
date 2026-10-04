#pragma once

#include "image/ImageBuffer.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rv
{

using ImageInfoPtr = std::shared_ptr<const ImageInfo>;

/// Asynchronous, cached access to image headers and decoded layers. Every
/// getter is non-blocking: it returns what is cached (or nullptr) and schedules
/// the missing work on a small thread pool; `notify` is called (from a worker
/// thread) whenever something asked for with Priority::NOW lands. Prefetches
/// land quietly: nothing on screen waits for them.
class ImageService
{
public:
	enum class Priority
	{
		NOW,
		PREFETCH
	};

	ImageService(std::function<void()> notify,
	             size_t budgetBytes = size_t(2) << 30,
	             int threads = 3);
	~ImageService();

	ImageInfoPtr info(const std::filesystem::path& p,
	                  Priority pr = Priority::NOW);

	/// Layer selected by label (stable across sequence frames); falls back to
	/// the first layer. `reduce` > 1 caches a box-downsampled copy (preview /
	/// playback).
	LayerImagePtr layer(const std::filesystem::path& p,
	                    const std::string& layerLabel,
	                    int reduce,
	                    Priority pr = Priority::NOW);

	/// Like layer(), but if the requested resolution isn't ready yet returns
	/// any cached resolution of the same layer (best first) so the view never
	/// goes blank.
	LayerImagePtr layerBestEffort(const std::filesystem::path& p,
	                              const std::string& layerLabel,
	                              int reduce,
	                              Priority pr = Priority::NOW);

	bool hasLayer(const std::filesystem::path& p,
	              const std::string& layerLabel,
	              int reduce);

	std::optional<std::string> error(const std::filesystem::path& p);

	/// Re-stat `paths`; any whose mtime/size changed (re-rendered, overwritten,
	/// deleted) has its cached header, layers and load error dropped, so the
	/// next request decodes the new contents. Returns true if anything changed.
	bool checkForChanges(const std::vector<std::filesystem::path>& paths);

	/// Drop queued prefetch jobs (e.g. when the playback range or selection
	/// changes).
	void clearPrefetch();

	size_t bytesUsed();
	size_t budget() const
	{
		return m_budget;
	}
	size_t pendingJobs();

private:
	struct Job
	{
		std::string m_key;
		std::string m_ident; ///< path|stamp: what errors and headers key on
		std::filesystem::path m_path;
		std::optional<std::string> m_layer; ///< nullopt: header only
		int m_reduce = 1;
		Priority m_pr;
	};
	struct CacheEntry
	{
		std::variant<ImageInfoPtr, LayerImagePtr> m_value;
		size_t m_bytes = 0;
		std::list<std::string>::iterator m_lru;
	};

	/// "path|mtime:size" for `p`; stats the file the first time it is seen.
	/// Every cache key includes it, so a changed file is a new image. m_mu held.
	std::string identLocked(const std::filesystem::path& p);
	static std::string layerKey(const std::string& ident,
	                            const std::string& layer,
	                            int reduce);
	void enqueue(Job job);
	void run();
	void put(const std::string& key, CacheEntry entry);
	template <class T>
	std::shared_ptr<const T> get(const std::string& key);

	std::function<void()> m_notify;
	size_t m_budget;
	std::mutex m_mu;
	std::condition_variable m_cv;
	std::deque<Job> m_queue;
	/// Queued or running, by key; true once asked for with Priority::NOW.
	std::unordered_map<std::string, bool> m_inFlight;
	std::unordered_map<std::string, CacheEntry> m_cache;
	std::list<std::string> m_lru; ///< front = most recent
	std::unordered_map<std::string, std::string> m_errors; ///< by ident
	std::unordered_map<std::string, std::string> m_stamps; ///< path → stamp
	size_t m_bytes = 0;
	bool m_stop = false;
	std::vector<std::thread> m_threads;
};

} // namespace rv
