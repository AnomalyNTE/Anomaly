#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// The on-screen keyframe timeline's geometry: where the track, keys, playhead
// and buttons are on the AHUD canvas, the zoomed view window onto the
// timeline, and what a cursor position hits. Pure layout math; the plugin
// draws it and turns the hits into keyframe requests.
namespace better_pose::timeline {

inline constexpr int kNone = -1;
enum class Hit : std::uint8_t { None, Track, Playhead, Key, Play, Stop, Record, Delete, Fit };

struct Rect {
  float left{};
  float top{};
  float right{};
  float bottom{};
};

struct KeyMark {
  std::uint32_t frame{};
  bool pose{};
  bool expression{};
  bool camera{};
  float x{};
  bool visible{true};  // inside the view window (MakeLayout sets it)
};

// The part of the timeline the track shows, in frames: zoomed in, it is a
// window onto [0, length]; zoomed all the way out, it is all of it.
struct View {
  double start{};
  double end{};
};

inline constexpr double kMinimumViewFrames = 30.0;  // one second, the closest zoom

// The view clamped to [0, length], at least kMinimumViewFrames wide (or the
// whole length when that is shorter), keeping its width when it slides.
inline View ClampView(View view, const std::uint32_t length) noexcept {
  const double total = (std::max)(1.0, static_cast<double>(length));
  double width = view.end - view.start;
  if (!(width > 0.0) || !std::isfinite(width) || width >= total)
    return {0.0, total};
  width = (std::max)(width, (std::min)(kMinimumViewFrames, total));
  double start = std::isfinite(view.start) ? view.start : 0.0;
  start = std::clamp(start, 0.0, total - width);
  return {start, start + width};
}

// Windows reports the wheel in units of 120 per notch (WHEEL_DELTA); a
// high-resolution wheel or touchpad sends fractions of it.
inline constexpr double kWheelUnitsPerNotch = 120.0;

// Mouse wheel over the track, in raw wheel units (`wheel` > 0 zooms in). The
// frame under the cursor (`anchor`) stays under it, so the zoom closes in on
// where the user points. Each notch scales the width by 0.8.
inline View ZoomView(const View &view, const std::uint32_t length, const double anchor,
                     const int wheel) noexcept {
  const View current = ClampView(view, length);
  const double width = current.end - current.start;
  const double scale = std::pow(0.8, static_cast<double>(wheel) / kWheelUnitsPerNotch);
  const double next_width = width * scale;
  const double t = width > 0.0 ? std::clamp((anchor - current.start) / width, 0.0, 1.0) : 0.5;
  const double start = anchor - t * next_width;
  return ClampView({start, start + next_width}, length);
}

// Middle drag: the track slides by `pixels`, so the frame under the cursor
// follows it (drag right = earlier frames come into view).
inline View PanView(const View &view, const std::uint32_t length, const float track_width,
                    const float pixels) noexcept {
  const View current = ClampView(view, length);
  if (!(track_width > 1.0F))
    return current;
  const double frames = static_cast<double>(pixels) / track_width * (current.end - current.start);
  return ClampView({current.start - frames, current.end - frames}, length);
}

// While playing, a playhead that runs off either edge turns the page so it is
// back in view (its new page starts at the playhead); in view, nothing moves.
inline View FollowPlayhead(const View &view, const std::uint32_t length,
                           const double frame) noexcept {
  const View current = ClampView(view, length);
  if (frame >= current.start && frame <= current.end)
    return current;
  const double width = current.end - current.start;
  return ClampView({frame, frame + width}, length);
}

struct Layout {
  Rect panel{};
  Rect track{};
  Rect play_button{};
  Rect stop_button{};
  Rect record_button{};
  Rect delete_button{};
  Rect fit_button{};  // lower left: zoom back out to the whole timeline
  float frame_start{};  // the view window, in frames
  float frame_end{};
  float playhead_x{};
  bool playhead_visible{true};
  std::vector<KeyMark> keys;
};

inline bool Contains(const Rect &r, const float x, const float y) noexcept {
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline float FrameToX(const Layout &layout, const double frame) noexcept {
  const double span = (std::max)(1.0, static_cast<double>(layout.frame_end - layout.frame_start));
  const double t = (frame - layout.frame_start) / span;
  return layout.track.left + static_cast<float>(std::clamp(t, 0.0, 1.0) *
                                                (layout.track.right - layout.track.left));
}

inline double XToFrame(const Layout &layout, const float x) noexcept {
  const float width = layout.track.right - layout.track.left;
  if (!(width > 1.0F)) return layout.frame_start;
  const double t = std::clamp(static_cast<double>(x - layout.track.left) / width, 0.0, 1.0);
  return layout.frame_start + t * (layout.frame_end - layout.frame_start);
}

inline Layout MakeLayout(const float width, const float height, const std::uint32_t length,
                         const double frame, const std::vector<KeyMark> &marks,
                         const View &requested_view = {}) {
  Layout out;
  constexpr float kBottomSafeArea = 78.0F;
  const float panel_height = 106.0F;
  out.panel = {8.0F, (std::max)(height - panel_height - kBottomSafeArea, 0.0F), width - 8.0F,
               height - kBottomSafeArea};
  out.track = {out.panel.left + 122.0F, out.panel.top + 34.0F,
               (std::max)(out.panel.left + 124.0F, out.panel.right - 12.0F),
               out.panel.top + 68.0F};
  out.play_button = {out.panel.left + 10.0F, out.panel.top + 10.0F,
                     out.panel.left + 52.0F, out.panel.top + 30.0F};
  out.stop_button = {out.panel.left + 58.0F, out.panel.top + 10.0F,
                     out.panel.left + 100.0F, out.panel.top + 30.0F};
  out.delete_button = {out.panel.right - 210.0F, out.panel.top + 10.0F,
                       out.panel.right - 115.0F, out.panel.top + 30.0F};
  out.record_button = {out.panel.right - 105.0F, out.panel.top + 10.0F,
                       out.panel.right - 10.0F, out.panel.top + 30.0F};
  // Under the play buttons, left of the track: the corner nothing else uses.
  out.fit_button = {out.panel.left + 10.0F, out.panel.bottom - 30.0F,
                    out.panel.left + 100.0F, out.panel.bottom - 10.0F};
  const View view = ClampView(requested_view, length);
  out.frame_start = static_cast<float>(view.start);
  out.frame_end = static_cast<float>(view.end);
  out.playhead_x = FrameToX(out, frame);
  out.playhead_visible = frame >= view.start && frame <= view.end;
  out.keys = marks;
  for (auto &key : out.keys) {
    key.x = FrameToX(out, key.frame);
    key.visible = key.frame >= view.start && key.frame <= view.end;
  }
  return out;
}

inline int HitKey(const Layout &layout, const float x, const float y, const float radius = 9.0F) noexcept {
  for (std::size_t i{}; i != layout.keys.size(); ++i) {
    if (!layout.keys[i].visible)
      continue;  // off the zoomed view: drawn nowhere, so not grabbable
    const float dx = x - layout.keys[i].x;
    const float dy = y - (layout.track.top + layout.track.bottom) * 0.5F;
    if (dx * dx + dy * dy <= radius * radius) return static_cast<int>(i);
  }
  return kNone;
}

inline Hit HitTest(const Layout &layout, const float x, const float y, int &key_index) noexcept {
  key_index = HitKey(layout, x, y);
  if (key_index != kNone) return Hit::Key;
  if (Contains(layout.play_button, x, y)) return Hit::Play;
  if (Contains(layout.stop_button, x, y)) return Hit::Stop;
  if (Contains(layout.record_button, x, y)) return Hit::Record;
  if (Contains(layout.delete_button, x, y)) return Hit::Delete;
  if (Contains(layout.fit_button, x, y)) return Hit::Fit;
  if (Contains(layout.track, x, y)) {
    if (layout.playhead_visible && std::abs(x - layout.playhead_x) <= 7.0F) return Hit::Playhead;
    return Hit::Track;
  }
  return Hit::None;
}

}  // namespace better_pose::timeline
