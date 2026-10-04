#include "app/Viewer.h"

#include "util/Ui.h"

#include <ftxui/screen/terminal.hpp>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

namespace
{
const double FPS_CHOICES[] = {12, 23.976, 24, 25, 30, 48, 60};
} // namespace

Viewer::Viewer(AppContext& ctx, std::function<void()> onClose)
    : m_ctx(ctx), m_onClose(std::move(onClose)), m_viewSlot(newSlot(ctx)),
      m_meta(m_state, ctx), m_files(m_state, ctx), m_inspector(m_state, ctx),
      m_layers(m_state), m_player(
                             [this]
                             {
	                             // From the ticker thread. A posted task, so a
	                             // tick that shows nothing new costs no redraw.
	                             m_ctx.m_post(
	                                 [this]
	                                 {
		                                 if(tick())
		                                 {
			                                 m_ctx.m_redraw();
		                                 }
	                                 });
                             })
{
}

// --- open / close ---

void Viewer::open(std::vector<Entry> entries, bool tile)
{
	m_state.m_sources.clear();
	for(auto& e : entries)
	{
		m_state.m_sources.push_back({std::move(e)});
		m_ctx.m_ann.touch(sourceKey(m_state.m_sources.back()));
	}
	m_state.m_current = 0;
	m_state.m_frame = 0;
	m_state.m_view = ViewParams{};
	m_state.m_picked.reset();
	m_fitBesidePanel = false;
	m_tile = tile;
	m_meta.resetScroll();
	m_player.reset(); // fps from the file once known
}

void Viewer::close()
{
	m_player.pause();
	m_ctx.m_svc.clearPrefetch();
	m_tile = false;
	m_state.m_picked.reset(); // a pick belongs to this viewing session
	m_onClose();
}

bool Viewer::rescan()
{
	bool changed = false;
	for(auto& src : m_state.m_sources)
	{
		Entry& en = src.m_entry;
		if(en.m_kind != Entry::Kind::SEQUENCE)
		{
			continue;
		}
		// A sequence being rendered grows (or shrinks).
		Entry again = entryForPath(en.m_frames.front());
		if(again.m_kind == Entry::Kind::SEQUENCE &&
		   again.m_frames != en.m_frames)
		{
			spdlog::info("sequence {} now {} frames",
			             again.m_name,
			             again.m_frames.size());
			en = std::move(again);
			m_state.m_frame =
			    std::min(m_state.m_frame, m_state.frameCount() - 1);
			changed = true;
		}
	}
	return changed;
}

std::vector<fs::path> Viewer::shownDirs() const
{
	std::vector<fs::path> dirs;
	for(const auto& s : m_state.m_sources)
	{
		fs::path dir = s.frame(0).parent_path();
		if(std::ranges::find(dirs, dir) == dirs.end())
		{
			dirs.push_back(std::move(dir));
		}
	}
	return dirs;
}

int Viewer::reduceFor(const ImageInfo& info, const ImageSlot& slot) const
{
	const bool fit = &slot != m_viewSlot.get() || m_state.m_view.m_fit;
	// While playing, decode up to 25% below the size sent: a 4K plate on a
	// ~2000 px wide view then caches at 1920 px (33 MB) instead of 4K
	// (132 MB), and a whole sequence fits in the cache instead of being
	// decoded again on every loop.
	constexpr double PLAYBACK_SLACK = 1.25;
	return m_ctx.reduceFor(slot,
	                       info.fitBounds(m_state.m_layerLabel),
	                       fit ? std::nullopt
	                           : std::optional(m_state.m_view.m_zoom),
	                       pixelCap(),
	                       m_player.playing() ? PLAYBACK_SLACK : 1.0);
}

int Viewer::pixelCap() const
{
	int cap = m_tile
	              ? TOTAL_PIXEL_BUDGET /
	                    std::max<int>(1, static_cast<int>(m_tileSlots.size()))
	              : TOTAL_PIXEL_BUDGET;
	if(m_player.playing())
	{
		if(!m_tile)
		{
			cap /= 2;
		}
		// Inline transfer (ssh): every pixel crosses the link, compressed
		// but base64'd; halve it again to keep playback near frame rate.
		if(m_ctx.m_caps.m_transfer == Transfer::DIRECT)
		{
			cap /= 2;
		}
	}
	return cap;
}

const ImageSlot& Viewer::playbackSlot() const
{
	return m_tile && !m_tileSlots.empty() ? *m_tileSlots.front() : *m_viewSlot;
}

bool Viewer::showsSource(int i) const
{
	return (m_tile && m_state.m_sources.size() > 1) || i == m_state.m_current;
}

// --- actions ---

void Viewer::cycleLayer(const ImageInfoPtr& info, int delta)
{
	if(!info || info->m_layers.empty())
	{
		return;
	}
	int n = static_cast<int>(info->m_layers.size());
	int i = info->findLayer(m_state.m_layerLabel);
	i = ((i < 0 ? 0 : i) + delta + n) % n;
	m_state.m_layerLabel = info->m_layers[i].label();
}

void Viewer::zoomBy(double factor,
                    std::optional<std::pair<double, double>> anchor)
{
	m_fitBesidePanel = false; // zooming / panning takes over from fit
	if(m_state.m_view.m_fit)
	{
		if(!m_viewSlot->drawn())
		{
			return;
		}
		m_state.m_view.m_zoom = m_viewSlot->effectiveZoom();
		auto m = m_viewSlot->mapping();
		auto [aw, ah] = m_viewSlot->areaPixels();
		double q =
		    m.m_scale / m_state.m_view.m_zoom; // output px per terminal px
		m_state.m_view.m_centerX = m.imageX(aw * q / 2.0);
		m_state.m_view.m_centerY = m.imageY(ah * q / 2.0);
		m_state.m_view.m_fit = false;
	}
	m_state.m_view.m_zoom =
	    std::clamp(m_state.m_view.m_zoom * factor, 1.0 / 64, 256.0);
	if(anchor)
	{
		m_state.m_view.m_centerX =
		    anchor->first + (m_state.m_view.m_centerX - anchor->first) / factor;
		m_state.m_view.m_centerY =
		    anchor->second +
		    (m_state.m_view.m_centerY - anchor->second) / factor;
	}
}

