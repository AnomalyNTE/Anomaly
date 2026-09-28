#include "../morph_catalog.hpp"
#include "../pose_history.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using namespace better_pose::morph;

void Check(const bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

// Names as they appear on the live NTE body mesh.
void Groups() {
  Check(GroupOf("look_U") == Group::Gaze && GroupOf("look_LD") == Group::Gaze, "gaze");
  Check(GroupOf("EL_Happy_L_CLO") == Group::Eyes && GroupOf("eye_SF") == Group::Eyes &&
            GroupOf("biyan_R") == Group::Eyes && GroupOf("TD_EyesClo") == Group::Eyes,
        "eyes");
  Check(GroupOf("EB_Angry_01") == Group::Brows && GroupOf("EB_UD_L") == Group::Brows, "brows");
  Check(GroupOf("jawOpen") == Group::Mouth && GroupOf("jawOpen_Sad_01_OP") == Group::Mouth &&
            GroupOf("mouthPucker") == Group::Mouth,
        "mouth");
  Check(GroupOf("TD_Imagination") == Group::Other && GroupOf("") == Group::Other, "other");
}

void OrderIsGroupedAndStable() {
  Catalog catalog;
  std::vector<Entry> list;
  for (const char *name : {"jawOpen", "look_U", "EL_Happy_L_CLO", "EB_UD_L", "mouthPucker",
                           "look_R", "TD_Imagination", "EL_Sad_01_OP"}) {
    Entry entry;
    entry.name = name;
    list.push_back(entry);
  }
  catalog.Build(0x1234, list);
  Check(catalog.asset == 0x1234 && catalog.entries.size() == 8 && catalog.order.size() == 8,
        "catalogue built");
  std::vector<std::string> shown;
  for (const auto index : catalog.order)
    shown.push_back(catalog.entries[index].name);
  const std::vector<std::string> expected{"EL_Happy_L_CLO", "EL_Sad_01_OP", "look_U", "look_R",
                                          "EB_UD_L",        "jawOpen",      "mouthPucker",
                                          "TD_Imagination"};
  Check(shown == expected, "grouped eyes, gaze, brows, mouth, other; authored order inside");
  Check(catalog.CountIn(Group::Eyes) == 2 && catalog.CountIn(Group::Mouth) == 2 &&
            catalog.CountIn(Group::Other) == 1,
        "group counts");
}

void WeightsDriveOnlyTouchedMorphs() {
  Weights weights;
  weights.Resize(4);
  Check(weights.DrivenCount() == 0, "nothing is driven until touched");
  weights.Set(1, 0.6F);
  weights.Set(3, 1.7F);
  weights.Set(9, 1.0F);  // out of range: ignored
  Check(weights.DrivenCount() == 2, "touched morphs are driven");
  Check(weights.value[1] == 0.6F && weights.value[3] == 1.0F, "weights clamp to 0..1");
  weights.Set(1, -0.5F);
  Check(weights.value[1] == 0.0F && weights.driven[1] == 1,
        "a morph set to zero is still held at zero");
  weights.Release(1);
  Check(weights.driven[1] == 0 && weights.value[1] == 0.0F && weights.DrivenCount() == 1,
        "release hands the morph back");
  weights.Resize(2);
  Check(weights.DrivenCount() == 0 && weights.value.size() == 2, "a new mesh starts clean");
}

Catalog MakeCatalog(const std::vector<const char *> &names) {
  std::vector<Entry> list;
  for (const char *name : names) {
    Entry entry;
    entry.name = name;
    list.push_back(entry);
  }
  Catalog catalog;
  catalog.Build(1, list);
  return catalog;
}

void SaveAndLoad() {
  const auto catalog = MakeCatalog({"jawOpen", "look_U", "EL_Happy_L_CLO", "mouthPucker"});
  Weights weights;
  weights.Resize(4);
  weights.Set(0, 0.8F);
  weights.Set(2, 1.0F);
  const auto saved = CollectDriven(catalog, weights);
  Check(saved.size() == 2 && saved[0].name == "jawOpen" && saved[0].weight == 0.8F &&
            saved[1].name == "EL_Happy_L_CLO",
        "only driven morphs are saved, by name");

  // Another character with a different order and one morph missing.
  const auto other = MakeCatalog({"EL_Happy_L_CLO", "mouthPucker", "look_U"});
  Weights loaded;
  loaded.Resize(3);
  loaded.Set(1, 0.3F);  // something set before the import
  std::vector<std::string> missing;
  const auto matched = ApplySaved(other, saved, loaded, missing);
  Check(matched == 1 && missing.size() == 1 && missing[0] == "jawOpen",
        "names are matched, the missing one is reported");
  Check(loaded.driven[0] == 1 && loaded.value[0] == 1.0F, "a matched morph lands by name");
  Check(loaded.driven[1] == 0 && loaded.value[1] == 0.0F,
        "loading replaces the expression instead of mixing into it");
}
void ExpressionHistory() {
  using better_pose::history::ExpressionState;
  better_pose::history::ExpressionHistory history;
  ExpressionState rest{{0.0F, 0.0F}, {0, 0}};
  history.Reset(rest);
  // Take morph 0 over at 0.5: one step once it settles.
  ExpressionState edited{{0.5F, 0.0F}, {1, 0}};
  history.Observe(edited, false, 1000);
  Check(history.Observe(edited, false, 1300), "a settled expression edit is one step");
  ExpressionState out;
  Check(history.Undo(edited, out) && out.driven[0] == 0, "undo hands the morph back");
  Check(history.Redo(rest, out) && out.driven[0] == 1 && out.weights[0] == 0.5F,
        "redo takes it over again");
  // An undriven weight is the game's: it moving is not an edit.
  ExpressionState game_moved{{0.5F, 0.7F}, {1, 0}};
  history.Observe(game_moved, false, 2000);
  Check(!history.Observe(game_moved, false, 2400), "a weight the game owns is not recorded");
}
}  // namespace

int main() {
  Groups();
  OrderIsGroupedAndStable();
  WeightsDriveOnlyTouchedMorphs();
  SaveAndLoad();
  ExpressionHistory();
  std::cout << "PASS groups, grouped order, driven weights, save and load, expression history\n";
  return 0;
}
