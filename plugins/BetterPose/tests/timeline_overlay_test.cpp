#include "../timeline_overlay.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace tl = better_pose::timeline;
void Check(bool value, const char *message) { if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
bool Near(double a, double b) { return std::abs(a - b) < 1e-5; }

void LayoutAndMapping() {
  std::vector<tl::KeyMark> marks{{0, true, false, false, 0.0F}, {30, false, true, false, 0.0F}, {60, true, true, true, 0.0F}};
  const auto layout = tl::MakeLayout(1280.0F, 720.0F, 90, 30.0, marks);
  Check(layout.track.left < layout.track.right, "track has width");
  Check(Near(tl::FrameToX(layout, 0), layout.track.left), "frame zero maps left");
  Check(Near(tl::FrameToX(layout, 90), layout.track.right), "last frame maps right");
  Check(Near(tl::XToFrame(layout, layout.track.left + (layout.track.right - layout.track.left) * .5F), 45), "pixel maps frame");
  Check(Near(layout.keys[1].x, tl::FrameToX(layout, 30)), "key mark maps");
}

void HitTargets() {
  const auto layout = tl::MakeLayout(1000.0F, 600.0F, 100, 40, {});
  int index = -1;
  Check(tl::HitTest(layout, layout.play_button.left + 2, layout.play_button.top + 2, index) == tl::Hit::Play, "play button");
  Check(tl::HitTest(layout, layout.stop_button.left + 2, layout.stop_button.top + 2, index) == tl::Hit::Stop, "stop button");
  Check(tl::HitTest(layout, layout.record_button.left + 2, layout.record_button.top + 2, index) == tl::Hit::Record, "record button");
  Check(tl::HitTest(layout, layout.delete_button.left + 2, layout.delete_button.top + 2, index) == tl::Hit::Delete, "delete button");
  Check(tl::HitTest(layout, layout.export_button.left + 2, layout.export_button.top + 2, index) ==
            tl::Hit::Export,
        "export timeline button");
  Check(layout.panel.bottom <= 600.0F - 78.0F, "timeline leaves bottom safe area");
  Check(tl::HitTest(layout, layout.playhead_x, layout.track.top + 2, index) == tl::Hit::Playhead, "playhead");
  Check(tl::HitTest(layout, layout.track.left + 20, layout.track.top + 2, index) == tl::Hit::Track, "track");
}

void KeyHit() {
  std::vector<tl::KeyMark> marks{{10, true, false, false, 0.0F}, {50, false, true, false, 0.0F}};
  const auto layout = tl::MakeLayout(1000.0F, 600.0F, 100, 0, marks);
  int index = -1;
  Check(tl::HitTest(layout, layout.keys[0].x, (layout.track.top + layout.track.bottom) * .5F, index) == tl::Hit::Key && index == 0, "first key");
  Check(tl::HitKey(layout, layout.keys[1].x + 8, (layout.track.top + layout.track.bottom) * .5F) == 1, "key tolerance");
  Check(tl::HitKey(layout, layout.keys[1].x + 12, (layout.track.top + layout.track.bottom) * .5F) == tl::kNone, "outside key tolerance");
}

void ExportLanes() {
  std::vector<tl::KeyMark> marks{
      {10, true, false, false, 0.0F},
      {50, false, true, false, 0.0F},
      {70, true, true, true, 0.0F}};
  const auto layout =
      tl::MakeLayout(1000.0F, 600.0F, 100, 0, marks, {}, true);
  const auto lanes = tl::TrackLaneY(layout);
  Check(layout.export_tracks, "export mode selected");
  Check(lanes[0] < lanes[1] && lanes[1] < lanes[2],
        "export lanes are distinct");
  Check(tl::HitKey(layout, layout.keys[0].x, lanes[0]) == 0,
        "bone key sits on the bone lane");
  Check(tl::HitKey(layout, layout.keys[0].x, lanes[1]) == tl::kNone,
        "a pose-only key is not pickable on the morph lane");
  Check(tl::HitKey(layout, layout.keys[1].x, lanes[1]) == 1,
        "morph key sits on the morph lane");
  Check(tl::HitKey(layout, layout.keys[2].x, lanes[2]) == 2,
        "camera key sits on the camera lane");
}

void SmallAndZeroLength() {
  const auto layout = tl::MakeLayout(300.0F, 200.0F, 0, 0, {});
  Check(layout.track.right > layout.track.left, "small layout still has a track");
  Check(Near(tl::FrameToX(layout, 0), layout.track.left), "zero length starts at left");
  Check(Near(tl::FrameToX(layout, 1), layout.track.right), "zero length has a one-frame span");
}

void ViewClamp() {
  // An unset view (both zero) is the whole timeline.
  const auto all = tl::ClampView({}, 900);
  Check(Near(all.start, 0) && Near(all.end, 900), "default view shows everything");
  // Too narrow widens to one second; too wide shrinks to everything.
  const auto narrow = tl::ClampView({100, 105}, 900);
  Check(Near(narrow.end - narrow.start, tl::kMinimumViewFrames), "one second at the closest zoom");
  const auto wide = tl::ClampView({-50, 2000}, 900);
  Check(Near(wide.start, 0) && Near(wide.end, 900), "never wider than the timeline");
  // Sliding past an end keeps the width.
  const auto past = tl::ClampView({850, 950}, 900);
  Check(Near(past.start, 800) && Near(past.end, 900), "slid back inside, same width");
  // A timeline shorter than a second shows all of it.
  const auto tiny = tl::ClampView({0, 5}, 10);
  Check(Near(tiny.start, 0) && Near(tiny.end, 10), "short timeline shows all");
}

void ZoomKeepsTheCursorFrame() {
  // The wheel arrives the way Windows reports it: 120 units per notch.
  constexpr int kNotch = 120;
  const tl::View all{0, 900};
  // Zoom in at frame 300: it stays where it was along the track.
  const auto in = tl::ZoomView(all, 900, 300, kNotch);
  Check(Near(in.end - in.start, 720), "one notch (120 units) is 0.8x, not all the way");
  const double before = (300.0 - all.start) / (all.end - all.start);
  const double after = (300.0 - in.start) / (in.end - in.start);
  Check(Near(before, after), "the frame under the cursor stays under it");
  // A high-resolution wheel's half notch zooms half as far (in log scale).
  const auto half = tl::ZoomView(all, 900, 300, kNotch / 2);
  Check(Near(half.end - half.start, 900.0 * std::sqrt(0.8)), "a half notch is sqrt(0.8)x");
  // Zooming out from there all the way returns to everything.
  const auto out = tl::ZoomView(in, 900, 300, -10 * kNotch);
  Check(Near(out.start, 0) && Near(out.end, 900), "zoomed out to everything");
  // Many notches in stop at one second.
  const auto deep = tl::ZoomView(all, 900, 450, 40 * kNotch);
  Check(Near(deep.end - deep.start, tl::kMinimumViewFrames), "stops at one second");
  Check(deep.start <= 450 && deep.end >= 450, "still around the cursor");
}

void PanAndFollow() {
  const tl::View view{300, 400};
  // Dragging right by the whole track width brings the previous page in.
  const auto left = tl::PanView(view, 900, 500.0F, 500.0F);
  Check(Near(left.start, 200) && Near(left.end, 300), "drag right shows earlier frames");
  const auto right = tl::PanView(view, 900, 500.0F, -250.0F);
  Check(Near(right.start, 350) && Near(right.end, 450), "drag left shows later frames");
  const auto edge = tl::PanView(view, 900, 500.0F, 5000.0F);
  Check(Near(edge.start, 0) && Near(edge.end, 100), "panning stops at the start");
  // Following: in view nothing moves; off the right edge the page turns.
  const auto stay = tl::FollowPlayhead(view, 900, 350);
  Check(Near(stay.start, 300) && Near(stay.end, 400), "in view: still");
  const auto page = tl::FollowPlayhead(view, 900, 401);
  Check(Near(page.start, 401) && Near(page.end, 501), "off the edge: next page from the playhead");
  const auto wrap = tl::FollowPlayhead(view, 900, 10);
  Check(Near(wrap.start, 10) && Near(wrap.end, 110), "a looped playhead brings the view back");
}

void ZoomedLayout() {
  std::vector<tl::KeyMark> marks{{100, true, false, false, 0.0F}, {500, true, false, false, 0.0F}};
  const auto layout = tl::MakeLayout(1280.0F, 720.0F, 900, 600.0, marks, {80, 200});
  Check(Near(layout.frame_start, 80) && Near(layout.frame_end, 200), "the layout uses the view");
  Check(layout.keys[0].visible && !layout.keys[1].visible, "keys outside the view are hidden");
  Check(!layout.playhead_visible, "a playhead outside the view is hidden");
  const float key_y = (layout.track.top + layout.track.bottom) * 0.5F;
  Check(tl::HitKey(layout, layout.keys[1].x, key_y) == tl::kNone,
        "a hidden key cannot be grabbed at the edge it is clamped to");
  int index = -1;
  Check(tl::HitTest(layout, layout.track.right - 1.0F, layout.track.top + 2.0F, index) ==
            tl::Hit::Track,
        "a hidden playhead is not grabbed at the edge either");
  Check(Near(tl::XToFrame(layout, layout.track.left), 80) &&
            Near(tl::XToFrame(layout, layout.track.right), 200),
        "pixels map into the zoomed window");
}

void FitButton() {
  const auto layout = tl::MakeLayout(1280.0F, 720.0F, 900, 0.0, {}, {100, 200});
  int index = -1;
  Check(tl::HitTest(layout, layout.fit_button.left + 2, layout.fit_button.top + 2, index) ==
            tl::Hit::Fit,
        "the fit button is hit");
  // Lower left: under the play buttons, left of the track, inside the panel.
  Check(layout.fit_button.top > layout.play_button.bottom, "below the play buttons");
  Check(layout.fit_button.right < layout.track.left, "left of the track");
  Check(layout.export_button.right <= layout.track.left,
        "export switch stays in the left gutter");
  Check(layout.fit_button.bottom <= layout.panel.bottom &&
            layout.fit_button.left >= layout.panel.left,
        "inside the panel");
}

void PlaybackShortcut() {
  using tl::SpaceAction;
  Check(tl::ResolveSpaceAction(true, true, false, false, false, true, false) ==
            SpaceAction::ToggleMotion,
        "space does not toggle a loaded MMD motion");
  Check(tl::ResolveSpaceAction(true, true, false, false, false, true, true) ==
            SpaceAction::ToggleMotion,
        "loaded MMD motion does not take precedence over keyframes");
  Check(tl::ResolveSpaceAction(true, true, false, false, false, false, true) ==
            SpaceAction::ToggleKeyframes,
        "space does not toggle an available keyframe timeline");
  Check(tl::ResolveSpaceAction(true, true, false, false, false, false, false) ==
            SpaceAction::None,
        "space toggled an empty keyframe timeline");
  Check(tl::ResolveSpaceAction(true, true, false, true, false, true, false) ==
                SpaceAction::None &&
            tl::ResolveSpaceAction(true, true, true, false, false, true, false) ==
                SpaceAction::None &&
            tl::ResolveSpaceAction(true, true, false, false, true, true, false) ==
                SpaceAction::None,
        "space shortcut ignored typing or key-repeat guards");
}
}  // namespace

int main() {
  LayoutAndMapping(); HitTargets(); KeyHit(); ExportLanes(); SmallAndZeroLength();
  ViewClamp(); ZoomKeepsTheCursorFrame(); PanAndFollow(); ZoomedLayout();
  FitButton(); PlaybackShortcut();
  std::cout << "PASS timeline layout, mapping, hit targets, key hit, zero length, view clamp, "
               "export lanes and switch, zoom around cursor (120 units/notch), "
               "pan and follow, zoomed layout, fit button, playback shortcut\n";
}