void Viewer::fitView(const ImageInfoPtr& info)
{
	m_state.m_view.m_fit = true;
	m_fitBesidePanel = false;
	// Side panels are drawn over the image; fit into what is left of them.
	if(m_tile || !sidePanelOpen() || !info)
	{
		return;
	}
	// From the terminal size, not the last draw, so it is right straight
	// after a resize (the view spans the full width, minus HUD + status).
	const auto term = Terminal::Size();
	const int aw = term.dimx * m_viewSlot->pxPerCellX();
	const int ah = std::max(0, term.dimy - 2) * m_viewSlot->pxPerCellY();
	const int leftPx = coveredLeft() * m_viewSlot->pxPerCellX();
	const int visibleW =
	    aw - leftPx - coveredRight() * m_viewSlot->pxPerCellX();
	const Box2i dw = info->fitBounds(m_state.m_layerLabel);
	if(visibleW <= 0 || ah <= 0 || dw.width() <= 0 || dw.height() <= 0)
	{
		return;
	}
	m_state.m_view.m_fit = false;
	// 10% breathing room so the image does not butt against the panel.
	constexpr double PADDED = 0.9;
	m_state.m_view.m_zoom =
	    PADDED * std::min(visibleW / static_cast<double>(dw.width()),
	                      ah / static_cast<double>(dw.height()));
	// The view centre is the middle of the whole area; shift it so the image
	// sits in the middle of the uncovered part (between the columns).
	m_state.m_view.m_centerX =
	    dw.m_x0 + dw.width() / 2.0 +
	    (aw / 2.0 - (leftPx + visibleW / 2.0)) / m_state.m_view.m_zoom;
	m_state.m_view.m_centerY = dw.m_y0 + dw.height() / 2.0;
	m_fitBesidePanel = true; // re-done every draw until zoom / pan
}

void Viewer::pan(double dxCells, double dyCells)
{
	m_fitBesidePanel = false;
	if(m_state.m_view.m_fit)
	{
		zoomBy(1.0);
	}
	m_state.m_view.m_centerX +=
	    dxCells * m_viewSlot->pxPerCellX() / m_state.m_view.m_zoom;
	m_state.m_view.m_centerY +=
	    dyCells * m_viewSlot->pxPerCellY() / m_state.m_view.m_zoom;
}

void Viewer::setFrame(int f)
{
	int n = m_state.frameCount();
	m_state.m_frame = ((f % n) + n) % n;
}

void Viewer::togglePlay()
{
	if(m_state.frameCount() < 2)
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	if(m_player.playing())
	{
		m_player.pause();
		m_ctx.m_svc.clearPrefetch();
	}
	else
	{
		m_player.play();
	}
}

void Viewer::prefetchFrame(int f)
{
	auto& svc = m_ctx.m_svc;
	const ImageSlot& slot = playbackSlot();
	for(int i = 0; i < static_cast<int>(m_state.m_sources.size()); ++i)
	{
		if(!showsSource(i))
		{
			continue;
		}
		const fs::path path = m_state.m_sources[i].frame(f);
		auto info = svc.info(path, ImageService::Priority::PREFETCH);
		if(!info)
		{
			continue;
		}
		svc.layer(path,
		          m_state.m_layerLabel,
		          reduceFor(*info, slot),
		          ImageService::Priority::PREFETCH);
	}
}

void Viewer::prepareAhead(int f)
{
	auto& svc = m_ctx.m_svc;
	auto ahead = [&](ImageSlot& slot, const fs::path& path, const auto& layer)
	{
		auto info = svc.info(path);
		if(!info)
		{
			return;
		}
		const int reduce = reduceFor(*info, slot);
		if(svc.hasLayer(path, layer, reduce))
		{
			slot.prepareAhead(svc.layer(path, layer, reduce));
		}
	};
	if(!m_tile)
	{
		ahead(*m_viewSlot,
		      m_state.m_sources[m_state.m_current].frame(f),
		      m_state.m_layerLabel);
		return;
	}
	const bool bySource = m_state.m_sources.size() > 1;
	const size_t n = std::min(m_tileSlots.size(), m_tileRefs.size());
	for(size_t i = 0; i < n; ++i)
	{
		const auto& src = m_state.m_sources[bySource ? i : m_state.m_current];
		ahead(*m_tileSlots[i], src.frame(f), m_tileRefs[i].m_layer);
	}
}

bool Viewer::tick()
{
	if(!m_player.playing())
	{
		return false;
	}
	auto& svc = m_ctx.m_svc;
	auto info = svc.info(m_state.currentFramePath());
	if(!info)
	{
		return false;
	}
	const int reduce = reduceFor(*info, playbackSlot());
	// Read ahead: as many frames as fit comfortably in the cache.
	const auto& dw = info->displayWindow();
	const double frameBytes = static_cast<double>(dw.width()) / reduce *
	                          dw.height() / reduce * 4 * sizeof(float);
	const int ahead =
	    Player::readAhead(svc.budget(), frameBytes, m_state.m_sources.size());
	auto next = m_player.tick(
	    m_state.m_frame,
	    m_state.frameCount(),
	    ahead,
	    [&](int f) { prefetchFrame(f); },
	    [&](int f)
	    {
		    bool ready = true;
		    for(int i = 0; i < static_cast<int>(m_state.m_sources.size()); ++i)
		    {
			    const fs::path path = m_state.m_sources[i].frame(f);
			    if(showsSource(i) &&
			       !svc.hasLayer(path, m_state.m_layerLabel, reduce))
			    {
				    svc.layer(path, m_state.m_layerLabel, reduce); // urgent
				    ready = false;
			    }
		    }
		    return ready;
	    });
	if(!next)
	{
		return false;
	}
	m_state.m_frame = *next;
	prepareAhead((*next + 1) % std::max(1, m_state.frameCount()));
	return true;
}

// --- annotations ---

