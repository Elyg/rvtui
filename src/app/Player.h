#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <thread>

namespace rv
{

/// Playback clock and read-ahead for the viewer. A ticker thread calls `wake`
/// when the next frame is due (and a few times per frame while it is late);
/// the UI thread then calls tick(), which says whether to show the next frame.
/// Frames are never skipped: when the next one is not decoded yet, playback
/// stalls and the measured fps drops.
class Player
{
public:
	using Clock = std::chrono::steady_clock;
	using NowFn = std::function<Clock::time_point()>;

	static constexpr double DEFAULT_FPS = 24.0;
	static constexpr int MIN_AHEAD = 2, MAX_AHEAD = 96;

	/// @param wake called from the ticker thread while playing (empty: no
	///             ticker thread, e.g. in tests).
	/// @param now  the clock (a fake one in tests).
	explicit Player(std::function<void()> wake, NowFn now = &Clock::now);
	~Player();
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	bool playing() const noexcept
	{
		return m_playing;
	}
	void play();
	void pause();

	/// The chosen rate; 0 = take it from the file once known.
	double fps() const noexcept
	{
		return m_fps;
	}
	void setFps(double fps) noexcept
	{
		m_fps = fps;
	}
	/// The rate playback aims at: the chosen one, else the file's, else 24.
	double targetFps(std::optional<double> fileFps) const noexcept
	{
		return m_fps > 0 ? m_fps : fileFps.value_or(DEFAULT_FPS);
	}
	/// Smoothed rate of the frames actually shown: from the time between
	/// them, not from how late each was against the schedule.
	double measuredFps() const noexcept
	{
		return m_measuredFps;
	}
	/// When the next frame is due (the ticker wakes the UI then).
	Clock::time_point nextDue() const noexcept
	{
		return Clock::time_point(Clock::duration(m_due.load()));
	}
	/// How often the ticker wakes the UI while the next frame is overdue
	/// (stalled on decoding, or the UI is busy): four times per frame.
	int tickerIntervalMs() const noexcept
	{
		return m_tickerIntervalMs;
	}
	/// Back to a fresh state (paused, fps from the file).
	void reset();

	/// Frames to read ahead: as many as fit in 60% of the cache budget, shared
	/// by `sources`, within MIN_AHEAD..MAX_AHEAD.
	static int readAhead(std::size_t budgetBytes,
	                     double frameBytes,
	                     std::size_t sources) noexcept;

	/// One tick on the UI thread, showing `frame` of `frameCount`. Asks for
	/// the next `ahead` frames with `prefetch(f)`, then, once a frame period
	/// has passed, returns the next frame if `ready(f)` (its pixels are
	/// cached; ready() asks for them urgently when not).
	template <class Prefetch, class Ready>
	std::optional<int> tick(int frame,
	                        int frameCount,
	                        int ahead,
	                        Prefetch&& prefetch,
	                        Ready&& ready);

private:
	void startTicker();
	void stopTicker();
	/// Seconds since the last advance; updates the ticker interval.
	double sinceAdvance(Clock::time_point now, double fps);
	/// Record an advance at `now` (`since` seconds after the scheduled
	/// reference).
	void advanced(Clock::time_point now, double since, double fps);
	void setDue(double fps);

	std::function<void()> m_wake;
	NowFn m_now;
	bool m_playing = false;
	double m_fps = 0;
	double m_measuredFps = 0;
	double m_interval = 0;           ///< smoothed seconds between shown frames
	Clock::time_point m_lastAdvance; ///< the schedule: when the frame was due
	std::optional<Clock::time_point> m_lastShown; ///< when it really advanced
	std::atomic<Clock::rep> m_due{0};             ///< m_lastAdvance + a period
	std::atomic<int> m_tickerIntervalMs{20};
	std::jthread m_ticker; ///< last: stops before the rest goes
};

template <class Prefetch, class Ready>
std::optional<int> Player::tick(
    int frame, int frameCount, int ahead, Prefetch&& prefetch, Ready&& ready)
{
	const double fps = m_fps > 0 ? m_fps : DEFAULT_FPS;
	const auto now = m_now();
	const double since = sinceAdvance(now, fps);
	const int n = std::max(1, frameCount);
	const int next = (frame + 1) % n;
	const int count = std::min(ahead, n - 1);
	for(int i = 0; i < count; ++i)
	{
		prefetch((next + i) % n);
	}
	if(since < 1.0 / fps || !ready(next))
	{
		return std::nullopt; // too early, or stall rather than skip
	}
	advanced(now, since, fps);
	return next;
}

} // namespace rv
