#include "../morph_catalog.hpp"
#include "../pose_history.hpp"
#include "../mmd_morph_map.hpp"

#include <cmath>
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

void MmdMapping() {
  namespace mm = better_pose::mmd_morph;
  // The two measured NTE characters: one has vowel shapes, one does not.
  const std::vector<std::string> with_vowels{"jawOpen", "jawOpen_a", "jawOpen_yi", "jawOpen_wu",
                                             "jawOpen_ei", "jawOpen_o", "biyan", "biyan_L",
                                             "biyan_R", "EL_Smile_01_CLO", "EB_UD_L", "EB_UD_R",
                                             "mouthFunnel", "mouthPucker"};
  const std::vector<std::string> without_vowels{"jawOpen", "mouthFunnel", "mouthPucker", "biyan",
                                                "EL_Smile_01_CLO"};
  const std::vector<std::string> mmd{"あ", "お", "ウィンク", "上", "まばたき", "ありえない"};

  const auto a = mm::Resolve(mmd, with_vowels);
  Check(a.size() == 6, "one entry per MMD morph");
  Check(a[0].drives.size() == 1 && with_vowels[a[0].drives[0].entry] == "jawOpen_a",
        "あ takes the vowel shape when the character has it");
  Check(with_vowels[a[1].drives[0].entry] == "jawOpen_o", "お takes jawOpen_o");
  Check(with_vowels[a[2].drives[0].entry] == "biyan_L", "ウィンク closes the left eye");
  Check(a[3].drives.size() == 2 && with_vowels[a[3].drives[0].entry] == "EB_UD_L" &&
            with_vowels[a[3].drives[1].entry] == "EB_UD_R",
        "上 raises both brows");
  Check(a[5].drives.empty(), "an unknown MMD morph maps to nothing");

  const auto b = mm::Resolve(mmd, without_vowels);
  Check(without_vowels[b[0].drives[0].entry] == "jawOpen", "あ falls back to jawOpen");
  Check(without_vowels[b[1].drives[0].entry] == "mouthFunnel", "お falls back to mouthFunnel");
  Check(b[2].drives.empty() && b[3].drives.empty(),
        "a morph the character lacks entirely is skipped, not guessed");
  Check(with_vowels[a[4].drives[0].entry] == "biyan" && a[4].drives[0].scale == 1.0F,
        "まばたき uses biyan at full strength");
  const std::vector<std::string> no_biyan{"jawOpen", "TD_EyesClo"};
  const auto c = mm::Resolve({"まばたき"}, no_biyan);
  Check(c[0].drives.size() == 1 && no_biyan[c[0].drives[0].entry] == "TD_EyesClo" &&
            c[0].drives[0].scale == 0.5F,
        "the TD_EyesClo fallback carries its own half strength");
  // い / え: the vowel shape is full strength, only the jawOpen stand-in is partial.
  const auto vowel = mm::Resolve({"い", "え"}, with_vowels);
  Check(with_vowels[vowel[0].drives[0].entry] == "jawOpen_yi" && vowel[0].drives[0].scale == 1.0F &&
            with_vowels[vowel[1].drives[0].entry] == "jawOpen_ei" &&
            vowel[1].drives[0].scale == 1.0F,
        "a dedicated vowel shape is driven at full strength");
  const auto stand_in = mm::Resolve({"い", "え"}, without_vowels);
  Check(without_vowels[stand_in[0].drives[0].entry] == "jawOpen" &&
            stand_in[0].drives[0].scale == 0.6F && stand_in[1].drives[0].scale == 0.7F,
        "the jawOpen stand-in keeps its partial strength");

  // Sampling: linear between keys, held at the ends.
  const std::vector<mm::Key> keys{{10, 0.0F}, {20, 1.0F}, {40, 0.5F}};
  Check(mm::Sample(keys, 0.0) == 0.0F && mm::Sample(keys, 50.0) == 0.5F, "held at the ends");
  Check(std::abs(mm::Sample(keys, 15.0) - 0.5F) < 1e-6F &&
            std::abs(mm::Sample(keys, 30.0) - 0.75F) < 1e-6F,
        "linear between keys");
  Check(mm::Sample({}, 5.0) == 0.0F, "no keys, no weight");

  // Combining: あ and ワ on the same NTE morph add up and clamp; untouched
  // morphs are not driven at all.
  const std::vector<std::string> both{"あ", "ワ", "まばたき"};
  const auto resolved = mm::Resolve(both, with_vowels);
  std::vector<float> weights;
  std::vector<std::uint8_t> touched;
  mm::Combine(resolved, {0.7F, 0.6F, 0.0F}, with_vowels.size(), weights, touched);
  Check(weights[1] == 1.0F && touched[1] == 1, "two MMD morphs on one target add and clamp");
  Check(touched[6] == 1 && weights[6] == 0.0F, "a keyed morph at zero is still driven (open eyes)");
  Check(touched[0] == 0 && touched[9] == 0, "morphs the motion never keys are left alone");
}
}  // namespace

int main() {
  MmdMapping();
  Groups();
  OrderIsGroupedAndStable();
  WeightsDriveOnlyTouchedMorphs();
  SaveAndLoad();
  ExpressionHistory();
  std::cout << "PASS mmd mapping, groups, grouped order, driven weights, save and load, "
               "expression history\n";
  return 0;
}
