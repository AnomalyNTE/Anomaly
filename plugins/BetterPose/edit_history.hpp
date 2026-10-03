#pragma once

#include "keyframe_track.hpp"
#include "pose_history.hpp"

// One undo history for every edit the user makes, the way MMD has it: the
// pose, the expression and the keyframe track are one snapshot, so Ctrl+Z
// takes back the last edit whichever of them it touched, and an edit that
// touched several (auto-key: dragging a bone also writes its key) is one step.
// The settle-then-record rule is History<>'s (pose_history.hpp), unchanged.
namespace better_pose::history {

struct EditState {
  PoseState pose;
  ExpressionState expression;
  keyframes::TrackState track;

  bool SameAs(const EditState &other) const noexcept {
    return pose.SameAs(other.pose) && expression.SameAs(other.expression) &&
           track.SameAs(other.track);
  }
};

using EditHistory = History<EditState>;

}  // namespace better_pose::history