KeyLookup Viewer::keyLookup(int source,
                            const fs::path& frame,
                            const ImageInfoPtr& info,
                            const std::string& layer) const
{
	return
	    [this, source, frame, info, layer](const std::string& key, bool builtin)
	        -> std::optional<std::string>
	{
		if(builtin)
		{
			if(key == "file")
			{
				return frame.filename().string();
			}
			if(key == "frame")
			{
				return source >= 0 && source < static_cast<int>(
				                                   m_state.m_sources.size())
				           ? m_state.m_sources[source].frameLabel(
				                 m_state.m_frame)
				           : std::string();
			}
			if(key == "layer")
			{
				return layer.empty() ? std::string("rgba") : layer;
			}
			if(key == "res" && info)
			{
				const Box2i& d = info->displayWindow();
				return fmt::format("{}x{}", d.width(), d.height());
			}
			if(key == "fps")
			{
				return fmt::format("{:.3g}",
				                   m_player.targetFps(info ? info->m_fps
				                                           : std::nullopt));
			}
			if(key == "date")
			{
				const std::time_t t = std::time(nullptr);
				std::tm tm{};
				localtime_r(&t, &tm);
				return fmt::format("{:%Y-%m-%d}", tm);
			}
			return std::nullopt;
		}
		if(!info)
		{
			return std::nullopt;
		}
		// The shown layer's part first, then part 0.
		const int l = info->findLayer(layer);
		const size_t part =
		    l >= 0 ? static_cast<size_t>(info->m_layers[l].m_part) : 0;
		for(size_t p : {part, size_t{0}})
		{
			if(p >= info->m_parts.size())
			{
				continue;
			}
			for(const auto& a : info->m_parts[p].m_attributes)
			{
				if(a.m_name == key)
				{
					return a.m_value;
				}
			}
		}
		return std::nullopt;
	};
}

OverlayText Viewer::overlayText(std::initializer_list<const AnnotationSet*> sets,
                                const KeyLookup& lookup) const
{
	OverlayText out;
	if(!m_ctx.m_ann.m_visible)
	{
		return out;
	}
	for(const AnnotationSet* set : sets)
	{
		if(!set)
		{
			continue;
		}
		for(int s = 0; s < SLOT_COUNT; ++s)
		{
			for(const auto& line : set->m_slots[s])
			{
				out[s].push_back(resolveLine(line, lookup));
			}
		}
	}
	return out;
}

// --- events ---

void Viewer::togglePanes()
{
	if(sidePanelOpen())
	{
		m_hiddenPanes = HiddenPanes{m_meta.isOpen(),
		                            m_files.isOpen(),
		                            m_inspector.isOpen(),
		                            m_layers.isOpen(),
		                            m_state.m_focus};
		m_meta.setOpen(false);
		m_files.setOpen(false);
		m_inspector.setOpen(false);
		m_layers.setOpen(false);
		m_state.m_focus = Focus::IMAGE;
	}
	else if(m_hiddenPanes)
	{
		m_meta.setOpen(m_hiddenPanes->m_meta);
		m_files.setOpen(m_hiddenPanes->m_files);
		m_inspector.setOpen(m_hiddenPanes->m_inspector);
		m_layers.setOpen(m_hiddenPanes->m_layers);
		m_state.m_focus = m_hiddenPanes->m_focus;
		m_hiddenPanes.reset();
	}
	else
	{
		m_ctx.m_message = "no panes to show";
	}
}

bool Viewer::event(Event e)
{
	ImageInfoPtr info = m_ctx.m_svc.info(m_state.currentFramePath());
	Focus& focus = m_state.m_focus;
	if((focus == Focus::META && !m_meta.isOpen()) ||
	   (focus == Focus::FILES && !m_files.isOpen()) ||
	   (focus == Focus::INSPECT && !m_inspector.isOpen()) ||
	   (focus == Focus::LAYERS && !m_layers.isOpen()))
	{
		focus = Focus::IMAGE;
	}
	// Letter aliases for the pane numbers (RV-ish): m metadata, o files,
	// i inspector, / layers. Not while typing, nor `o` inside an annotation
	// set, where it adds a line.
	const bool annKeys =
	    focus == Focus::FILES && m_files.annotations().isOpen();
	if(!typing() && e.is_character())
	{
		static const std::pair<const char*, const char*> ALIASES[] =
		    {{"m", "2"}, {"o", "3"}, {"i", "4"}, {"/", "5"}};
		for(const auto& [letter, number] : ALIASES)
		{
			if(e == Event::Character(letter) && !(annKeys && *letter == 'o'))
			{
				e = Event::Character(number);
				break;
			}
		}
	}
	if((focus == Focus::META && m_meta.event(e, info)) ||
	   (focus == Focus::FILES && m_files.event(e)) ||
	   (focus == Focus::INSPECT && m_inspector.event(e)) ||
	   (focus == Focus::LAYERS && m_layers.event(e, info)))
	{
		return true;
	}
	if(e == Event::CtrlL)
	{
		if(m_meta.isOpen() && focus == Focus::IMAGE)
		{
			focus = Focus::META; // into the metadata column
		}
		else
		{
			m_ctx.tmuxSelectPane('R');
		}
		return true;
	}
	if(e == Event::CtrlH)
	{
		m_ctx.tmuxSelectPane('L');
		return true;
	}
	if(e == Event::CtrlK)
	{
		m_ctx.tmuxSelectPane('U');
		return true;
	}
	return e.is_mouse() ? mouseEvent(e, info) : keyEvent(e, info);
}

