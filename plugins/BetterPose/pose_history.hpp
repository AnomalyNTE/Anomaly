#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Undo/redo for the manual joint pose, by snapshots.
//
// Every edit path -- sliders, reset buttons, on-screen drags and IK, pose
// import -- ends up in the same two values: the per-bone angle table and the
// body offset. So instead of teaching each path to record itself, the history
// watches those values: while they keep changing (a slider being dragged, a
// joint being pulled) nothing is recorded, and once they have been still for
// a moment the state before the change becomes one undo step. One drag is one
// step however many frames it took.
namespace better_pose::history {

struct PoseState {
  std::vector<std::array<double, 3>> angles;
  std::array<double, 3> root_offset{};

  // Trailing zero angles do not change the pose; the table grows on demand.
  bool SameAs(const PoseState &other) const noexcept {
    constexpr double kEpsilon = 1e-9;
    for (std::size_t axis{}; axis != 3; ++axis)
      if (std::abs(root_offset[axis] - other.root_offset[axis]) > kEpsilon)
        return false;
    const std::size_t count = (std::max)(angles.size(), other.angles.size());
    for (std::size_t bone{}; bone != count; ++bone) {
      const std::array<double, 3> zero{};
      const auto &a = bone < angles.size() ? angles[bone] : zero;
      const auto &b = bone < other.angles.size() ? other.angles[bone] : zero;
      for (std::size_t axis{}; axis != 3; ++axis)
        if (std::abs(a[axis] - b[axis]) > kEpsilon)
          return false;
    }
    return true;
  }
};

class PoseHistory {
public:
  static constexpr std::size_t kMaximumSteps = 64;
  static constexpr std::uint64_t kSettleMilliseconds = 250;

  // Forget everything and treat `state` as the starting point (plugin load,
  // character switch: steps from another pose must not be undone into).
  void Reset(const PoseState &state) {
    undo_.clear();
    redo_.clear();
    committed_ = state;
    pending_ = false;
    initialized_ = true;
  }

  // Called every frame with the live state. `editing` is true while an input
  // is still held (mouse button down); a step is only closed once it is
  // released and the state has stopped changing for kSettleMilliseconds.
  // Returns true when a new undo step was recorded.
  bool Observe(const PoseState &state, const bool editing, const std::uint64_t now) {
    if (!initialized_) {
      Reset(state);
      return false;
    }
    if (!pending_) {
      if (state.SameAs(committed_))
        return false;
      pending_ = true;
      last_ = state;
      changed_at_ = now;
      return false;
    }
    if (!state.SameAs(last_)) {
      last_ = state;
      changed_at_ = now;
      return false;
    }
    if (editing || now - changed_at_ < kSettleMilliseconds)
      return false;
    pending_ = false;
    if (state.SameAs(committed_))
      return false;  // changed and changed back: nothing to undo
    undo_.push_back(committed_);
    if (undo_.size() > kMaximumSteps)
      undo_.pop_front();
    redo_.clear();
    committed_ = state;
    return true;
  }

  // Undo/redo return the state to apply. An edit still settling is closed
  // first, so undo goes back to before it rather than one step further.
  bool Undo(const PoseState &live, PoseState &out) {
    Flush(live);
    if (undo_.empty())
      return false;
    redo_.push_back(committed_);
    committed_ = undo_.back();
    undo_.pop_back();
    out = committed_;
    return true;
  }

  bool Redo(const PoseState &live, PoseState &out) {
    Flush(live);
    if (redo_.empty())
      return false;
    undo_.push_back(committed_);
    committed_ = redo_.back();
    redo_.pop_back();
    out = committed_;
    return true;
  }

  std::size_t UndoCount() const noexcept { return undo_.size(); }
  std::size_t RedoCount() const noexcept { return redo_.size(); }

private:
  void Flush(const PoseState &live) {
    if (!initialized_) {
      Reset(live);
      return;
    }
    pending_ = false;
    if (live.SameAs(committed_))
      return;
    undo_.push_back(committed_);
    if (undo_.size() > kMaximumSteps)
      undo_.pop_front();
    redo_.clear();
    committed_ = live;
  }

  std::deque<PoseState> undo_;
  std::deque<PoseState> redo_;
  PoseState committed_;
  PoseState last_;
  std::uint64_t changed_at_{};
  bool pending_{};
  bool initialized_{};
};

}  // namespace better_pose::history
