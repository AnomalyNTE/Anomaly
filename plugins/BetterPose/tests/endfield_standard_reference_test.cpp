#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void Check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

int Find(const nlohmann::json &bones, const std::string &name) {
  for (const auto &bone : bones) {
    if (bone.value("name", std::string()) == name)
      return bone.at("index").get<int>();
  }
  return -1;
}

}  // namespace

int main() {
  std::ifstream input("plugins/BetterPose/data/reference-endfield-standard-a.json",
                      std::ios::binary);
  Check(input.good(), "Endfield standard reference file exists");
  const auto json = nlohmann::json::parse(input);
  Check(json.at("kind") == "better-pose-reference-pmx",
        "reference uses the MMD source table schema");
  Check(json.value("modelName", std::string()) ==
            "Endfield Standard MMD / A-pose",
        "reference identifies the Endfield Standard MMD A pose");
  Check(json.value("source", std::string()) == "Endfield-Poser StandardRig" &&
            json.value("license", std::string()) == "AGPL-3.0-only",
        "reference records its compatible source and license");
  const auto &bones = json.at("bones");
  Check(json.value("boneCount", 0) == 73 && bones.size() == 73,
        "reference contains the complete 73-bone standard rig");
  for (const char *name :
       {"全ての親",
        "センター",
        "上半身3",
        "左腕",
        "右ひじ",
        "左中指1",
        "右親指2",
        "左足IK",
        "右つま先IK"})
    Check(Find(bones, name) >= 0, "reference contains required standard bones");
  Check(Find(bones, "上半身3") > Find(bones, "上半身2"),
        "standard spine extension is present");
  const int left_arm = Find(bones, "左腕");
  const int left_wrist = Find(bones, "左手首");
  const int right_arm = Find(bones, "右腕");
  const int right_wrist = Find(bones, "右手首");
  Check(bones.at(left_arm).at("position").at(1).get<double>() >
            bones.at(left_wrist).at("position").at(1).get<double>() &&
            bones.at(right_arm).at("position").at(1).get<double>() >
                bones.at(right_wrist).at("position").at(1).get<double>(),
        "arms use the Standard MMD A-pose slope");
  const int left_foot_ik = Find(bones, "左足IK");
  const auto &ik = bones.at(left_foot_ik).at("ik");
  Check(ik.at("target").get<int>() == Find(bones, "左足首") &&
            ik.at("links").size() == 2,
        "left leg carries its standard two-bone IK chain");
  Check(bones.at(Find(bones, "左親指1")).at("position").at(2).get<double>() !=
            bones.at(Find(bones, "左人指1")).at("position").at(2).get<double>(),
        "thumb remains an out-of-plane finger chain");
  const auto &head_tail = bones.at(Find(bones, "頭")).at("tailOffset");
  Check(head_tail.at(0).get<double>() == 0.0 &&
            head_tail.at(1).get<double>() > 0.0 &&
            head_tail.at(2).get<double>() == 0.0,
        "head uses a vertical cranial axis instead of the eye offset");

  std::cout << "PASS Endfield Standard MMD reference schema and A-pose\n";
  return 0;
}
