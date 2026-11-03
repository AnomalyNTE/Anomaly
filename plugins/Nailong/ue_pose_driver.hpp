// Reads the same original skeletal component driven by BetterPose/MMD.

#pragma once

#include "ue_native_bridge.hpp"

#include "ue_bone_map.hpp"

#include <set>

namespace nailong {

// UE::EBoneSpaces::Type: WorldSpace=0, ComponentSpace=1.

// GetBoneTransform must be sampled in component space because the retargeter

// computes parent-relative deltas below.

inline constexpr std::uint8_t kBoneSpaceWorld=0;

inline constexpr std::uint8_t kBoneSpaceComponent=1;

inline bool ValidTransform(const ue::Transform& t){

    // FTransform padding has no value contract and may contain arbitrary bits.

    for(double v:t.rotation)if(!std::isfinite(v))return false;

    for(double v:t.translation)if(!std::isfinite(v))return false;

    for(double v:t.scale)if(!std::isfinite(v))return false;

    double q=0;for(auto v:t.rotation)q+=v*v;

    return q>.9&&q<1.1&&std::abs(t.scale[0])>1e-8&&std::abs(t.scale[1])>1e-8&&std::abs(t.scale[2])>1e-8;

}

inline M4 TransformMatrix(const ue::Transform& t){

    auto m=Quaternion(t.rotation[0],t.rotation[1],t.rotation[2],t.rotation[3]);

    for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i*4+j]*=t.scale[j];SetTranslation(m,{t.translation[0],t.translation[1],t.translation[2]});return m;

}

inline bool ComponentMatrix(std::uintptr_t obj,M4& m){ue::Transform t;if(!ue::Call(obj,"/Script/Engine.SceneComponent.K2_GetComponentToWorld",&t,96)||!ValidTransform(t))return false;m=TransformMatrix(t);return true;}

struct PoseDriver {

    ue::Ref mesh,asset;

    std::vector<std::array<std::uint8_t,8>> fnames;

    std::vector<std::string> names;

    std::vector<std::int32_t> parents,role,sample_indices;

    std::vector<M4> reference,alignment;

    M4 game_to_root=Identity(),root_to_game=Identity();

    std::int32_t asset_offset=-1;std::size_t mapped=0;

    std::string mapping_summary,reference_source,diagnostic;

    std::uint64_t sample_count=0;

    bool ReadReferenceFromAsset(std::uintptr_t ptr){

        const std::size_t count=names.size();

        for(std::size_t offset=16;offset+16<=0x800;offset+=8){

            ue::Array pose{},info{};

            if(!ue::nl_read(ptr+offset,&pose,16)||pose.num!=std::int32_t(count)||pose.max<pose.num||pose.max>1000000||!pose.data)continue;

            if(!ue::nl_read(ptr+offset-16,&info,16)||info.num!=pose.num||info.max<info.num||!info.data)continue;

            std::vector<ue::Transform> local(count);if(!ue::nl_read(reinterpret_cast<std::uintptr_t>(pose.data),local.data(),count*96))continue;

            bool transforms=true;for(auto& t:local){if(!ValidTransform(t)){transforms=false;break;}for(auto s:t.scale)if(std::abs(s-1)>.05)transforms=false;}if(!transforms)continue;

            for(std::size_t stride:{std::size_t(12),std::size_t(16)}){

                std::vector<std::uint8_t> metadata(count*stride);if(!ue::nl_read(reinterpret_cast<std::uintptr_t>(info.data),metadata.data(),metadata.size()))continue;

                std::vector<std::int32_t> candidate(count);bool match=true;

                for(std::size_t i=0;i<count;++i){std::uint32_t id=0;std::memcpy(&id,metadata.data()+i*stride,4);std::memcpy(&candidate[i],metadata.data()+i*stride+8,4);

                    std::uint32_t expected=0;std::memcpy(&expected,fnames[i].data(),4);

                    if(id!=expected||candidate[i]>=std::int32_t(i)||candidate[i]<-1){match=false;break;}}

                if(!match)continue;parents=std::move(candidate);reference.resize(count);

                for(std::size_t i=0;i<count;++i){reference[i]=TransformMatrix(local[i]);if(parents[i]>=0)reference[i]=Mul(reference[parents[i]],reference[i]);}

                return true;

            }

        }return false;

    }

