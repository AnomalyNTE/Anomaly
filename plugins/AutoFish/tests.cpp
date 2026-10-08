#include "flow.hpp"
#include "hud_map.hpp"
#include "cast_point.hpp"
#include "restock.hpp"
#include "sell_policy.hpp"
#include <iostream>
#include <stdexcept>
using fishing::Action;
void Check(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
int main() {
    try {
        fishing::SellPolicy sale;
        Check(sale.Next(true,100,99,0,true,1000)==fishing::SellAction::None,"do not sell below the warehouse threshold");
        Check(sale.Next(true,100,100,0,false,1000)==fishing::SellAction::None,"do not sell during active fishing");
        Check(sale.Next(true,100,100,100,true,1000)==fishing::SellAction::None,"an all-locked warehouse never sends a sell request");
        Check(sale.Next(false,100,100,0,true,1000)==fishing::SellAction::None,"disabled auto sell never sends a request");
        Check(sale.Next(true,100,100,10,true,1000)==fishing::SellAction::Send,"sell when the threshold is reached and unlocked fish exist");
        sale.Begin(100,1000);sale.Retry();
        Check(sale.Next(true,1,100,10,true,2000)==fishing::SellAction::Wait,"changing the threshold cannot duplicate an in-flight sale");
        Check(sale.Next(false,100,10,10,true,2500)==fishing::SellAction::Received,"reconcile a completed sale even after switching auto sell off");
        Check(sale.Next(true,1,15,10,true,3000)==fishing::SellAction::None,"allow inventory replication to settle after selling");
        sale.Begin(15,6000);
        Check(sale.Next(true,1,15,10,true,16000)==fishing::SellAction::Timeout &&
            sale.Next(true,1,15,10,true,17000)==fishing::SellAction::None,"an unanswered sale is not sent repeatedly");
        sale.Retry();Check(sale.Next(true,1,15,10,true,18000)==fishing::SellAction::Send,"explicit retry unlocks a failed sale");
        int64_t cost{};
        Check(fishing::PurchaseCost(100,20,cost) && cost==2000,"purchase cost matches the selected batch");
        Check(!fishing::PurchaseCost(100,0,cost) && !fishing::PurchaseCost(INT64_MAX,2,cost),"reject zero quantities and overflowing prices");
        fishing::Restock purchase;
        Check(purchase.Begin(1000,0) && !purchase.Begin(1001,0),"send only one purchase while waiting for inventory");
        Check(!purchase.Received(0) && !purchase.Expired(10999),"an unchanged inventory cannot acknowledge a purchase");
        purchase.Retry();Check(!purchase.Begin(2000,0),"setting changes cannot duplicate an in-flight purchase");
        Check(purchase.Received(20) && !purchase.pending,"actual received inventory completes the purchase");
        Check(purchase.Begin(12000,0) && purchase.Expired(22000) && !purchase.Begin(23000,0),"a timed-out purchase is latched and never resent automatically");
        purchase.Retry();Check(purchase.Begin(24000,0),"an explicit retry can unlock a failed restock");
        fishing::Flow f;
        Check(fishing::EventPhase("Server_TryThrowRod")==-1 && fishing::EventPhase("Client_StartThrowRod")==2 && fishing::EventPhase("Client_HasFishBait")==4 &&
            fishing::EventPhase("ClientPlayerSureFishing")==5 && fishing::EventPhase("Client_ThrowUpResult")==0 &&
            fishing::EventPhase("UnrelatedEvent")==-1,"real fishing callbacks drive phases without guessing stale player fields");
        CastPoint point{};point.point={100,200,300};std::memcpy(point.world.data(),"test",4);
        Check(point.Valid("test",{0,0,0}) && !point.Valid("elsewhere",{0,0,0}) && !point.Valid("test",{10000,0,0}),
            "recorded casts stay in their original world and local fishing area");
        Check(f.Next(0,false,false,true,1000)==Action::None,"outside fishing no actions");
        Check(f.Next(1,true,false,true,2000)==Action::None,"allow aiming interface to settle");
        Check(f.Next(1,true,false,true,2700)==Action::Cast,"cast in selection phase");f.Sent(2700);
        Check(f.Next(1,true,false,true,2800)==Action::None,"do not flood cast requests");
        Check(f.Next(3,true,false,true,3000)==Action::None,"wait for a real bite");
        f.Next(4,true,false,true,4000);Check(f.Next(4,true,false,true,4150)==Action::Hook,"hook after bite");f.Sent(4150);
        Check(f.Next(4,true,false,true,5000)==Action::None,"one hook per phase");
        f.Next(5,true,false,true,6000);Check(f.Next(5,true,false,true,7200)==Action::Pull,"use directional pulling during the minigame");
        Check(f.Next(5,true,false,true,17200)==Action::Pull,"continue adjusting while fish struggles");
        Check(f.Next(5,true,false,true,186000)==Action::Stop,"stop a stalled minigame");
        Check(fishing::LockDelta(100,0,320,0.02F)*320<6.5 && fishing::LockDelta(0,100,320,0.02F)>0,
            "large tracking errors cannot teleport the rod beyond normal movement per frame");
        Check(fishing::LockDelta(100,0,0,0.02F)==0 && fishing::LockDelta(10000,0,320,3)<=0.05F,
            "invalid movement speed is rejected and stalls cannot produce a large catch-up jump");
        Check(std::abs(fishing::LockDelta(3,0,320,0.02F)*320-3)<0.001,
            "small tracking errors settle precisely without overshooting");
        Check(fishing::MarkerCenter(195.0,150.0,0.0)==270.0 && fishing::MarkerCenter(270.0,33.8,0.5)==270.0,
            "left-aligned green region and center-aligned rod are compared by their visual centers");
        HudMapHeader map{};map.data=0x10000;map.count=10;map.capacity=16;map.bit_count=10;map.bit_capacity=128;map.first_free=0;
        Check(map.Valid(),"sparse HUD map bitmap length is separate from first-free index");
        map.bit_count=9;Check(!map.Valid(),"reject truncated HUD allocation bitmap");
        map.count=129;map.capacity=256;map.bit_count=129;map.bit_capacity=256;
        Check(!map.Valid(),"large HUD maps require an external allocation bitmap");
        f.Reset();f.Next(0,false,true,true,1000);
        Check(f.Next(0,false,true,true,8000,false)==Action::None && f.attempts==0,
            "a catch cutscene without a result widget cannot spend continuation retries");
        Check(f.Next(0,false,true,true,9000,true)==Action::Continue,
            "the real result card enables continuation after a delayed cutscene");
        f.Reset();f.Next(0,false,true,true,1000,false);
        Check(f.Next(0,false,true,true,31000,false)==Action::Stop,
            "a permanently missing result card remains bounded");
        f.Reset();f.Next(0,false,true,true,1000);
        Check(f.Next(0,false,true,true,2200)==Action::Continue,"continue after result settles");f.Sent(2200);
        Check(f.Next(0,false,true,false,2300)==Action::Stop,"single fish mode stops at result");
        f.Reset();Check(f.Next(99,true,false,true,1000)==Action::Stop,"unknown fishing phase cannot send requests");
        f.Next(1,false,false,true,2000);Check(f.Next(1,false,false,true,4000)==Action::None,"hidden interface cannot cast");
        f.Reset();f.Next(1,true,false,true,1000);
        for(int i=0;i<3;++i) {auto t=1700+2500*i;Check(f.Next(1,true,false,true,t)==Action::Cast,"bounded cast retries");f.Sent(t);}
        Check(f.Next(1,true,false,true,9200)==Action::Stop,"bad casting point stops instead of spending indefinitely");
        std::cout<<"Fishing transitions and request limits passed.\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
