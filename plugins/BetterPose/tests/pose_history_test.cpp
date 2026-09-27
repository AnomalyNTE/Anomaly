#include "../pose_history.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using better_pose::history::PoseHistory;
using better_pose::history::PoseState;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

PoseState Pose(const double bone1_pitch, const double root_x = 0.0) {
  PoseState state;
  state.angles.assign(4, {0.0, 0.0, 0.0});
  state.angles[1][0] = bone1_pitch;
  state.root_offset[0] = root_x;
  return state;
}

// Feed one value for `frames` frames at 16 ms each, starting at `now`.
std::uint64_t Hold(PoseHistory &history, const PoseState &state, std::uint64_t now,
                   const int frames, const bool editing = false) {
  for (int frame{}; frame != frames; ++frame, now += 16)
    history.Observe(state, editing, now);
  return now;
}

void DragIsOneStep() {
  PoseHistory history;
  std::uint64_t now = 1000;
  now = Hold(history, Pose(0), now, 5);
  // A slider dragged from 0 to 30 over 40 frames, mouse held.
  for (int frame = 1; frame <= 40; ++frame, now += 16)
    history.Observe(Pose(frame * 0.75), true, now);
  Check(history.UndoCount() == 0, "nothing is recorded while the drag is live");
  // Paused mid-drag with the button still down: still not a step.
  now = Hold(history, Pose(30), now, 40, true);
  Check(history.UndoCount() == 0, "a pause with the button held is not a step");
  // Released and settled.
  now = Hold(history, Pose(30), now, 20);
  Check(history.UndoCount() == 1, "the whole drag is one step");
  PoseState out;
  Check(history.Undo(Pose(30), out) && out.SameAs(Pose(0)), "undo returns to before the drag");
  Check(history.RedoCount() == 1, "undo makes a redo");
  Check(history.Redo(Pose(0), out) && out.SameAs(Pose(30)), "redo returns the drag");
}

void SeparateEditsAreSeparateSteps() {
  PoseHistory history;
  std::uint64_t now = 0;
  now = Hold(history, Pose(0), now, 3);
  now = Hold(history, Pose(10), now, 30);
  now = Hold(history, Pose(10, 5), now, 30);
  now = Hold(history, Pose(-20, 5), now, 30);
  Check(history.UndoCount() == 3, "three settled edits, three steps");
  PoseState out;
  PoseState live = Pose(-20, 5);
  Check(history.Undo(live, out) && out.SameAs(Pose(10, 5)), "undo 1");
  Check(history.Undo(out, out) && out.SameAs(Pose(10)), "undo 2");
  Check(history.Undo(out, out) && out.SameAs(Pose(0)), "undo 3");
  Check(!history.Undo(out, out), "nothing further to undo");
  // A new edit after undoing drops the redo branch.
  now = Hold(history, Pose(0), now, 30);
  now = Hold(history, Pose(45), now, 30);
  Check(history.RedoCount() == 0, "a new edit clears redo");
  Check(history.UndoCount() == 1, "the new edit is the only step");
}

void UndoWhileSettlingClosesTheEdit() {
  PoseHistory history;
  std::uint64_t now = 0;
  now = Hold(history, Pose(0), now, 3);
  now = Hold(history, Pose(15), now, 30);  // settled: one step
  now = Hold(history, Pose(25), now, 3);   // still settling
  PoseState out;
  Check(history.Undo(Pose(25), out) && out.SameAs(Pose(15)),
        "undo during an unsettled edit undoes that edit, not the one before");
  Check(history.UndoCount() == 1 && history.RedoCount() == 1, "counts after flushing undo");
  // The restored value must not be recorded as a new edit.
  now = Hold(history, out, now, 30);
  Check(history.UndoCount() == 1 && history.RedoCount() == 1, "applying undo is not an edit");
}

void ChangedBackIsNothing() {
  PoseHistory history;
  std::uint64_t now = 0;
  now = Hold(history, Pose(0), now, 3);
  now = Hold(history, Pose(5), now, 3, true);
  now = Hold(history, Pose(0), now, 30);
  Check(history.UndoCount() == 0, "a change that returns to the start records nothing");
}

void TrailingZerosAndReset() {
  PoseState short_state;
  short_state.angles.assign(2, {0.0, 0.0, 0.0});
  PoseState long_state = short_state;
  long_state.angles.resize(300, {0.0, 0.0, 0.0});
  Check(short_state.SameAs(long_state) && long_state.SameAs(short_state),
        "growing the angle table with zeros is not an edit");

  PoseHistory history;
  std::uint64_t now = 0;
  now = Hold(history, Pose(0), now, 3);
  now = Hold(history, Pose(10), now, 30);
  history.Reset(Pose(99));
  Check(history.UndoCount() == 0 && history.RedoCount() == 0, "reset forgets every step");
  PoseState out;
  Check(!history.Undo(Pose(99), out), "nothing to undo after a reset");
}

void Bounded() {
  PoseHistory history;
  std::uint64_t now = 0;
  now = Hold(history, Pose(0), now, 3);
  for (int step = 1; step <= 100; ++step)
    now = Hold(history, Pose(step), now, 30);
  Check(history.UndoCount() == PoseHistory::kMaximumSteps, "history is capped");
  PoseState out = Pose(100);
  while (history.Undo(out, out)) {
  }
  Check(out.SameAs(Pose(100 - static_cast<double>(PoseHistory::kMaximumSteps))),
        "the oldest steps are dropped first");
}
}  // namespace

int main() {
  DragIsOneStep();
  SeparateEditsAreSeparateSteps();
  UndoWhileSettlingClosesTheEdit();
  ChangedBackIsNothing();
  TrailingZerosAndReset();
  Bounded();
  std::cout << "PASS drag is one step, separate steps, redo branch, undo while settling, "
               "changed back, trailing zeros, reset, cap\n";
  return 0;
}