bool Viewer::mouseEvent(Event e, const ImageInfoPtr& info)
{
	auto m = e.mouse();
	const bool overInfo = m_meta.isOpen() && m_meta.contains(m.x, m.y);
	const bool overFiles = m_files.isOpen() && m_files.contains(m.x, m.y);
	const bool overSide = (rightPanelOpen() && inside(m_sideBox, m.x, m.y)) ||
	                      (leftPanelOpen() && inside(m_leftBox, m.x, m.y));
	const bool wheel =
	    m.button == Mouse::WheelUp || m.button == Mouse::WheelDown;
	if(wheel && overInfo)
	{
		m_meta.scroll(m.button == Mouse::WheelUp ? -3 : 3);
		return true;
	}
	if(wheel)
	{
		if(m_tile || overSide)
		{
			return true;
		}
		double f = m.button == Mouse::WheelUp ? 1.25 : 1.0 / 1.25;
		zoomBy(f, m_viewSlot->imageCoordAt(m.x, m.y));
		return true;
	}
	// Click selects (a tile, or which pane has focus); Ctrl+click picks
	// the colour. macOS terminals often turn Ctrl+click into a right
	// click, so that picks too.
	const bool pressed = m.motion == Mouse::Pressed;
	const bool pick = pressed && ((m.button == Mouse::Left && m.control) ||
	                              m.button == Mouse::Right);
	if(!pressed || (m.button != Mouse::Left && !pick))
	{
		return m_inspector.isOpen(); // redraw for the readout on mouse move
	}
	if(overSide)
	{
		if(overFiles)
		{
			m_files.click(m.y);
		}
		else if(overInfo)
		{
			m_state.m_focus = Focus::META;
		}
		else if(m_inspector.isOpen() && m_inspector.contains(m.x, m.y))
		{
			m_state.m_focus = Focus::INSPECT;
		}
		else if(m_layers.isOpen() && m_layers.contains(m.x, m.y))
		{
			m_layers.click(m.y, info);
		}
		return true;
	}
	m_state.m_focus = Focus::IMAGE;
	// A click on a tile selects it (Enter opens it).
	if(m_tile && !pick)
	{
		for(size_t i = 0; i < m_tileSlots.size(); ++i)
		{
			if(!inside(m_tileSlots[i]->box(), m.x, m.y))
			{
				continue;
			}
			if(m_state.m_sources.size() > 1)
			{
				m_state.m_current = static_cast<int>(i);
			}
			else if(info && i < info->m_layers.size())
			{
				m_state.m_layerLabel = info->m_layers[i].label();
			}
			break;
		}
	}
	Sample s = pick ? sampleAt(m.x, m.y) : Sample{};
	if(s.m_state == Sample::State::OK)
	{
		m_ctx.m_message = "picked " + sampleCoord(s) + "  " + s.m_layer;
		m_state.m_picked = std::move(s);
	}
	return true;
}

