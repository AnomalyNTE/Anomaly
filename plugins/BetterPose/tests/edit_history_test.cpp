#include "../edit_history.hpp"

#include <cstdlib>
#include <iostream>

namespace {
namespace bh = better_pose::history;
namespace kf = better_pose::keyframes;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bh::EditState State(const double pitch, const float smile, const std::uint32_t key_frame) {
  bh::EditState s;
  s.pose.angles.assign(3, {0.0, 0.0, 0.0});
  s.pose.angles[1][0] = pitch;
  s.expression.weights = {smile};
  s.expression.driven = {static_cast<std::uint8_t>(smile != 0.0F ? 1 : 0)};
  if (key_frame != 0) {
    kf::Key key;
    key.frame = key_frame;
    key.has_pose = true;
    key.bones.push_back({"spine", pitch, 0.0, 0.0});
    s.track.keys.push_back(key);
  }
  return s;
}

// One value held for `frames` frames, 16 ms apart, starting at `now`.
std::uint64_t Hold(bh::EditHistory &history, const bh::EditState &state, std::uint64_t now,
                   const int frames, const bool editing = false) {
  for (int frame{}; frame != frames; ++frame, now += 16)
    history.Observe(state, editing, now);
  return now;
}

void SameAsComparesAllThree() {
  const auto base = State(10, 0.5F, 30);
  Check(base.SameAs(State(10, 0.5F, 30)), "identical states");
  Check(!base.SameAs(State(11, 0.5F, 30)), "a pose difference counts");
  Check(!base.SameAs(State(10, 0.6F, 30)), "an expression difference counts");
  Check(!base.SameAs(State(10, 0.5F, 31)), "a track difference counts");
}

void EachKindOfEditIsAStep() {
  bh::EditHistory history;
  std::uint64_t now = 1000;
  now = Hold(history, State(0, 0, 0), now, 5);
  now = Hold(history, State(20, 0, 0), now, 30);       // pose
  now = Hold(history, State(20, 0.8F, 0), now, 30);    // expression
  now = Hold(history, State(20, 0.8F, 15), now, 30);   // track
  Check(history.UndoCount() == 3, "pose, expression and track edits are one history");

  bh::EditState out;
  Check(history.Undo(State(20, 0.8F, 15), out) && out.SameAs(State(20, 0.8F, 0)),
        "the newest edit (the key) goes first, whatever page it came from");
  Check(history.Undo(out, out) && out.SameAs(State(20, 0, 0)), "then the expression");
  Check(history.Undo(out, out) && out.SameAs(State(0, 0, 0)), "then the pose");
  Check(!history.Undo(out, out), "nothing left");
  Check(history.Redo(out, out) && out.SameAs(State(20, 0, 0)), "redo walks forward again");
}

void AutoKeyDragIsOneStep() {
  // Auto-key: while a bone is dragged the pose moves every frame, and when the
  // drag settles its key appears. Both belong to the same edit.
  bh::EditHistory history;
  std::uint64_t now = 1000;
  now = Hold(history, State(0, 0, 0), now, 5);
  for (int frame = 1; frame <= 20; ++frame, now += 16)
    history.Observe(State(frame * 1.0, 0, 0), true, now);   // mouse held
  now = Hold(history, State(20, 0, 0), now, 3);              // released...
  now = Hold(history, State(20, 0, 30), now, 30);            // ...and keyed
  Check(history.UndoCount() == 1, "the drag and its key are one step");
  bh::EditState out;
  Check(history.Undo(State(20, 0, 30), out) && out.SameAs(State(0, 0, 0)),
        "undo takes back both the pose and the key");
}

void UndoClosesAnEditStillSettling() {
  bh::EditHistory history;
  std::uint64_t now = 1000;
  now = Hold(history, State(0, 0, 0), now, 5);
  now = Hold(history, State(5, 0, 0), now, 2);  // not settled yet
  bh::EditState out;
  Check(history.Undo(State(5, 0, 0), out) && out.SameAs(State(0, 0, 0)),
        "undo goes back to before the unsettled edit, not one further");
  static_cast<void>(now);
}
}  // namespace

int main() {
  SameAsComparesAllThree();
  EachKindOfEditIsAStep();
  AutoKeyDragIsOneStep();
  UndoClosesAnEditStillSettling();
  std::cout << "PASS same-as over all three, one history for pose/expression/track, "
               "auto-key drag is one step, undo closes a settling edit\n";
  return 0;
}
