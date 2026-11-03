#pragma once

#include "ue_skin.hpp"

#include <cctype>

#include <limits>

#include <string_view>

#include <vector>



namespace nailong {



enum class BonePart { Unknown, Pelvis, Spine, Neck, Head, Clavicle, UpperArm, Forearm, Hand, Thigh, Calf, Foot, Toe };

struct BoneSemantic { BonePart part=BonePart::Unknown; int side=0; int number=-1; };



inline std::string BoneLower(std::string_view value) {

    std::string out;

    out.reserve(value.size());

    for (const unsigned char c : value) out.push_back(char(std::tolower(c)));

    return out;

}

inline std::vector<std::string> BoneTokens(std::string_view value) {

    std::vector<std::string> tokens;

    std::string current;

    const auto flush=[&](){ if(!current.empty()){tokens.push_back(current);current.clear();} };

    for (const unsigned char c : value) {

        if (std::isalnum(c)) current.push_back(char(std::tolower(c)));

        else flush();

    }

    flush();

    return tokens;

}

inline bool BoneStarts(std::string_view value,std::string_view prefix) {

    return value.size()>=prefix.size()&&value.substr(0,prefix.size())==prefix;

}

inline int BoneDigits(std::string_view value,std::string_view prefix) {

    if(!BoneStarts(value,prefix)||value.size()==prefix.size())return -1;

    int number=0;

    for(std::size_t i=prefix.size();i<value.size();++i){

        if(value[i]<'0'||value[i]>'9')return -1;

        number=number*10+(value[i]-'0');

        if(number>99)return -1;

    }

    return number;

}

inline BoneSemantic ParseBoneSemantic(std::string_view value) {

    BoneSemantic result{};

    const auto raw=BoneTokens(value);

    std::vector<std::string> tokens;tokens.reserve(raw.size()+2);

    for(std::size_t i=0;i<raw.size();++i){

        const auto& token=raw[i];

        if((token=="upper"||token=="lower")&&i+1<raw.size()&&raw[i+1]=="arm"){

            tokens.push_back(token+"arm");++i;continue;

        }

        if((token=="upper"||token=="lower")&&i+1<raw.size()&&raw[i+1]=="leg"){

            tokens.push_back(token+"leg");++i;continue;

        }

        if(BoneStarts(token,"left")&&token.size()>4){tokens.push_back("left");tokens.push_back(token.substr(4));continue;}

        if(BoneStarts(token,"right")&&token.size()>5){tokens.push_back("right");tokens.push_back(token.substr(5));continue;}

        const auto looks_like_stem=[](std::string_view stem){

            return stem=="pelvis"||stem=="spine"||stem=="neck"||stem=="head"||stem=="clavicle"||

                stem=="shoulder"||stem=="upperarm"||stem=="forearm"||stem=="hand"||stem=="thigh"||

                stem=="calf"||stem=="shin"||stem=="foot"||stem=="toe";

        };

        if(token.size()>1&&token.back()=='l'&&looks_like_stem(token.substr(0,token.size()-1))){

            tokens.push_back(token.substr(0,token.size()-1));tokens.push_back("l");continue;

        }

        if(token.size()>1&&token.back()=='r'&&looks_like_stem(token.substr(0,token.size()-1))){

            tokens.push_back(token.substr(0,token.size()-1));tokens.push_back("r");continue;

        }

        tokens.push_back(token);

    }

    for(const auto& token:tokens){

        if(token=="l"||token=="left")result.side=-1;

        else if(token=="r"||token=="right")result.side=1;

    }

    for(std::size_t token_index=0;token_index<tokens.size();++token_index){

        const auto& token=tokens[token_index];

        if(token=="bip001"||token=="bip01"||token=="mixamorig"||token=="armature"||token=="def"||token=="jnt"||token=="joint")continue;

        if(token=="pelvis"||token=="hips"||token=="hip"||token=="root"){

            if(result.part==BonePart::Unknown)result.part=BonePart::Pelvis;

            continue;

        }

        if(BoneStarts(token,"spine")){

            result.part=BonePart::Spine;

            const int n=BoneDigits(token,"spine");

            if(n>=0)result.number=n;

            else if(token=="spine"&&token_index+1<tokens.size()&&tokens[token_index+1].size()<=2){

                bool digits=true;for(const char c:tokens[token_index+1])digits=digits&&(c>='0'&&c<='9');

                if(digits)result.number=std::stoi(tokens[token_index+1]);

            }

            continue;

        }

        if(token=="chest"||token=="torso"){

            result.part=BonePart::Spine;

            if(result.number<0)result.number=2;

            continue;

        }

        if(token=="neck"||BoneStarts(token,"neck")){result.part=BonePart::Neck;continue;}

        if(token=="head"||BoneStarts(token,"head")){result.part=BonePart::Head;continue;}

        if(token=="clavicle"||token=="shoulder"){result.part=BonePart::Clavicle;continue;}

        if(token=="upperarm"||token=="humerus"||token=="bicep"||token=="arm"){

            result.part=BonePart::UpperArm;continue;

        }

        if(token=="forearm"||token=="lowerarm"||token=="elbow"){

            result.part=BonePart::Forearm;continue;

        }

        if(token=="hand"||token=="wrist"||token=="palm"){result.part=BonePart::Hand;continue;}

        if(token=="thigh"||token=="upperleg"||token=="upleg"){

            result.part=BonePart::Thigh;continue;

        }

        if(token=="calf"||token=="shin"||token=="lowerleg"){

            result.part=BonePart::Calf;continue;

        }

        if(token=="foot"||token=="ankle"){result.part=BonePart::Foot;continue;}

        if(token=="toe"||token=="toe0"||token=="ball"){result.part=BonePart::Toe;continue;}

    }

    return result;

}

inline std::string BoneCanonical(std::string_view value) {

    std::string out;

    for(const auto& token:BoneTokens(value)){out+=token;out.push_back('|');}

    return out;

}

inline bool SameBoneSemantic(const BoneSemantic& a,const BoneSemantic& b) {

    return a.part!=BonePart::Unknown&&a.part==b.part&&a.side==b.side&&

        (a.part!=BonePart::Spine||a.number==b.number);

}

inline bool HasSameRoleAncestor(const SourceRig& source,std::size_t index) {

    if(index>=source.bones.size()||source.bones[index].role.empty())return false;

    const auto expected=ParseBoneSemantic(source.bones[index].role);

    for(auto parent=source.bones[index].parent;parent>=0&&std::size_t(parent)<source.bones.size();parent=source.bones[std::size_t(parent)].parent){

        const auto& ancestor=source.bones[std::size_t(parent)];

        if(!ancestor.role.empty()&&SameBoneSemantic(expected,ParseBoneSemantic(ancestor.role)))return true;

    }

    return false;

}

inline bool IsPelvisRoot(const SourceRig& source,std::size_t index) {

    return index<source.bones.size()&&source.bones[index].parent<0&&

        ParseBoneSemantic(source.bones[index].role).part==BonePart::Pelvis;

}

inline bool IsRepeatedPelvisHelper(const SourceRig& source,std::size_t index) {

    return index<source.bones.size()&&ParseBoneSemantic(source.bones[index].role).part==BonePart::Pelvis&&

        HasSameRoleAncestor(source,index);

}

inline bool SuppressDuplicateRoleMotion(const SourceRig& source,std::size_t index) {

    return HasSameRoleAncestor(source,index)&&!IsRepeatedPelvisHelper(source,index);

}

inline bool BoneOptional(BonePart part) { return part==BonePart::Clavicle||part==BonePart::Toe; }

inline int BoneSemanticScore(const BoneSemantic& expected,const BoneSemantic& candidate) {

    if(expected.part==BonePart::Unknown||candidate.part!=expected.part)return -100000;

    if(expected.side!=0&&candidate.side!=expected.side)return -100000;

    if(expected.side==0&&candidate.side!=0&&expected.part!=BonePart::Spine)return -100000;

    int score=100;

    if(expected.side==candidate.side)score+=15;

    if(expected.part==BonePart::Spine){

        if(expected.number>=0&&candidate.number>=0){

            const int distance=std::abs(expected.number-candidate.number);

            if(distance==0)score+=35;else if(distance==1)score+=12;else score-=distance*20;

        } else if(expected.number<0&&candidate.number<0)score+=25;

    }

    return score;

}



struct AutomaticBoneMap {
    int unmatched_roles=0;
    int ambiguous_roles=0;