    bool ReadReferenceFromComponent(std::uintptr_t component,std::string& error){

        const std::size_t count=names.size();

        std::vector<M4> local(count);std::vector<std::int32_t> official_parents(count,-1);

        for(std::size_t i=0;i<count;++i){

            alignas(16) std::array<std::uint8_t,112> transform_params{};

            const auto index=std::int32_t(i);std::memcpy(transform_params.data(),&index,4);

            if(!ue::Call(component,"/Script/Engine.SkinnedMeshComponent.GetRefPoseTransform",transform_params.data(),transform_params.size())){

                error="GetRefPoseTransform failed";return false;

            }

            ue::Transform t;std::memcpy(&t,transform_params.data()+16,96);

            if(!ValidTransform(t)){error="GetRefPoseTransform returned an invalid transform";return false;}

            local[i]=TransformMatrix(t);

            alignas(16) std::array<std::uint8_t,16> parent_params{};

            std::memcpy(parent_params.data(),fnames[i].data(),8);

            if(!ue::Call(component,"/Script/Engine.SkinnedMeshComponent.GetParentBone",parent_params.data(),parent_params.size())){

                error="GetParentBone failed";return false;

            }

            std::array<std::uint8_t,8> parent_name{};std::memcpy(parent_name.data(),parent_params.data()+8,8);

            bool parent_is_none=true;for(const auto byte:parent_name)parent_is_none=parent_is_none&&(byte==0);

            for(std::size_t j=0;j<count;++j){

                if(std::memcmp(parent_name.data(),fnames[j].data(),8)==0){official_parents[i]=std::int32_t(j);break;}

            }

            if(!parent_is_none&&official_parents[i]<0){error="GetParentBone returned an unknown parent";return false;}

            if(official_parents[i]>=std::int32_t(i)){error="GetParentBone returned a non-topological parent";return false;}

        }

        parents=std::move(official_parents);reference.resize(count);

        for(std::size_t i=0;i<count;++i){

            reference[i]=local[i];

            if(parents[i]>=0)reference[i]=Mul(reference[std::size_t(parents[i])],reference[i]);

        }

        reference_source="component-refpose";return true;

    }

