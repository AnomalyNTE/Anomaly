// Source-rig retargeting and four-weight LBS. Independent of the game/renderer.

#pragma once

#include "ue_model.hpp"

#include <algorithm>

#include <map>

namespace nailong {

using V3=std::array<double,3>;

using M4=std::array<double,16>;

inline M4 Identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}

inline M4 Mul(const M4& a,const M4& b){M4 c{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)c[i*4+j]+=a[i*4+k]*b[k*4+j];return c;}

inline V3 Point(const M4& a,const V3& v){return {a[0]*v[0]+a[1]*v[1]+a[2]*v[2]+a[3],a[4]*v[0]+a[5]*v[1]+a[6]*v[2]+a[7],a[8]*v[0]+a[9]*v[1]+a[10]*v[2]+a[11]};}

inline V3 Vector(const M4& a,const V3& v){return {a[0]*v[0]+a[1]*v[1]+a[2]*v[2],a[4]*v[0]+a[5]*v[1]+a[6]*v[2],a[8]*v[0]+a[9]*v[1]+a[10]*v[2]};}

inline V3 Add(V3 a,V3 b){for(int i=0;i<3;++i)a[i]+=b[i];return a;}

inline V3 Sub(V3 a,V3 b){for(int i=0;i<3;++i)a[i]-=b[i];return a;}

inline double Dot(V3 a,V3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}

inline V3 Normalize(V3 a){auto n=std::sqrt(Dot(a,a));if(n>1e-12)for(auto& v:a)v/=n;return a;}

inline V3 Cross(V3 a,V3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}

inline V3 Translation(const M4& a){return {a[3],a[7],a[11]};}

inline void SetTranslation(M4& a,V3 p){a[3]=p[0];a[7]=p[1];a[11]=p[2];}

inline bool Inverse(const M4& a,M4& b){

    const double det=a[0]*(a[5]*a[10]-a[6]*a[9])-a[1]*(a[4]*a[10]-a[6]*a[8])+a[2]*(a[4]*a[9]-a[5]*a[8]);

    if(!std::isfinite(det)||std::abs(det)<1e-15)return false;

    b=Identity();b[0]=(a[5]*a[10]-a[6]*a[9])/det;b[1]=(a[2]*a[9]-a[1]*a[10])/det;b[2]=(a[1]*a[6]-a[2]*a[5])/det;

    b[4]=(a[6]*a[8]-a[4]*a[10])/det;b[5]=(a[0]*a[10]-a[2]*a[8])/det;b[6]=(a[2]*a[4]-a[0]*a[6])/det;

    b[8]=(a[4]*a[9]-a[5]*a[8])/det;b[9]=(a[1]*a[8]-a[0]*a[9])/det;b[10]=(a[0]*a[5]-a[1]*a[4])/det;

    auto t=Vector(b,Translation(a));for(auto& x:t)x=-x;SetTranslation(b,t);return true;

}

inline M4 Rotation(const M4& a){M4 r=Identity();for(int j=0;j<3;++j){auto v=Normalize(V3{a[j],a[4+j],a[8+j]});for(int i=0;i<3;++i)r[i*4+j]=v[i];}return r;}

inline M4 Quaternion(double x,double y,double z,double w){

    M4 r=Identity();r[0]=1-2*(y*y+z*z);r[1]=2*(x*y-z*w);r[2]=2*(x*z+y*w);

    r[4]=2*(x*y+z*w);r[5]=1-2*(x*x+z*z);r[6]=2*(y*z-x*w);

    r[8]=2*(x*z-y*w);r[9]=2*(y*z+x*w);r[10]=1-2*(x*x+y*y);return r;

}

inline M4 Aim(V3 from,V3 to){

    from=Normalize(from);to=Normalize(to);const double d=std::clamp(Dot(from,to),-1.,1.);

    if(d>.999999)return Identity();auto axis=Cross(from,to);

    if(d<-.999999){axis=Normalize(Cross(from,std::abs(from[0])<.9?V3{1,0,0}:V3{0,1,0}));return Quaternion(axis[0],axis[1],axis[2],0);}

    const double w=std::sqrt((1+d)*.5),s=.5/w;return Quaternion(axis[0]*s,axis[1]*s,axis[2]*s,w);

}

struct SourceBone {std::int32_t parent=-1;std::string name,role;M4 bind{},inverse{};};

struct SourceRig {std::vector<SourceBone> bones;};