    std::vector<std::int32_t> target_by_source;

    std::size_t mapped=0;

    std::string error;

};



inline AutomaticBoneMap MapHumanoidBones(

    const std::vector<std::string>& target_names,

    const std::vector<std::int32_t>& target_parents,

    const std::vector<M4>& target_reference,

    const SourceRig& source) {

    AutomaticBoneMap result;

    result.target_by_source.assign(source.bones.size(),-1);

    if(target_names.size()!=target_parents.size()||target_names.size()!=target_reference.size()){

        result.error="target skeleton metadata size mismatch";return result;

    }

    std::vector<BoneSemantic> candidates;

    candidates.reserve(target_names.size());

    std::vector<std::int32_t> target_owner(target_names.size(),-1);

    for(const auto& name:target_names)candidates.push_back(ParseBoneSemantic(name));

    std::vector<std::string> canonical;

    canonical.reserve(target_names.size());

    for(const auto& name:target_names)canonical.push_back(BoneCanonical(name));

    const auto isAncestor=[&](std::int32_t child,std::int32_t ancestor){

        for(int hops=0;child>=0&&hops<8;++hops){if(child==ancestor)return true;child=target_parents[std::size_t(child)];}

        return false;

    };

    for(std::size_t si=0;si<source.bones.size();++si){

        const auto& source_bone=source.bones[si];

        if(source_bone.role.empty())continue;

        const BoneSemantic expected=ParseBoneSemantic(source_bone.role);

        if(expected.part==BonePart::Unknown){result.error="cannot classify source role: "+source_bone.role;return result;}

        const std::string exact=BoneCanonical(source_bone.role);

        int best=-100000,second=-100000;std::int32_t best_index=-1;

        for(std::size_t ti=0;ti<target_names.size();++ti){

            int score=BoneSemanticScore(expected,candidates[ti]);

            if(score<-1000)continue;

            if(target_owner[ti]>=0){

                const auto& owner_role=source.bones[std::size_t(target_owner[ti])].role;

                if(!SameBoneSemantic(expected,ParseBoneSemantic(owner_role)))continue;

            }

            if(canonical[ti]==exact)score+=1000;

            const auto parent=source_bone.parent>=0&&std::size_t(source_bone.parent)<result.target_by_source.size()

                ?result.target_by_source[std::size_t(source_bone.parent)]:-1;

            if(parent>=0){

                if(target_parents[ti]==parent)score+=90;

                else if(isAncestor(target_parents[ti],parent)||isAncestor(parent,target_parents[ti]))score+=20;

                else score-=18;

            }

            if(score>best){second=best;best=score;best_index=std::int32_t(ti);}else if(score>second)second=score;

        }

        if(best_index<0){

            if(BoneOptional(expected.part))continue;

            // Modular NPC rigs (AHTNPCCharacter_Sub and friends) assemble head/top/
            // bottom from separate budgeted meshes and routinely lack a secondary
            // spine helper such as Bip001-Spine2. Aborting here dropped such an NPC
            // to a static replacement -- the pose was available, we simply refused
            // it. Skip the unmatched role and keep the rest of the skeleton live.
            ++result.unmatched_roles;
            continue;

        }

        if(second>=best-8&&best<1000){

            // Two candidates scored almost the same. Aborting here dropped the whole
            // NPC to a static replacement (runtime: "ambiguous target bones for role:
            // Bip001-Pelvis"), even though best_index is a perfectly usable pick.
            // Prefer the higher-scoring bone and record the tie; a slightly imperfect
            // pelvis beats a frozen model.
            ++result.ambiguous_roles;

        }

        result.target_by_source[si]=best_index;target_owner[std::size_t(best_index)]=std::int32_t(si);++result.mapped;

    }

    return result;

}



} // namespace nailong
