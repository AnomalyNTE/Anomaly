#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// MMD morph names -> NTE morph targets, for playing a VMD's facial keys.
//
// MMD models share a de-facto set of morph names (the ones below cover every
// non-zero morph in 27 real motions); NTE characters use their own English
// names, and different characters carry different sets (one has jawOpen_a ...
// jawOpen_o vowel shapes, another only per-emotion jawOpen_*). So each MMD
// morph lists *targets*, and each target lists *candidates* tried in order:
// the first candidate the character has is driven. A target is a weight
// multiplier on the MMD value, so one MMD morph can drive several NTE morphs
// (上 raises both brows) or a partial one.
//
// Directions were checked in game: biyan_L closes the character's own left
// eye (MMD ウィンク is the model's left eye too), EB_UD_* at 1 raises a brow.
namespace better_pose::mmd_morph {

// One candidate NTE morph. Its own scale multiplies the target's: fallbacks
// are different shapes, so one can need a different strength than the first
// choice (TD_EyesClo closes further than biyan and looks right at half).
struct Candidate {
  std::string_view name;
  float scale = 1.0F;
  Candidate(const char *n) : name(n) {}  // NOLINT: implicit, keeps the table terse
  Candidate(const std::string_view n, const float s = 1.0F) : name(n), scale(s) {}
};

struct Target {
  std::vector<Candidate> candidates;  // first one present wins
  float scale = 1.0F;
};

struct Rule {
  std::string_view mmd;  // UTF-8
  std::vector<Target> targets;
};

// Builders keep the table readable: T(scale, candidates...) is one target,
// C(name, scale) a candidate with its own strength, R(mmd, targets...) one rule.
inline Candidate C(const std::string_view name, const float scale) { return {name, scale}; }
inline Target T(const float scale, std::initializer_list<Candidate> candidates) {
  return Target{std::vector<Candidate>(candidates), scale};
}
inline Target T(std::initializer_list<Candidate> candidates) { return T(1.0F, candidates); }
inline Rule R(const std::string_view mmd, std::initializer_list<Target> targets) {
  return Rule{mmd, std::vector<Target>(targets)};
}

inline const std::vector<Rule> &Rules() {
  static const std::vector<Rule> rules{
      // Mouth: the vowel shapes MMD lip sync is built from.
      R("あ", {T({"jawOpen_a", "jawOpen"})}),
      R("い", {T({"jawOpen_yi", C("jawOpen", 0.6F)})}),
      R("う", {T({"jawOpen_wu", "mouthPucker"})}),
      R("え", {T({"jawOpen_ei", C("jawOpen", 0.7F)})}),
      R("お", {T({"jawOpen_o", "mouthFunnel"})}),
      R("ワ", {T({"jawOpen_a", "jawOpen"})}),
      R("ω", {T({"mouthPucker"})}),
      R("口角上げ", {T(0.5F, {"jawOpen_Smile_01_CLO", "jawOpen_Happy_01_CLO"})}),
      R("にやり", {T({"jawOpen_Smile_01_CLO", "jawOpen_Happy_01_CLO"})}),
      // Eyes.
      R("まばたき", {T({"biyan", C("TD_EyesClo", 0.5F)})}),
      R("ウィンク", {T({"biyan_L"})}),
      R("ウィンク２", {T({"biyan_L"})}),
      R("ウィンク右", {T({"biyan_R"})}),
      R("ｳｨﾝｸ２右", {T({"biyan_R"})}),
      R("笑い", {T({"EL_Smile_01_CLO"})}),
      R("にこり", {T({"EB_Smile_01"})}),
      R("喜び", {T({"EB_Happy_01"})}),
      R("びっくり", {T({"EL_Surprised_01", "EL_Surprised_01_OP"})}),
      R("怒り目", {T({"EL_Angry_01_OP"})}),
      R("じと目", {T({"EL_Despise_01_OP"})}),
      // Brows.
      R("困る", {T({"EB_Sad_01"})}),
      R("怒り", {T({"EB_Angry_01"})}),
      R("真面目", {T({"EB_Serious_01"})}),
      R("上", {T({"EB_UD_L"}), T({"EB_UD_R"})}),
      R("下", {T(0.5F, {"EB_Sad_01"})}),
      // Blush.
      R("照れ", {T({"TDS_lianhong", "TDS_haixiu"})}),
  };
  return rules;
}

// A resolved mapping for one character: which catalogue entries an MMD morph
// drives and by how much.
struct Drive {
  std::uint32_t entry;  // index into the character's morph catalogue
  float scale;
};

struct Resolved {
  std::string mmd;
  std::vector<Drive> drives;  // empty: no NTE morph for it on this character
};

// `names` is the character's morph list (catalogue order). Returns one entry
// per MMD morph in `mmd_names`, in that order.
inline std::vector<Resolved> Resolve(const std::vector<std::string> &mmd_names,
                                     const std::vector<std::string> &names) {
  const auto find = [&](const std::string_view name) -> std::int64_t {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<std::int64_t>(it - names.begin());
  };
  std::vector<Resolved> out;
  out.reserve(mmd_names.size());
  for (const auto &mmd : mmd_names) {
    Resolved resolved{mmd, {}};
    const auto rule = std::find_if(Rules().begin(), Rules().end(),
                                   [&](const Rule &r) { return r.mmd == mmd; });
    if (rule != Rules().end()) {
      for (const auto &target : rule->targets) {
        for (const auto &candidate : target.candidates) {
          const std::int64_t index = find(candidate.name);
          if (index >= 0) {
            resolved.drives.push_back(
                {static_cast<std::uint32_t>(index), target.scale * candidate.scale});
            break;
          }
        }
      }
    }
    out.push_back(std::move(resolved));
  }
  return out;
}

// Sample a sparse morph track: linear between keys, held at the ends.
struct Key {
  std::uint32_t frame;
  float weight;
};

inline float Sample(const std::vector<Key> &keys, const double frame) {
  if (keys.empty())
    return 0.0F;
  if (frame <= keys.front().frame)
    return keys.front().weight;
  if (frame >= keys.back().frame)
    return keys.back().weight;
  const auto next = std::upper_bound(keys.begin(), keys.end(), frame,
                                     [](const double f, const Key &k) { return f < k.frame; });
  const auto &b = *next;
  const auto &a = *(next - 1);
  const double span = static_cast<double>(b.frame) - static_cast<double>(a.frame);
  if (!(span > 0.0))
    return b.weight;
  const double t = (frame - static_cast<double>(a.frame)) / span;
  return static_cast<float>(a.weight + (b.weight - a.weight) * t);
}

// Combine the sampled MMD morphs into NTE weights: several MMD morphs can land
// on the same NTE morph (あ and ワ on jawOpen_a), and MMD adds morphs, so the
// contributions are summed and clamped. `touched` marks the NTE morphs the
// motion drives at all, so the caller can hand every other morph to the game.
inline void Combine(const std::vector<Resolved> &resolved, const std::vector<float> &mmd_weights,
                    const std::size_t nte_count, std::vector<float> &weights,
                    std::vector<std::uint8_t> &touched) {
  weights.assign(nte_count, 0.0F);
  touched.assign(nte_count, 0);
  const std::size_t count = (std::min)(resolved.size(), mmd_weights.size());
  for (std::size_t i{}; i != count; ++i) {
    for (const auto &drive : resolved[i].drives) {
      if (drive.entry >= nte_count)
        continue;
      weights[drive.entry] += mmd_weights[i] * drive.scale;
      touched[drive.entry] = 1;
    }
  }
  for (auto &weight : weights)
    weight = std::clamp(weight, 0.0F, 1.0F);
}

}  // namespace better_pose::mmd_morph