inline bool ParseRig(std::span<const std::uint8_t> bytes,SourceRig& result,std::string& error){

    auto fail=[&](const char* e){error=e;return false;};

    if(bytes.size()<12||std::memcmp(bytes.data(),"NLBR",4))return fail("rig header invalid");

    auto read=[&]<typename T>(std::size_t p){T v{};std::memcpy(&v,bytes.data()+p,sizeof(v));return v;};

    if(read.template operator()<std::uint32_t>(4)!=1)return fail("rig version invalid");auto count=read.template operator()<std::uint32_t>(8);

    if(!count||count>256)return fail("rig bone count invalid");SourceRig parsed;std::size_t off=12;

    for(std::uint32_t i=0;i<count;++i){

        if(bytes.size()-off<136)return fail("truncated bone");SourceBone b;b.parent=read.template operator()<std::int32_t>(off);

        const auto name=read.template operator()<std::uint16_t>(off+4),role=read.template operator()<std::uint16_t>(off+6);

        if(!name||name>128||role>128||std::size_t(136+name+role)>bytes.size()-off||b.parent>=std::int32_t(i)||b.parent<-1)return fail("bone hierarchy invalid");

        std::memcpy(b.bind.data(),bytes.data()+off+8,128);for(double v:b.bind)if(!std::isfinite(v))return fail("nonfinite bind matrix");

        if(std::abs(b.bind[12])+std::abs(b.bind[13])+std::abs(b.bind[14])+std::abs(b.bind[15]-1)>1e-9||!Inverse(b.bind,b.inverse))return fail("singular/non-affine bind");

        b.name.assign(reinterpret_cast<const char*>(bytes.data()+off+136),name);b.role.assign(reinterpret_cast<const char*>(bytes.data()+off+136+name),role);

        off+=136+name+role;parsed.bones.push_back(std::move(b));

    }

    if(off!=bytes.size())return fail("rig trailing bytes");result=std::move(parsed);error.clear();return true;

}

inline bool ValidateWeights(const ModelAsset& asset,const SourceRig& rig,std::string& error){

    for(const auto& s:asset.sections){

        if(s.weights.size()!=s.positions.size()||s.bone_indices.size()!=s.positions.size()){

            error="bone data count mismatch";return false;

        }

        for(std::size_t i=0;i<s.weights.size();++i){

            double sum=0;

            for(int j=0;j<4;++j){

                auto w=s.weights[i][j];

                if(!std::isfinite(w)||w<0||w>1||s.bone_indices[i][j]>=rig.bones.size()){

                    error="bone weight/index invalid";return false;

                }

                sum+=w;

            }

            if(std::abs(sum-1)>1e-5){error="bone weights not normalized";return false;}

        }

    }

    return true;

}

inline M4 InSourceAxes(const M4& targetDelta,const M4& sourceToTarget){

    M4 targetToSource{};

    if(!Inverse(sourceToTarget,targetToSource))return targetDelta;

    return Mul(Mul(targetToSource,targetDelta),sourceToTarget);

}

inline M4 ApplyLocalDelta(const M4& delta,const M4& bind_local){

    // A bone's own rotation must not rotate its head offset. Parent motion moves

    // the head; the bone-local translation remains its bind length.

    M4 result=Mul(delta,bind_local);

    SetTranslation(result,Translation(bind_local));

    return result;

}

inline std::vector<M4> RetargetLocal(

    const SourceRig& rig,

    const std::vector<M4>& local_delta,

    const std::vector<M4>& source_to_target_parent,

    V3 rootOffset) {

    std::vector<M4> pose(rig.bones.size());

    for(std::size_t i=0;i<pose.size();++i){

        const auto& b=rig.bones[i];

        M4 d=local_delta[i];

        if(b.role.empty())d=Identity();

        M4 axis=source_to_target_parent[i];

        M4 source_delta=InSourceAxes(d,axis);

        M4 local_bind=b.bind;

        if(b.parent>=0)local_bind=Mul(rig.bones[std::size_t(b.parent)].inverse,b.bind);

        M4 local=ApplyLocalDelta(source_delta,local_bind);

        if(b.parent<0){

            pose[i]=local;

            SetTranslation(pose[i],Add(Translation(local),rootOffset));

        }else{

            pose[i]=Mul(pose[std::size_t(b.parent)],local);

        }

    }

    return pose;

}

