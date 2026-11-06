#include "plan.hpp"
#include <cstdlib>
#include <iostream>

void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    using namespace taxi;
    Observation observed{{10,1,20,1,30,1,40,1}, {101,201,301}, Stage::Idle};
    Run run;
    Check(run.Next(observed,0)==Action::None, "idle started a taxi session without the player");
    observed.stage=Stage::Waiting;
    Check(run.Next(observed,1)==Action::None, "waiting generated an order locally");
    observed.stage=Stage::Pickup;
    Check(run.Next(observed,10)==Action::Pickup, "assigned passenger was not handled");
    run.Submitted(true,10);
    Check(run.Next(observed,1000)==Action::None && !run.failed, "pickup request was repeated before native acknowledgement");
    observed.stage=Stage::Destination;
    Check(run.Next(observed,2000)==Action::Deliver, "confirmed pickup did not enable delivery");
    run.Submitted(true,2000);
    Check(run.Next(observed,3000)==Action::None, "delivery request was repeated");
    observed.stage=Stage::SpecialEvent;
    Check(run.Next(observed,3500)==Action::None, "special event was skipped by a fabricated completion");
    observed.stage=Stage::Settle;
    Check(run.Next(observed,4000)==Action::Continue, "settlement did not use the continue action");
    run.Submitted(true,4000);
    Check(run.Next(observed,5000)==Action::None, "settlement was submitted more than once");
    observed.stage=Stage::Waiting;run.Next(observed,6000);
    observed.stage=Stage::Pickup;
    Check(run.Next(observed,7000)==Action::Pickup, "same template/points could not recur after the native order boundary");
    run.Submitted(true,7000);
    Check(run.Next(observed,21999)==Action::None && !run.failed, "order acknowledgement expired too early");
    Check(run.Next(observed,22000)==Action::None && run.failed, "rejected order was retried forever");
    observed.order[0]=102;
    Check(run.Next(observed,23000)==Action::Pickup && !run.failed, "a genuinely different order retained an old failure");
    run.Submitted(false,23000);
    Check(run.Next(observed,24000)==Action::None && run.failed, "uncertain native dispatch was retried");
    ++observed.identity[7];
    Check(run.Next(observed,25000)==Action::Pickup && !run.failed, "vehicle generation change retained an old request");
    run.Submitted(true,25000);
    observed.stage=Stage::Destination;run.Next(observed,26000);run.Submitted(true,26000);
    observed.stage=Stage::Pickup;
    Check(run.Next(observed,27000)==Action::None && run.failed, "regressed replication caused another pickup request");
    run.Reset();observed.order[1]=0;
    Check(run.Next(observed,28000)==Action::None, "incomplete point IDs reached automation");
    observed.order={101,201,301};observed.stage=static_cast<Stage>(6);
    Check(run.Next(observed,29000)==Action::None, "an unknown gameplay state was treated as a valid order");

    run.Reset();observed.order={101,201,301};observed.stage=Stage::Destination;
    Check(run.Next(observed,30000)==Action::Deliver,"event fixture could not reach delivery");run.Submitted(true,30000);
    observed.stage=Stage::SpecialEvent;Check(run.Next(observed,31000)==Action::None,"talk event emitted a delivery request");
    observed.stage=Stage::Destination;
    Check(run.Next(observed,80000)==Action::Deliver && !run.failed,"a finished event returning to delivery was treated as stale replication");
    run.Reset();observed.stage=Stage::Pickup;run.Next(observed,90000);run.Submitted(true,90000);
    run.Suspend(150000);
    Check(run.Next(observed,150500)==Action::None && !run.failed,"dialogue suspension expired an unacknowledged request");

    ConfirmedOrders count;
    count.Observe({20,1},100);
    Check(count.added==0, "historical account total was displayed as new automatic completions");
    count.Observe({20,1},100);
    Check(count.added==0, "native dispatch/UI state was confused with a completed-order increase");
    count.Observe({20,1},101);count.Observe({20,1},101);
    Check(count.added==1, "repeated replicated count was counted more than once");
    count.Observe({20,1},100);count.Observe({20,1},101);
    Check(count.added==1, "temporarily older replication caused duplicate accounting");
    count.Observe({20,1},103);
    Check(count.added==3, "native count deltas were lost");
    count.Observe({20,2},200);count.Observe({20,2},201);
    Check(count.added==4, "a reused player address counted another session's historical total");
    count.Reset();Check(!count.initialized && count.added==0, "plugin reload retained completion statistics");
    std::cout << "Taxi native order sequencing, acknowledgement and confirmed count checks passed\n";
}
