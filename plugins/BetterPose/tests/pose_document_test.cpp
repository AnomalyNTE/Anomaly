#include "../pose_document.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using namespace better_pose::pose_document;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

// The two measured skeletons at the indices pose1.json edits: the left arm
// shares its index on both, the right arm does not (costume bones sit in
// between on the second character).
std::vector<std::string> SkeletonA() {
  std::vector<std::string> names(200, "Bn_other");
  names[7] = "Bip001-L-UpperArm";
  names[8] = "Bip001-L-Forearm";
  names[9] = "Bip001-L-Hand";
  names[64] = "Bip001-R-UpperArm";
  names[65] = "Bip001-R-Forearm";
  names[66] = "Bip001-R-Hand";
  names[75] = "Bip001-R-Finger13";
  names[128] = "Bon_zuiba_R";
  return names;
}

std::vector<std::string> SkeletonB() {
  std::vector<std::string> names(200, "Bn_costume");
  names[7] = "Bip001-L-UpperArm";
  names[8] = "Bip001-L-Forearm";
  names[9] = "Bip001-L-Hand";
  names[75] = "Bip001-R-UpperArm";
  names[76] = "Bip001-R-Forearm";
  names[77] = "Bip001-R-Hand";
  names[128] = "Bn_r_xiuAa_001";
  return names;
}

void NamesWin() {
  const std::vector<SavedBone> saved{
      {7, "Bip001-L-UpperArm", 1, 2, 3},
      {64, "Bip001-R-UpperArm", 4, 5, 6},  // index 64 on A, 75 on B
      {66, "Bip001-R-Hand", 7, 8, 9},      // index 66 on A, 77 on B
  };
  const auto on_b = Place(saved, SkeletonB(), 8192);
  Check(on_b.by_name == 3 && on_b.by_index == 0 && on_b.missing.empty(), "all three by name");
  Check(on_b.placed[0].bone == 7, "the left arm keeps its index");
  Check(on_b.placed[1].bone == 75 && on_b.placed[1].pitch == 4,
        "the right upper arm lands on B's right upper arm, not on its index");
  Check(on_b.placed[2].bone == 77, "and so does the right hand");
}

void MissingNamesAreReportedNotGuessed() {
  const std::vector<SavedBone> saved{{128, "Bon_zuiba_R", 1, 0, 0},
                                     {7, "Bip001-L-UpperArm", 2, 0, 0}};
  const auto on_b = Place(saved, SkeletonB(), 8192);
  Check(on_b.placed.size() == 1 && on_b.placed[0].bone == 7,
        "a face bone B lacks is skipped, never written to B's bone 128 (a sleeve)");
  Check(on_b.missing.size() == 1 && on_b.missing[0] == "Bon_zuiba_R", "and reported by name");
}

void OldFilesFallBackToIndices() {
  // A file from before names were saved: only indices.
  const std::vector<SavedBone> saved{{7, "", 1, 0, 0}, {75, "", 2, 0, 0}};
  const auto on_b = Place(saved, SkeletonB(), 8192);
  Check(on_b.by_index == 2 && on_b.by_name == 0 && on_b.placed[1].bone == 75,
        "an old file still loads by index, as before");
  // Names not loaded yet on this character: indices too.
  const std::vector<SavedBone> named{{7, "Bip001-L-UpperArm", 1, 0, 0}};
  const auto unknown = Place(named, {}, 8192);
  Check(unknown.by_index == 1 && unknown.placed[0].bone == 7,
        "without the character's names the index is used");
  // Out of range indices are dropped.
  const auto clipped = Place({{9000, "", 1, 0, 0}}, {}, 8192);
  Check(clipped.placed.empty(), "an index past the limit is dropped");
}

void SameCharacterIsUnchanged() {
  const std::vector<SavedBone> saved{{64, "Bip001-R-UpperArm", 4, 5, 6},
                                     {75, "Bip001-R-Finger13", 1, 1, 1}};
  const auto on_a = Place(saved, SkeletonA(), 8192);
  Check(on_a.placed.size() == 2 && on_a.placed[0].bone == 64 && on_a.placed[1].bone == 75,
        "loading on the character it was saved from lands on the same indices");
}
}  // namespace

int main() {
  NamesWin();
  MissingNamesAreReportedNotGuessed();
  OldFilesFallBackToIndices();
  SameCharacterIsUnchanged();
  std::cout << "PASS names win, missing reported, old files by index, same character\n";
  return 0;
}