inline std::vector<M4> Retarget(const SourceRig& rig,const std::vector<M4>& delta,const std::vector<M4>& alignment,V3 rootOffset){

    std::vector<M4> pose(rig.bones.size());

    for(std::size_t i=0;i<pose.size();++i){const auto& b=rig.bones[i];M4 d=delta[i];

        if(b.role.empty()&&b.parent>=0){d=Mul(pose[b.parent],rig.bones[b.parent].inverse);SetTranslation(d,{0,0,0});}

        // Alignment maps source limb axes to target limb axes. Conjugate the

        // target motion into source axes; never rotate the source bind pose.

        pose[i]=Mul(InSourceAxes(d,alignment[i]),b.bind);

        auto p=Translation(b.bind);

        if(b.parent<0)p=Add(p,rootOffset);else p=Point(pose[b.parent],Point(rig.bones[b.parent].inverse,p));

        SetTranslation(pose[i],p);

    }return pose;

}

using Q4=std::array<double,4>;

inline Q4 QIdentity(){return {0,0,0,1};}

inline Q4 QAdd(Q4 a,const Q4& b,double weight){for(int i=0;i<4;++i)a[i]+=b[i]*weight;return a;}

inline Q4 QScale(Q4 a,double weight){for(auto& v:a)v*=weight;return a;}

inline double QDot(const Q4& a,const Q4& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];}

inline Q4 QConjugate(const Q4& q){return {-q[0],-q[1],-q[2],q[3]};}

inline Q4 QMul(const Q4& a,const Q4& b){

    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],

            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],

            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],

            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};

}

inline Q4 QNormalize(Q4 q){const double n=std::sqrt(QDot(q,q));if(n>1e-12)for(auto& v:q)v/=n;else q=QIdentity();return q;}

inline Q4 QuaternionFromRotation(const M4& m){

    const double trace=m[0]+m[5]+m[10];Q4 q{};

    if(trace>0){const double s=std::sqrt(trace+1.0)*2.0;q[3]=.25*s;q[0]=(m[9]-m[6])/s;q[1]=(m[2]-m[8])/s;q[2]=(m[4]-m[1])/s;}

    else if(m[0]>m[5]&&m[0]>m[10]){const double s=std::sqrt(1.0+m[0]-m[5]-m[10])*2.0;q[3]=(m[9]-m[6])/s;q[0]=.25*s;q[1]=(m[1]+m[4])/s;q[2]=(m[2]+m[8])/s;}

    else if(m[5]>m[10]){const double s=std::sqrt(1.0+m[5]-m[0]-m[10])*2.0;q[3]=(m[2]-m[8])/s;q[0]=(m[1]+m[4])/s;q[1]=.25*s;q[2]=(m[6]+m[9])/s;}

    else{const double s=std::sqrt(1.0+m[10]-m[0]-m[5])*2.0;q[3]=(m[4]-m[1])/s;q[0]=(m[2]+m[8])/s;q[1]=(m[6]+m[9])/s;q[2]=.25*s;}

    return QNormalize(q);

}

struct DualQuat{Q4 real=QIdentity(),dual{0,0,0,0};};

inline DualQuat DualFromMatrix(const M4& matrix){

    DualQuat result;result.real=QuaternionFromRotation(Rotation(matrix));

    const auto t=Translation(matrix);result.dual=QScale(QMul(Q4{t[0],t[1],t[2],0},result.real),.5);return result;

}

inline DualQuat DualNormalize(DualQuat q){

    const double n=std::sqrt(QDot(q.real,q.real));

    if(n<=1e-12)return {};

    for(auto& v:q.real)v/=n;for(auto& v:q.dual)v/=n;

    const double projection=QDot(q.real,q.dual);for(int i=0;i<4;++i)q.dual[i]-=q.real[i]*projection;return q;

}

inline V3 QRotate(const Q4& q,const V3& v){

    const Q4 p{v[0],v[1],v[2],0};const auto r=QMul(QMul(q,p),QConjugate(q));return {r[0],r[1],r[2]};

}

inline V3 DualPoint(const DualQuat& q,const V3& v){

    const auto rotated=QRotate(q.real,v);const auto t=QScale(QMul(q.dual,QConjugate(q.real)),2.0);return Add(rotated,{t[0],t[1],t[2]});

}

inline V3 DualVector(const DualQuat& q,const V3& v){return QRotate(q.real,v);}