bool Viewer::keyEvent(const Event& e, const ImageInfoPtr& info)
{
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	if(m_inspector.isOpen() && e == Event::Escape)
	{
		// Esc backs out of the innermost mode first.
		m_inspector.setOpen(false);
	}
	else if(m_tile && e == Event::Return)
	{
		m_tile = false; // open the selected tile
	}
	else if(ch("q") || e == Event::Escape || e == Event::Backspace)
	{
		if(m_tile)
		{
			m_tile = false;
		}
		else
		{
			close();
		}
	}
	else if(ch("]"))
	{
		cycleLayer(info, 1);
	}
	else if(ch("["))
	{
		cycleLayer(info, -1);
	}
	else if(ch("n"))
	{
		m_state.m_current = (m_state.m_current + 1) %
		                    static_cast<int>(m_state.m_sources.size());
	}
	else if(ch("N"))
	{
		m_state.m_current = (m_state.m_current - 1 +
		                     static_cast<int>(m_state.m_sources.size())) %
		                    static_cast<int>(m_state.m_sources.size());
	}
	else if(ch("c"))
	{
		m_state.m_disp.m_mode = ChannelMode::COLOR;
	}
	else if(ch("r"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::RED
		                            ? ChannelMode::COLOR
		                            : ChannelMode::RED;
	}
	else if(ch("g"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::GREEN
		                            ? ChannelMode::COLOR
		                            : ChannelMode::GREEN;
	}
	else if(ch("b"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::BLUE
		                            ? ChannelMode::COLOR
		                            : ChannelMode::BLUE;
	}
	else if(ch("a"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::ALPHA
		                            ? ChannelMode::COLOR
		                            : ChannelMode::ALPHA;
	}
	else if(ch("u"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::LUMA
		                            ? ChannelMode::COLOR
		                            : ChannelMode::LUMA;
	}
	else if(ch("e"))
	{
		m_state.m_disp.m_exposure -= 0.5f;
	}
	else if(ch("E"))
	{
		m_state.m_disp.m_exposure += 0.5f;
	}
	else if(ch("y"))
	{
		m_state.m_disp.m_gamma = std::max(0.1f, m_state.m_disp.m_gamma - 0.1f);
	}
	else if(ch("Y"))
	{
		m_state.m_disp.m_gamma += 0.1f;
	}
	else if(ch("s"))
	{
		m_state.m_disp.m_srgb = !m_state.m_disp.m_srgb;
	}
	else if(ch("w") && m_tile)
	{
		// Tiles show the frame and the selected tile's dashed highlight:
		// selection → frame + selection → frame → off → selection.
		const bool frame = m_state.m_disp.m_outlines != Outlines::NONE;
		const bool sel = m_tileSelection;
		if(sel && !frame)
		{
			m_state.m_disp.m_outlines = Outlines::FRAME_AND_DATA;
		}
		else if(sel)
		{
			m_tileSelection = false;
		}
		else if(frame)
		{
			m_state.m_disp.m_outlines = Outlines::NONE;
		}
		else
		{
			m_tileSelection = true;
		}
		const bool f = m_state.m_disp.m_outlines != Outlines::NONE;
		m_ctx.m_message = f && m_tileSelection ? "outlines: frame + selection"
		                  : f                  ? "outlines: frame"
		                  : m_tileSelection    ? "outlines: selection"
		                                       : "outlines: off";
	}
	else if(ch("w"))
	{
		// Window outlines: off (default) → frame + data → frame only → off.
		m_state.m_disp.m_outlines =
		    m_state.m_disp.m_outlines == Outlines::FRAME_AND_DATA
		        ? Outlines::FRAME
		    : m_state.m_disp.m_outlines == Outlines::FRAME
		        ? Outlines::NONE
		        : Outlines::FRAME_AND_DATA;
		m_ctx.m_message = m_state.m_disp.m_outlines == Outlines::FRAME_AND_DATA
		                      ? "outlines: frame + data window"
		                  : m_state.m_disp.m_outlines == Outlines::FRAME
		                      ? "outlines: frame only"
		                      : "outlines: off";
	}
	else if(ch("0"))
	{
		m_state.m_disp = DisplayParams{};
	}
	else if(ch("+") || ch("="))
	{
		zoomBy(1.25);
	}
	else if(ch("-") || ch("_"))
	{
		zoomBy(1.0 / 1.25);
	}
	else if(ch("f"))
	{
		fitView(info);
	}
	else if(ch("z"))
	{
		zoomBy(1.0);
		m_state.m_view.m_zoom = 1.0;
	}
	else if(ch("h") || e == Event::ArrowLeft)
	{
		pan(-4, 0);
	}
	else if(ch("l") || e == Event::ArrowRight)
	{
		pan(4, 0);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		pan(0, -2);
	}
	else if(ch("j") || e == Event::ArrowDown)
	{
		pan(0, 2);
	}
	else if(ch("H"))
	{
		pan(-16, 0);
	}
	else if(ch("L"))
	{
		pan(16, 0);
	}
	else if(ch("K"))
	{
		pan(0, -8);
	}
	else if(ch("J"))
	{
		pan(0, 8);
	}
	else if(ch(" "))
	{
		togglePlay();
	}
	else if(ch("."))
	{
		setFrame(m_state.m_frame + 1);
	}
	else if(ch(","))
	{
		setFrame(m_state.m_frame - 1);
	}
	else if(ch("<"))
	{
		setFrame(0);
	}
	else if(ch(">"))
	{
		setFrame(m_state.frameCount() - 1);
	}
	else if(ch("F"))
	{
		const double cur =
		    m_player.targetFps(info ? info->m_fps : std::nullopt);
		double next = FPS_CHOICES[0];
		for(double c : FPS_CHOICES)
		{
			if(c > cur + 1e-3)
			{
				next = c;
				break;
			}
		}
		m_player.setFps(next);
	}
	else if(ch("t"))
	{
		m_tile = !m_tile;
	}
	else if(ch("1"))
	{
		m_state.m_focus = Focus::IMAGE;
	}
	// Pane numbers: open + focus (pressed again while focused, the pane's
	// own handler closes it).
	else if(ch("2"))
	{
		m_meta.setOpen(true);
		m_state.m_focus = Focus::META;
	}
	else if(ch("4"))
	{
		m_inspector.setOpen(true);
		m_state.m_focus = Focus::INSPECT;
	}
	else if(ch("5"))
	{
		m_layers.show(info);
	}
	else if(ch("3"))
	{
		m_files.show();
	}
	else if(e == Event::Tab)
	{
		togglePanes();
	}
	else if(ch("T"))
	{
		// Quick text: a new line in this source's last-used slot.
		m_files.setOpen(true);
		m_state.m_focus = Focus::FILES;
		m_files.annotations().quickAdd();
	}
	else if(ch("A"))
	{
		m_ctx.m_ann.m_visible = !m_ctx.m_ann.m_visible;
		m_ctx.m_message =
		    m_ctx.m_ann.m_visible ? "annotations: on" : "annotations: off";
	}
	else if(ch("}"))
	{
		m_meta.scroll(5);
	}
	else if(ch("{"))
	{
		m_meta.scroll(-5);
	}
	else if(m_meta.isOpen() && e == Event::PageDown)
	{
		m_meta.scroll(20);
	}
	else if(m_meta.isOpen() && e == Event::PageUp)
	{
		m_meta.scroll(-20);
	}
	else
	{
		return false;
	}
	return true;
}

// --- rendering ---

Element Viewer::renderHud(const ImageInfoPtr& info)
{
	const Source& src = m_state.m_sources[m_state.m_current];
	std::string name = src.m_entry.m_name;
	if(m_state.m_sources.size() > 1)
	{
		name = fmt::format("[{}/{}] {}",
		                   m_state.m_current + 1,
		                   m_state.m_sources.size(),
		                   name);
	}
	Elements parts;
	// lazygit-style pane number: `1` focuses the image.
	const bool imageFocused = m_state.m_focus == Focus::IMAGE;
	parts.push_back(text(" [1]") |
	                (imageFocused ? color(Color::Green) | bold : dim));
	parts.push_back(text(" " + name + " ") | bold);
	if(info)
	{
		int li = std::max(0, info->findLayer(m_state.m_layerLabel));
		parts.push_back(text(fmt::format(" {} ({}/{}) ",
		                                 info->m_layers[li].label(),
		                                 li + 1,
		                                 info->m_layers.size())) |
		                color(Color::Cyan) | bold);
	}
	// Channels as coloured letters: lit = shown, dim = hidden.
	const ChannelMode mode = m_state.m_disp.m_mode;
	auto letter = [](const char* l, Color c, bool lit)
	{ return text(l) | color(c) | (lit ? bold : dim); };
	const bool rgb = mode == ChannelMode::COLOR;
	parts.push_back(text(" "));
	parts.push_back(letter("R", Color::Red, rgb || mode == ChannelMode::RED));
	parts.push_back(
	    letter("G", Color::Green, rgb || mode == ChannelMode::GREEN));
	parts.push_back(letter("B", Color::Blue, rgb || mode == ChannelMode::BLUE));
	parts.push_back(letter("A", Color::White, mode == ChannelMode::ALPHA));
	if(mode == ChannelMode::LUMA)
	{
		parts.push_back(text(" luma") | bold);
	}
	parts.push_back(text(" "));
	// Display settings only once they differ from the defaults.
	const DisplayParams defaults;
	std::string disp;
	if(m_state.m_disp.m_exposure != defaults.m_exposure)
	{
		disp += fmt::format(" {:+.1f}ev", m_state.m_disp.m_exposure);
	}
	if(m_state.m_disp.m_gamma != defaults.m_gamma)
	{
		disp += fmt::format(" γ{:.1f}", m_state.m_disp.m_gamma);
	}
	if(m_state.m_disp.m_srgb != defaults.m_srgb)
	{
		disp += m_state.m_disp.m_srgb ? " sRGB" : " raw";
	}
	if(!disp.empty())
	{
		parts.push_back(text(disp + " ") | color(Color::Yellow));
	}
	parts.push_back(
	    text(m_state.m_view.m_fit
	             ? " 🔍 fit "
	             : fmt::format(" 🔍 {:.0f}% ", m_state.m_view.m_zoom * 100)) |
	    dim);
	if(m_state.m_picked)
	{
		parts.push_back(text(" ██ ") |
		                color(Color::RGB(m_state.m_picked->m_r,
		                                 m_state.m_picked->m_g,
		                                 m_state.m_picked->m_b)));
		parts.push_back(text(fmt::format("[{}, {}] ",
		                                 m_state.m_picked->m_x,
		                                 m_state.m_picked->m_y)) |
		                dim);
		parts.push_back(rgbaValues(*m_state.m_picked));
		parts.push_back(text(" "));
	}
	parts.push_back(filler());
	if(m_tile)
	{
		parts.push_back(text(" TILE ") | inverted);
	}
	if(m_inspector.isOpen())
	{
		parts.push_back(text(" INSPECT · Esc ") | inverted |
		                color(Color::Yellow));
	}
	return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
}

Sample Viewer::sampleAt(int cellX, int cellY)
{
	Sample out;
	// Which image is under the cell: a tile, or the main view.
	const ImageSlot* slot = nullptr;
	fs::path path;
	std::string layer;
	if(m_tile)
	{
		for(size_t i = 0; i < m_tileSlots.size() && i < m_tileRefs.size(); ++i)
		{
			if(m_tileSlots[i]->imageCoordAt(cellX, cellY))
			{
				slot = m_tileSlots[i].get();
				path = m_tileRefs[i].m_path;
				layer = m_tileRefs[i].m_layer;
				break;
			}
		}
	}
	else
	{
		slot = m_viewSlot.get();
		path = m_state.currentFramePath();
		layer = m_state.m_layerLabel;
	}
	auto coord = slot ? slot->imageCoordAt(cellX, cellY) : std::nullopt;
	if(!coord)
	{
		return out;
	}
	out.m_state = Sample::State::LOADING;
	out.m_layer = layer;
	out.m_x = static_cast<int>(std::floor(coord->first));
	out.m_y = static_cast<int>(std::floor(coord->second));
	// Exact values come from full resolution, fetched for the readout only;
	// until it lands, read whatever resolution is on screen (marked ≈).
	LayerImagePtr full = m_ctx.m_svc.layer(path, layer, 1);
	LayerImagePtr img =
	    full ? full : m_ctx.m_svc.layerBestEffort(path, layer, 1);
	if(!img)
	{
		return out;
	}
	out.m_exact = full != nullptr;
	bool inside = false;
	for(size_t c = 0; c < img->m_channelNames.size(); ++c)
	{
		auto v = img->at(static_cast<int>(c), out.m_x, out.m_y);
		inside |= v.has_value();
		out.m_values.emplace_back(img->m_channelNames[c], v.value_or(0.0f));
	}
	if(!inside)
	{
		out.m_state = Sample::State::OUTSIDE;
		return out;
	}
	ChannelMap cm = mapChannels(img->m_channelNames);
	auto get = [&](int idx)
	{ return idx >= 0 ? img->at(idx, out.m_x, out.m_y).value_or(0.0f) : 0.0f; };
	float lr = get(cm.m_r), lg = get(cm.m_g), lb = get(cm.m_b);
	out.m_rgba[0] = lr;
	out.m_rgba[1] = lg;
	out.m_rgba[2] = lb;
	out.m_hasAlpha = cm.m_a >= 0;
	out.m_rgba[3] = out.m_hasAlpha ? get(cm.m_a) : 1.0f;
	auto d8 = [&](float f)
	{ return static_cast<int>(applyDisplay(f, m_state.m_disp) * 255 + 0.5f); };
	out.m_r = d8(lr);
	out.m_g = d8(lg);
	out.m_b = d8(lb);
	out.m_luma = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
	out.m_state = Sample::State::OK;
	return out;
}

Element Viewer::renderTiles(const ImageInfoPtr& info, int width)
{
	bool bySource = m_state.m_sources.size() > 1;
	int n = bySource ? static_cast<int>(m_state.m_sources.size())
	                 : (info ? static_cast<int>(info->m_layers.size()) : 0);
	if(n == 0)
	{
		return text("");
	}
	while(static_cast<int>(m_tileSlots.size()) < n)
	{
		m_tileSlots.push_back(newSlot(m_ctx));
	}
	if(static_cast<int>(m_tileSlots.size()) > n)
	{
		m_tileSlots.resize(n);
	}

	// Contact-sheet layout: every tile is sized to the image's aspect (so
	// neighbours nearly touch), filled left to right then down, and the
	// whole block is centred. No captions: the HUD names the selected one.
	const auto dims = Terminal::Size();
	const int areaW = std::max(1, width);
	const int areaH = std::max(1, dims.dimy - 2); // minus HUD + status
	const double pxX = m_viewSlot->pxPerCellX(), pxY = m_viewSlot->pxPerCellY();
	// Tiles frame the display window only. Every tile's frame size: one per
	// source (they may differ), or the same frame for all layers.
	std::vector<std::pair<double, double>> frames(n);
	for(int i = 0; i < n; ++i)
	{
		ImageInfoPtr ti =
		    bySource
		        ? m_ctx.m_svc.info(m_state.m_sources[i].frame(m_state.m_frame))
		        : info;
		const Box2i b = (ti ? ti : info)->displayWindow();
		frames[i] = {std::max(1, b.width()), std::max(1, b.height())};
	}
	// Pick the column count that shows the most image overall, then shrink
	// the shared tile box to the largest image actually drawn in it. It
	// depends on every image, not the selected one, so it stays put.
	int cols = 1;
	double bestArea = -1;
	for(int c = 1; c <= n; ++c)
	{
		const int r = (n + c - 1) / c;
		const double boxW = areaW * pxX / c, boxH = areaH * pxY / r;
		double area = 0;
		for(const auto& [w, h] : frames)
		{
			const double sc = std::min(boxW / w, boxH / h);
			area += sc * w * sc * h;
		}
		if(area > bestArea * 1.0001) // ties: fewer columns
		{
			bestArea = area;
			cols = c;
		}
	}
	const int rows = (n + cols - 1) / cols;
	const double boxW = areaW * pxX / cols, boxH = areaH * pxY / rows;
	double usedW = 1, usedH = 1;
	for(const auto& [w, h] : frames)
	{
		const double sc = std::min(boxW / w, boxH / h);
		usedW = std::max(usedW, sc * w);
		usedH = std::max(usedH, sc * h);
	}
	const int tileW =
	    std::clamp(static_cast<int>(usedW / pxX), 1, areaW / cols);
	const int tileH =
	    std::clamp(static_cast<int>(usedH / pxY), 1, areaH / rows);

	m_tileRefs.resize(n);
	Elements gridRows;
	for(int r = 0; r < rows; ++r)
	{
		Elements rowEls;
		for(int c = 0; c < cols && r * cols + c < n; ++c)
		{
			const int i = r * cols + c;
			auto& slot = m_tileSlots[i];
			slot->setMaxPixels(pixelCap());
			fs::path path;
			std::string layer = m_state.m_layerLabel;
			bool selected; // the tile the HUD and panes describe
			if(bySource)
			{
				path = m_state.m_sources[i].frame(m_state.m_frame);
				selected = i == m_state.m_current;
			}
			else
			{
				path = m_state.currentFramePath();
				layer = info->m_layers[i].label();
				selected =
				    i == std::max(0, info->findLayer(m_state.m_layerLabel));
			}
			m_tileRefs[i] = {path, layer};
			const ImageInfoPtr ti = m_ctx.m_svc.info(path);
			LayerImagePtr li =
			    ti ? m_ctx.m_svc.layerBestEffort(path,
			                                     layer,
			                                     reduceFor(*ti, *slot))
			       : nullptr;
			if(!li)
			{
				// Still decoding: keep the tile's last picture up, as the
				// single view does (see render()).
				li = slot->shown();
			}
			Element img = text("…") | dim | center;
			if(li)
			{
				ViewParams frameFit;
				frameFit.m_fitFrame = true;
				// A contact sheet shows the frame outline at most.
				DisplayParams disp = m_state.m_disp;
				if(disp.m_outlines == Outlines::FRAME_AND_DATA)
				{
					disp.m_outlines = Outlines::FRAME;
				}
				disp.m_selected = selected && n > 1 && m_tileSelection;
				const int src = bySource ? i : m_state.m_current;
				img = slot->element(
				    li,
				    frameFit,
				    disp,
				    overlayText({m_state.sourceAnnotations(m_ctx.m_ann, src)},
				                keyLookup(src, path, ti, layer)));
			}
			rowEls.push_back(img | size(WIDTH, EQUAL, tileW) |
			                 size(HEIGHT, EQUAL, tileH));
		}
		gridRows.push_back(hbox(std::move(rowEls)));
	}
	// Global lines once over the whole sheet (the union of the tile frames),
	// resolved against the selected tile.
	const int sel = bySource
	                    ? m_state.m_current
	                    : std::max(0, info->findLayer(m_state.m_layerLabel));
	const TileRef& selRef = m_tileRefs[std::clamp(sel, 0, n - 1)];
	OverlayText sheet = overlayText({&m_ctx.m_ann.global()},
	                                keyLookup(m_state.m_current,
	                                          selRef.m_path,
	                                          m_ctx.m_svc.info(selRef.m_path),
	                                          selRef.m_layer));
	Element grid = vbox(std::move(gridRows));
	if(!overlayEmpty(sheet))
	{
		grid = drawAfter(
		    grid,
		    [this, sheet = std::move(sheet)](Screen& screen)
		    {
			    std::optional<ImageSlot::CellRect> u;
			    Box clip{};
			    for(const auto& t : m_tileSlots)
			    {
				    auto f = t->frameCells();
				    if(!f)
				    {
					    continue;
				    }
				    const Box b = t->box();
				    if(!u)
				    {
					    u = f;
					    clip = b;
					    continue;
				    }
				    const int x1 = std::max(u->m_x + u->m_w, f->m_x + f->m_w);
				    const int y1 = std::max(u->m_y + u->m_h, f->m_y + f->m_h);
				    u->m_x = std::min(u->m_x, f->m_x);
				    u->m_y = std::min(u->m_y, f->m_y);
				    u->m_w = x1 - u->m_x;
				    u->m_h = y1 - u->m_y;
				    clip = Box::Union(clip, b);
			    }
			    if(!u)
			    {
				    return;
			    }
			    paintOverlay(
			        screen,
			        clip,
			        layoutOverlay(u->m_x, u->m_y, u->m_w, u->m_h, sheet),
			        [this](int x,
			               int y) -> std::optional<std::array<uint8_t, 3>>
			        {
				        for(const auto& t : m_tileSlots)
				        {
					        if(auto c = t->cellColor(x, y))
					        {
						        return c;
					        }
				        }
				        return std::nullopt;
			        });
		    });
	}
	return grid | center;
}

Element Viewer::render()
{
	if(m_state.m_sources.empty())
	{
		return text("");
	}
	fs::path path = m_state.currentFramePath();
	ImageInfoPtr info = m_ctx.m_svc.info(path);
	if(info)
	{
		if(info->findLayer(m_state.m_layerLabel) < 0)
		{
			m_state.m_layerLabel = info->m_layers.front().label();
		}
		if(m_player.fps() <= 0 && info->m_fps)
		{
			m_player.setFps(*info->m_fps);
		}
	}

	Element main;
	if(!info)
	{
		auto err = m_ctx.m_svc.error(path);
		main = (err ? paragraph("error: " + *err) | color(Color::Red)
		            : text("loading…") | dim) |
		       center | flex;
	}
	else if(m_tile)
	{
		main =
		    renderTiles(info,
		                Terminal::Size().dimx - coveredLeft() - coveredRight());
	}
	else
	{
		if(m_fitBesidePanel)
		{
			fitView(info); // follow terminal resizes and pane changes
		}
		m_viewSlot->setMaxPixels(pixelCap());
		int reduce = reduceFor(*info, *m_viewSlot);
		LayerImagePtr img =
		    m_ctx.m_svc.layerBestEffort(path, m_state.m_layerLabel, reduce);
		if(!img)
		{
			// Still decoding: keep the last picture up. Blanking it would also
			// make kitty redraw every placeholder cell once the new one is in,
			// a burst tmux may drop output over (and the picture with it).
			img = m_viewSlot->shown();
		}
		if(!m_player.playing() && m_state.frameCount() > 1)
		{
			// Smooth stepping with '.'.
			const int n = m_state.frameCount();
			for(int i = 1; i <= 2 && i < n; ++i)
			{
				prefetchFrame((m_state.m_frame + i) % n);
			}
		}
		main =
		    img ? m_viewSlot->element(
		              img,
		              m_state.m_view,
		              m_state.m_disp,
		              overlayText({&m_ctx.m_ann.global(),
		                           m_state.sourceAnnotations(m_ctx.m_ann,
		                                                     m_state.m_current)},
		                          keyLookup(m_state.m_current,
		                                    path,
		                                    info,
		                                    m_state.m_layerLabel)))
		        : text("decoding…") | dim | center;
		main = main | flex;
	}

	// Side panels are drawn *over* the single image (dbox + clear_under) so
	// opening or closing them never refits or moves the picture; tiles are
	// laid out beside them instead, so none is hidden.
	Element body = main | flex;
	Element leftCol, rightCol;
	if(leftPanelOpen())
	{
		// Layers on top, files pinned to the bottom left.
		Elements panes;
		if(m_layers.isOpen())
		{
			panes.push_back(m_layers.render(info));
		}
		panes.push_back(filler());
		if(m_files.isOpen())
		{
			if(m_layers.isOpen())
			{
				panes.push_back(separator());
			}
			panes.push_back(m_files.render());
		}
		const bool leftFocused =
		    m_state.m_focus == Focus::LAYERS || m_state.m_focus == Focus::FILES;
		leftCol =
		    hbox(
		        {vbox(std::move(panes)) | size(WIDTH, EQUAL, leftPanelWidth()),
		         separator() | (leftFocused ? color(Color::Green) : nothing)}) |
		    reflect(m_leftBox);
	}
	if(rightPanelOpen())
	{
		Elements side;
		auto add = [&](Element el)
		{
			if(!side.empty())
			{
				side.push_back(separator());
			}
			side.push_back(std::move(el));
		};
		if(m_inspector.isOpen())
		{
			add(m_inspector.render(info ? sampleAt(m_mouseX, m_mouseY)
			                            : Sample{}));
		}
		if(m_meta.isOpen())
		{
			add(m_meta.render(info) | flex);
		}
		const bool rightFocused =
		    m_state.m_focus == Focus::META || m_state.m_focus == Focus::INSPECT;
		rightCol =
		    hbox({separator() | (rightFocused ? color(Color::Green) : nothing),
		          vbox(std::move(side)) |
		              size(WIDTH, EQUAL, sidePanelWidth())}) |
		    reflect(m_sideBox);
	}
	if(m_tile)
	{
		Elements row;
		if(leftCol)
		{
			row.push_back(leftCol);
		}
		row.push_back(body);
		if(rightCol)
		{
			row.push_back(rightCol);
		}
		body = hbox(std::move(row));
	}
	else if(leftCol || rightCol)
	{
		Elements over;
		if(leftCol)
		{
			over.push_back(leftCol | clear_under);
		}
		over.push_back(filler());
		if(rightCol)
		{
			over.push_back(rightCol | clear_under);
		}
		body = dbox({body, hbox(std::move(over))});
	}
	return vbox(
	    {renderHud(info),
	     body | flex,
	     typing() ? m_files.annotations().renderInput() : renderStatus(info)});
}

Element Viewer::renderStatus(const ImageInfoPtr& info)
{
	std::string right;
	if(m_inspector.isOpen())
	{
		right = fmt::format("Esc close inspector  ctrl+click pick  ? help  {}",
		                    m_tile ? "q untile" : "q exit viewer");
	}
	else if(m_tile)
	{
		right = "click select  Enter open  ? help  q untile";
	}
	else
	{
		right = "2 meta  3 files  4 inspect  5 layers  t tile  ? help  q exit "
		        "viewer";
	}
	if(m_state.m_sources.size() > 1 && !m_inspector.isOpen())
	{
		right = "n/N next  " + right;
	}
	// Playback state lives in the bottom bar: frame / fps left, cache right.
	Elements playback;
	std::string cache;
	if(m_state.frameCount() > 1)
	{
		// Frame number from the file name, then position in the sequence.
		const std::string label =
		    m_state.m_sources[m_state.m_current].frameLabel(m_state.m_frame);
		playback.push_back(text(" frame ") | dim);
		playback.push_back(text(label.empty() ? "-" : label) | bold |
		                   color(Color::Magenta));
		playback.push_back(text(fmt::format(" ({}/{}) ",
		                                    m_state.m_frame + 1,
		                                    m_state.frameCount())) |
		                   color(Color::Magenta));
		const double target =
		    m_player.targetFps(info ? info->m_fps : std::nullopt);
		playback.push_back(text(m_player.playing()
		                            ? fmt::format(" ▶ {:.1f}/{:.3g}fps ",
		                                          m_player.measuredFps(),
		                                          target)
		                            : fmt::format(" ⏸ {:.3g}fps ", target)) |
		                   (m_player.playing() ? color(Color::Green) : dim));
		cache = fmt::format("cache {}/{}",
		                    humanSize(m_ctx.m_svc.bytesUsed()),
		                    humanSize(m_ctx.m_svc.budget()));
	}
	return m_ctx.statusLine(right, std::move(playback), cache);
}

} // namespace rv