    bool Init(std::uintptr_t component,std::uintptr_t root,const SourceRig& source,std::string& error){

        mapped=0;mapping_summary.clear();reference_source.clear();diagnostic.clear();sample_indices.clear();sample_count=0;

        if(!ue::Capture(component,mesh)){error="driver skeletal mesh identity invalid";return false;}

        // GetNumBones reads the pose/bone state of the component and returns 0 on
        // freshly assembled or budgeted NPC meshes (AHTNPCCharacter_Sub assembles
        // head/top/bottom from separate UHTSkeletalMeshComponentBudgeted parts, and
        // a budgeted part can report an empty pose before its skeleton registers).
        // Never trust the count alone: probe GetBoneName until it stops answering,
        // and only fall back to GetNumBones when the probe found nothing.
        std::int32_t declared=0;
        for(int attempt=0;attempt<3&&declared<=0;++attempt){
            if(ue::Call(component,"/Script/Engine.SkinnedMeshComponent.GetNumBones",&declared,4)&&declared>0&&declared<=1024)break;
            declared=0;
        }

        std::vector<std::array<std::uint8_t,8>> probed_names;
        std::vector<std::string> probed_text;
        // GetBoneName returns a zero FName (not an error) once the index passes the
        // last bone, but a fresh budgeted mesh can also fail the call outright on
        // the very first index. Retry the first few indices before believing it, and
        // treat a name that cannot be resolved as "no more bones" only after it has
        // happened twice in a row.
        const std::int32_t probe_limit=declared>0?declared:1024;
        int misses_in_a_row=0;
        for(std::int32_t i=0;i<probe_limit;++i){
            std::array<std::uint8_t,12> p{};std::memcpy(p.data(),&i,4);
            bool ok=ue::Call(component,"/Script/Engine.SkinnedMeshComponent.GetBoneName",p.data(),p.size());
            if(!ok&&i<4){
                // Give a just-assembled component a chance to register its skeleton.
                for(int retry=0;retry<3&&!ok;++retry){
                    ok=ue::Call(component,"/Script/Engine.SkinnedMeshComponent.GetBoneName",p.data(),p.size());
                }
            }
            if(!ok){if(++misses_in_a_row>=2)break;continue;}

            std::array<std::uint8_t,8> fname{};std::memcpy(fname.data(),p.data()+4,8);
            bool is_none=true;for(const auto byte:fname)is_none=is_none&&(byte==0);
            if(is_none)break;

            std::uint32_t id=0;std::memcpy(&id,fname.data(),4);char name[192]{};std::size_t n=sizeof(name);
            if(!ue::g_names||!ue::nl_good(ue::g_names->resolve_utf8(ue::g_names->user,id,name,&n))){
                if(++misses_in_a_row>=2)break;
                continue;
            }
            misses_in_a_row=0;
            probed_names.push_back(fname);probed_text.push_back(name);
        }

        if(probed_names.empty()){error="driver bone count unavailable";return false;}

        fnames=std::move(probed_names);names=std::move(probed_text);

        asset_offset=ue::PropertyOffset("/Script/Engine.SkinnedMeshComponent","SkeletalMesh",8);

        if(asset_offset<0)asset_offset=ue::PropertyOffset("/Script/Engine.SkinnedMeshComponent","SkinnedAsset",8);

        std::uintptr_t assetptr=0;if(asset_offset<0||!ue::nl_read_ptr(component+std::uintptr_t(asset_offset),&assetptr)||!ue::Capture(assetptr,asset)){

            error="skeletal asset identity unavailable";return false;

        }

        std::string reference_error;

        if(!ReadReferenceFromComponent(component,reference_error)){

            if(!ReadReferenceFromAsset(assetptr)){

                error="validated reference pose unavailable: "+reference_error;return false;

            }

            reference_source="asset-refpose-fallback";

        }

        M4 root_world{},mesh_world{},inverse_root{};

        if(!ComponentMatrix(root,root_world)||!ComponentMatrix(component,mesh_world)||!Inverse(root_world,inverse_root)){error="driver component axes unavailable";return false;}

        game_to_root=Rotation(Mul(inverse_root,mesh_world));if(!Inverse(game_to_root,root_to_game)){error="driver axes singular";return false;}

        auto automatic=MapHumanoidBones(names,parents,reference,source);

        if(!automatic.error.empty()){error=automatic.error;return false;}

        role=std::move(automatic.target_by_source);mapped=automatic.mapped;

        alignment.assign(source.bones.size(),Identity());

        std::set<std::int32_t> needed;

        for(std::size_t i=0;i<role.size();++i){

            if(role[i]<0)continue;

            std::int32_t target=role[i];while(target>=0){needed.insert(target);target=parents[std::size_t(target)];}

            const auto source_parent=source.bones[i].parent;

            if(source_parent<0){

                // source root coordinates -> target mesh coordinates

                alignment[i]=root_to_game;

                continue;

            }

            const std::int32_t target_parent=parents[std::size_t(role[i])];

            const M4 target_parent_root=target_parent>=0

                ?Mul(game_to_root,Rotation(reference[std::size_t(target_parent)])):game_to_root;

            const M4 source_parent_root=Rotation(source.bones[std::size_t(source_parent)].bind);

            M4 inverse_target{};if(!Inverse(target_parent_root,inverse_target)){error="target parent axes singular";return false;}

            // Maps a vector in source-parent coordinates into target-parent coordinates.

            alignment[i]=Mul(inverse_target,source_parent_root);

        }

        sample_indices.assign(needed.begin(),needed.end());

        mapping_summary=std::to_string(mapped)+"/"+std::to_string(source.bones.size());

        // A humanoid target must map most of the source skeleton. Anything below
        // the floor is a non-humanoid rig (animal/vehicle/prop) and animating it
        // with this rig looks frozen -- refuse rather than ship a corpse.
        const std::size_t min_mapped=(source.bones.size()*3u)/4u;
        if(mapped<min_mapped){
            error="target rig is not humanoid (mapped "+mapping_summary+", need "+std::to_string(min_mapped)+")";
            return false;
        }

        return true;

    }