inline void RecalculateDeformedNormals(EngineSection& out){

    std::vector<V3> accumulated(out.positions.size(),V3{});

    for(std::size_t i=0;i+2<out.indices.size();i+=3){

        const auto ia=out.indices[i],ib=out.indices[i+1],ic=out.indices[i+2];

        if(ia<0||ib<0||ic<0||std::size_t(ia)>=out.positions.size()||std::size_t(ib)>=out.positions.size()||std::size_t(ic)>=out.positions.size())continue;

        const auto face=Cross(Sub(out.positions[std::size_t(ib)],out.positions[std::size_t(ia)]),Sub(out.positions[std::size_t(ic)],out.positions[std::size_t(ia)]));

        if(Dot(face,face)<=1e-20)continue;

        for(const auto index:{ia,ib,ic})for(int axis=0;axis<3;++axis)accumulated[std::size_t(index)][axis]+=face[axis];

    }

    for(std::size_t i=0;i<out.normals.size()&&i<accumulated.size();++i){

        if(Dot(accumulated[i],accumulated[i])>1e-20)out.normals[i]=Normalize(accumulated[i]);

    }

}

inline void DeformDualQuaternion(const ModelSection& s,const SourceRig& rig,const std::vector<M4>& pose,double scale,double yaw,EngineSection& out){

    std::vector<DualQuat> palette(pose.size());for(std::size_t b=0;b<pose.size();++b)palette[b]=DualFromMatrix(Mul(pose[b],rig.bones[b].inverse));

    const double a=yaw*3.14159265358979323846/180,c=std::cos(a),sn=std::sin(a);

    for(std::size_t i=0;i<s.positions.size();++i){

        const auto v=s.positions[i],n=s.normals[i];const V3 p{-v[1],-v[0],v[2]},q{-n[1],-n[0],n[2]},tangent{1,0,0};

        Q4 real{0,0,0,0},dual{0,0,0,0},reference{};bool have_reference=false;double weight_sum=0;

        for(int k=0;k<4;++k){const double w=s.weights[i][k];if(w<=0)continue;const auto b=s.bone_indices[i][k];auto dq=palette[b];

            if(!have_reference){reference=dq.real;have_reference=true;}else if(QDot(dq.real,reference)<0){dq.real=QScale(dq.real,-1);dq.dual=QScale(dq.dual,-1);}

            real=QAdd(real,dq.real,w);dual=QAdd(dual,dq.dual,w);weight_sum+=w;}

        DualQuat blended{};blended.real=real;blended.dual=dual;if(weight_sum>1e-12){blended.real=QScale(blended.real,1.0/weight_sum);blended.dual=QScale(blended.dual,1.0/weight_sum);}blended=DualNormalize(blended);

        auto dp=DualPoint(blended,p);auto dn=Normalize(DualVector(blended,q));auto dt=Normalize(DualVector(blended,tangent));

        out.positions[i]={(c*dp[0]-sn*dp[1])*scale,(sn*dp[0]+c*dp[1])*scale,dp[2]*scale};

        out.normals[i]={c*dn[0]-sn*dn[1],sn*dn[0]+c*dn[1],dn[2]};out.tangents[i]={c*dt[0]-sn*dt[1],sn*dt[0]+c*dt[1],dt[2],0};

    }

    // Per-bone normal rotation is correct for rigid patches but can show a false

    // crease where adjacent vertices have different blends. Match the actual

    // deformed triangles so the renderer does not turn that lighting seam into

    // a fake dent.

    RecalculateDeformedNormals(out);

}

inline void Deform(const ModelSection& s,const SourceRig& rig,const std::vector<M4>& pose,double scale,double yaw,EngineSection& out){

    std::vector<M4> palette(pose.size());for(std::size_t b=0;b<pose.size();++b)palette[b]=Mul(pose[b],rig.bones[b].inverse);

    const double a=yaw*3.14159265358979323846/180,c=std::cos(a),sn=std::sin(a);

    for(std::size_t i=0;i<s.positions.size();++i){const auto v=s.positions[i],n=s.normals[i];const V3 p{-v[1],-v[0],v[2]},q{-n[1],-n[0],n[2]},tangent{1,0,0};V3 dp{},dn{},dt{};

        for(int k=0;k<4;++k){double w=s.weights[i][k];if(w==0)continue;auto b=s.bone_indices[i][k];auto tp=Point(palette[b],p),tn=Vector(palette[b],q),tt=Vector(palette[b],tangent);for(int j=0;j<3;++j){dp[j]+=w*tp[j];dn[j]+=w*tn[j];dt[j]+=w*tt[j];}}

        out.positions[i]={(c*dp[0]-sn*dp[1])*scale,(sn*dp[0]+c*dp[1])*scale,dp[2]*scale};dn=Normalize(dn);dt=Normalize(dt);out.normals[i]={c*dn[0]-sn*dn[1],sn*dn[0]+c*dn[1],dn[2]};out.tangents[i]={c*dt[0]-sn*dt[1],sn*dt[0]+c*dt[1],dt[2],0};

    }

}

} // namespace nailong