    bool Sample(const SourceRig& source,double height,std::vector<M4>& pose,std::string& error){

        std::uintptr_t current_asset=0;

        if(ue::Alive(mesh)!=1||ue::Alive(asset)!=1||!ue::nl_read_ptr(mesh.ptr+std::uintptr_t(asset_offset),&current_asset)||current_asset!=asset.ptr){error="driver skeletal mesh changed";return false;}

        std::vector<M4> current(names.size());std::vector<bool> have(names.size(),false);

        for(const auto index:sample_indices){

            alignas(16) std::array<std::uint8_t,112> p{};std::memcpy(p.data(),fnames[std::size_t(index)].data(),8);p[8]=kBoneSpaceComponent; // EBoneSpaces::ComponentSpace

            if(!ue::Call(mesh.ptr,"/Script/Engine.SkinnedMeshComponent.GetBoneTransform",p.data(),p.size())){error=ue::last_error;return false;}

            ue::Transform t;std::memcpy(&t,p.data()+16,96);if(!ValidTransform(t)){error="animated bone transform invalid";return false;}current[std::size_t(index)]=TransformMatrix(t);have[std::size_t(index)]=true;

        }

        std::vector<M4> local_delta(source.bones.size(),Identity());

        V3 offset{};

        std::size_t duplicate_roles=0,pelvis_helpers=0;

        for(std::size_t i=0;i<source.bones.size();++i){

            const auto target_index=role[i];if(target_index<0)continue;

            if(!have[std::size_t(target_index)]){error="mapped target bone was not sampled";return false;}

            const M4 current_rotation=Rotation(current[std::size_t(target_index)]);

            const M4 reference_rotation=Rotation(reference[std::size_t(target_index)]);

            M4 inverse_reference{};if(!Inverse(reference_rotation,inverse_reference)){error="reference rotation singular";return false;}

            if(IsPelvisRoot(source,i)){

                // The actor component already supplies world heading. Applying

                // animated pelvis pitch/roll to the source rig's root tips the

                // entire replacement sideways. Keep root rotation at bind; move

                // the mesh with the root-position delta, and drive colocated

                // pelvis helper bones locally below instead.

                local_delta[i]=Identity();

                offset=Vector(game_to_root,Sub(Translation(current[std::size_t(target_index)]),Translation(reference[std::size_t(target_index)])));

                for(auto& v:offset)v/=height;

                continue;

            }

            if(SuppressDuplicateRoleMotion(source,i)){

                // Repeated non-pelvis helper roles (head/spine.006, foot/heel)

                // inherit their mapped ancestor and must not apply it twice.

                local_delta[i]=Identity();++duplicate_roles;continue;

            }

            if(IsRepeatedPelvisHelper(source,i))++pelvis_helpers;

            const auto target_parent=parents[std::size_t(target_index)];

            const M4 current_parent=target_parent>=0?Rotation(current[std::size_t(target_parent)]):Identity();

            const M4 reference_parent=target_parent>=0?Rotation(reference[std::size_t(target_parent)]):Identity();

            M4 inverse_current_parent{},inverse_reference_parent{};

            if(!Inverse(current_parent,inverse_current_parent)||!Inverse(reference_parent,inverse_reference_parent)){error="target parent rotation singular";return false;}

            const M4 current_local=Mul(inverse_current_parent,current_rotation);

            const M4 reference_local=Mul(inverse_reference_parent,reference_rotation);

            M4 inverse_reference_local{};if(!Inverse(reference_local,inverse_reference_local)){error="target local reference singular";return false;}

            local_delta[i]=Mul(current_local,inverse_reference_local);

        }

        ++sample_count;

        if((sample_count%30)==1){

            const auto angle_degrees=[](const M4& m){

                const double trace=m[0]+m[5]+m[10];

                const double c=std::clamp((trace-1.0)*.5,-1.0,1.0);

                return std::acos(c)*180.0/3.14159265358979323846;

            };

            auto angle_for=[&](const char* role_name){

                for(std::size_t i=0;i<source.bones.size();++i)if(source.bones[i].role==role_name)return angle_degrees(local_delta[i]);

                return 0.0;

            };

            diagnostic="ref="+reference_source+" map="+mapping_summary+

                " pelvis="+std::to_string(angle_for("Bip001-Pelvis"))+"deg"+

                " LUpper="+std::to_string(angle_for("Bip001-L-UpperArm"))+"deg"+

                " RUpper="+std::to_string(angle_for("Bip001-R-UpperArm"))+"deg"+

                " LThigh="+std::to_string(angle_for("Bip001-L-Thigh"))+"deg"+

                " RThigh="+std::to_string(angle_for("Bip001-R-Thigh"))+"deg"+

                " rootPelvis=locked pelvisHelpers="+std::to_string(pelvis_helpers)+

                " dup="+std::to_string(duplicate_roles)+

                " offset="+std::to_string(offset[0])+","+std::to_string(offset[1])+","+std::to_string(offset[2]);

        }

        pose=RetargetLocal(source,local_delta,alignment,offset);return true;

    }

};

} // namespace nailong
