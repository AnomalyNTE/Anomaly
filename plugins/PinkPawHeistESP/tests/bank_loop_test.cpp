// Include the plugin entry unit to exercise its actual settings migration and
// pickup lifecycle boundaries, without introducing production test accessors.
#include "../plugin.cpp"
#include "../bank_intro_skip.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace pink_paw_heist_esp;
using namespace std::chrono_literals;

namespace {
const auto origin = BankLoop::Clock::time_point{};
void Check(const bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
BankLoopObservation Ready(const bool in_bank) {
    BankLoopObservation o;
    o.world_ready = o.bank_known = true;
    o.in_bank = in_bank;
    o.loot_ready = true;
    o.loot_present = true;
    o.loot_revision = 1;
    return o;
}
void ReachExtraction(BankLoop& loop, BankLoopObservation& o) {
    loop.Start(origin);
    o = Ready(true);
    Check(loop.Tick(o, origin) == BankLoopAction::none, "Joining an existing bank must collect first");
    Check(loop.Tick(o, origin + 3s) == BankLoopAction::refresh_loot, "An empty observation needs a refresh");
    Check(loop.Tick(o, origin + 4s) == BankLoopAction::none, "The pre-refresh cache cannot authorize departure");
    ++o.loot_revision;
    Check(loop.Tick(o, origin + 5s) == BankLoopAction::none, "A completed refresh confirms the empty bank");
    Check(loop.Phase() == BankLoopPhase::waiting_extraction, "Must find an active extraction after confirmation");
}
}

void RunBankIntroRuntimeTests();

int main() {
    RunBankIntroRuntimeTests();
    {
        BankIntroSkip intro;
        Check(!intro.Ready(origin + 1h), "No bank playback can authorize a cinematic finish");
        intro.Observe(17, origin);
        Check(!intro.Ready(origin + 1999ms), "Allow the observed intro presentation to settle for two seconds");
        Check(intro.Ready(origin + 2s), "A settled bank intro can use its dialogue skip handler");
        intro.Dispatched();
        intro.Observe(17, origin + 3s);
        Check(!intro.Ready(origin + 10s), "An unchanged playback cannot repeat skipping");
        intro.Observe(0, origin + 11s);
        Check(!intro.Ready(origin + 1h), "Other sequences or stopped playback cannot own bank skipping");
        intro.Observe(17, origin + 12s);
        Check(intro.Ready(origin + 14s), "A new playback may reuse the same sequence actor");
        intro.Observe(18, origin + 15s);
        Check(!intro.Ready(origin + 16s) && intro.Ready(origin + 17s),
            "A different intro must receive its own presentation interval");
        intro.Reset();
        Check(!intro.Ready(origin + 1h), "Stopping/restarting clears the previous playback action");
    }
    {
        constexpr std::string_view saved_v11 = R"({"alwaysShowAccessCards":true,"autoTeleportPickup":false,"autoTeleportPickupDelayMs":350,"drawExtractions":true,"drawLootBoxes":false,"enabled":false,"menuOpen":true,"minimumValue":1000,"showActiveExtractionsOnly":true,"showPickableOnly":true,"teleportZOffset":0,"websocketEnabled":true,"websocketPort":14514})";
        Settings settings;
        Check(SettingsDocumentReader(saved_v11).Read(settings, 11), "The existing v11 settings must migrate");
        Check(settings.bank_loop_rounds == 0 && settings.bank_loop_skip_cutscenes &&
            settings.minimum_value == 1000 && settings.auto_teleport_pickup_delay_ms == 350,
            "Migration must preserve filters/delay and supply loop defaults");
        settings.bank_loop_rounds = 7;
        settings.bank_loop_skip_cutscenes = false;
        ApplySettings(settings);
        const auto serialized = SerializeSettings();
        Settings restored;
        Check(SettingsDocumentReader(serialized).Read(restored, 12) && restored.bank_loop_rounds == 7 &&
            !restored.bank_loop_skip_cutscenes && restored.minimum_value == 1000 &&
            restored.auto_teleport_pickup_delay_ms == 350,
            "Loop options and old settings must survive serialization");
        Check(!SettingsDocumentReader(saved_v11).Read(restored, 12),
            "The new schema must reject missing loop options");
    }
    {
        BankLoop loop;
        auto o = Ready(false);
        loop.Start(origin, 2);
        auto now = origin;
        for (std::uint32_t round = 1; round <= 2; ++round) {
            Check(loop.Tick(o, now) == BankLoopAction::enter, "Each requested round must enter normally");
            Check(loop.SkippingCutscenes(), "Entry permits cutscene skipping");
            o = Ready(true);
            static_cast<void>(loop.Tick(o, now += 1s));
            Check(loop.Collecting() && loop.SkippingCutscenes(),
                "An entry cinema can still show its skip button after loot is ready");
            Check(loop.Tick(o, now += 3s) == BankLoopAction::refresh_loot, "Confirm empty filtered loot");
            ++o.loot_revision;
            static_cast<void>(loop.Tick(o, now += 1s));
            o.extraction_available = true;
            Check(loop.Tick(o, now += 1s) == BankLoopAction::teleport_extraction, "Reach the extraction");
            o.at_extraction = true;
            Check(loop.Tick(o, now += 1s) == BankLoopAction::evacuate, "Request normal evacuation");
            Check(loop.SkippingCutscenes() && loop.Rounds() == round - 1,
                "Evacuation animation permits skipping but is not a completed round");
            o.world_ready = false;
            o.in_bank = false;
            static_cast<void>(loop.Tick(o, now += 1s));
            Check(loop.Rounds() == round - 1, "Loading must not consume a requested round");
            o = Ready(false);
            static_cast<void>(loop.Tick(o, now += 1s));
            Check(loop.Rounds() == round && loop.RoundLimit() == 2, "Only confirmed returns count");
            now += 8s;
        }
        Check(loop.Phase() == BankLoopPhase::completed && !loop.Running() && !loop.SkippingCutscenes(),
            "The last requested round must stop both automation and skipping");
        Check(loop.Tick(o, now + 1h) == BankLoopAction::none && loop.Rounds() == 2,
            "A finite run must never enter an extra bank");
        loop.Start(now, 1);
        Check(loop.Rounds() == 0 && loop.RoundLimit() == 1 && loop.Running(), "Restart must accept a fresh limit");
        loop.Stop();
        Check(!loop.SkippingCutscenes(), "Stopping must disable cinematic actions");
    }
    {
        // A stable list used to suppress all further full preparation after
        // streaming replaced the world/controller. An explicit recovery request
        // must reopen collection even while the normal refresh timer is pending.
        LootRefreshPolicy refresh(10s);
        refresh.Complete(origin, true, true);
        Check(!refresh.Begin(origin + 1s, false), "Stable loot should avoid redundant scans");
        Check(refresh.Begin(origin + 1s, true), "Unavailable pickup context must allow a forced preparation refresh");
        refresh.Complete(origin + 1s, false, true);
        Check(refresh.Begin(origin + 11s, false), "Recovery must allow the list to settle again");

        BankLoop loop;
        loop.Start(origin);
        auto o = Ready(true);
        o.loot_ready = false;
        o.has_targets = o.has_unattempted_targets = true;
        static_cast<void>(loop.Tick(o, origin));
        static_cast<void>(loop.Tick(o, origin + 30s));
        Check(loop.Phase() == BankLoopPhase::waiting_loot,
            "A populated list without the current pickup context must wait");
        o.loot_ready = true;
        static_cast<void>(loop.Tick(o, origin + 31s));
        Check(loop.Collecting(), "Rebinding the pickup context must resume collection");
        o.pickup_busy = true;
        static_cast<void>(loop.Tick(o, origin + 100s));
        Check(loop.Collecting() && loop.Failure() == BankLoopFailure::none,
            "Recovery must not inherit the loading deadline");
    }
    {
        struct Entry {
            AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
            RobBankInspection rob_bank;
        };
        // Observed through the plugin's live loot-snapshot output: pickable
        // coin variants with no resolved price coexist with priced bank loot.
        // Their metadata must not impose an all-items-ready barrier.
        std::array<Entry, 2> entries;
        for (auto& entry : entries) {
            entry.snapshot.flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
            entry.rob_bank.entity = {117454, 1};
            entry.rob_bank.pickability = RobBankPickability::candidate;
        }
        entries[1].rob_bank.item_resolved = true;
        entries[1].rob_bank.fons_value = 50000;
        const auto is_target = [](const Entry& entry) {
            return entry.rob_bank.is_access_card ||
                (entry.rob_bank.item_resolved && entry.rob_bank.fons_value >= 999);
        };
        const auto attempted = [](RobBankEntity) { return false; };
        const auto observed = ObserveBankLoopLoot(entries, true, true, false, is_target, attempted);
        Check(observed.ready && observed.present && observed.has_unattempted_targets &&
            observed.observed_count == 2 && observed.ready_count == 1 && observed.target_count == 1,
            "An unresolved pickable item cannot block an identified pickup target");
        BankLoop loop;
        loop.Start(origin);
        auto o = Ready(true);
        o.loot_ready = observed.ready;
        o.loot_present = observed.present;
        o.has_targets = observed.has_targets;
        o.has_unattempted_targets = observed.has_unattempted_targets;
        static_cast<void>(loop.Tick(o, origin));
        Check(loop.Collecting(), "The loop must collect a ready target from a mixed metadata frame");

        const auto loading = ObserveBankLoopLoot(entries, true, false, false, is_target, attempted);
        Check(!loading.ready, "The inspection context is still required");
        entries[1].rob_bank.item_resolved = false;
        const auto unknown = ObserveBankLoopLoot(entries, true, true, false, is_target, attempted);
        Check(unknown.ready && !unknown.present && !unknown.has_targets,
            "Unidentified loot alone cannot prove an initialized empty bank");
        loop.Start(origin);
        o.loot_ready = unknown.ready;
        o.loot_present = unknown.present;
        o.has_targets = unknown.has_targets;
        o.has_unattempted_targets = unknown.has_unattempted_targets;
        static_cast<void>(loop.Tick(o, origin));
        Check(loop.Tick(o, origin + 15s) == BankLoopAction::none &&
            loop.Phase() == BankLoopPhase::waiting_loot,
            "Relaxing per-item metadata must not restore premature evacuation");
        entries[1].rob_bank.is_access_card = true;
        const auto card = ObserveBankLoopLoot(entries, true, true, false, is_target, attempted);
        Check(card.ready && card.present && card.target_count == 0 &&
            !card.has_targets && card.has_unattempted_targets,
            "A card must be attempted without being counted as minimum-value loot");
    }
    {
        struct Entry {
            AnomalyNteEntitySnapshotV1 snapshot{sizeof(snapshot)};
            RobBankInspection rob_bank;
        };
        // Live failure: the last targets were zero-value KeyCard actors which
        // stayed pickable after requests; no priced loot met the value filter.
        std::array<Entry, 2> entries;
        for (std::size_t i{}; i < entries.size(); ++i) {
            entries[i].snapshot.flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
            entries[i].rob_bank.entity = {static_cast<std::uint32_t>(315708 + i), 1604708};
            entries[i].rob_bank.pickability = RobBankPickability::candidate;
            entries[i].rob_bank.item_resolved = true;
            entries[i].rob_bank.is_access_card = true;
        }
        const auto target = [](const Entry& entry) {
            return entry.rob_bank.is_access_card || entry.rob_bank.fons_value >= 1000;
        };
        const auto not_attempted = [](RobBankEntity) { return false; };
        const auto all_attempted = [](RobBankEntity) { return true; };
        const auto apply = [](BankLoopObservation& o, const BankLoopLootObservation& loot) {
            o.loot_ready = loot.ready;
            o.loot_present = loot.present;
            o.has_targets = loot.has_targets;
            o.has_unattempted_targets = loot.has_unattempted_targets;
        };
        BankLoop loop;
        loop.Start(origin);
        auto o = Ready(true);
        apply(o, ObserveBankLoopLoot(entries, true, true, false, target, not_attempted));
        static_cast<void>(loop.Tick(o, origin));
        Check(loop.Tick(o, origin + 4s) == BankLoopAction::none && loop.Collecting(),
            "Unattempted cards must still be collected before departure");
        apply(o, ObserveBankLoopLoot(entries, true, true, false, target, all_attempted));
        Check(loop.Tick(o, origin + 7s) == BankLoopAction::refresh_loot,
            "Persistent attempted cards cannot prevent empty-loot confirmation");
        // A newly observed card during confirmation must get its attempt too.
        apply(o, ObserveBankLoopLoot(entries, true, true, false, target, not_attempted));
        Check(loop.Tick(o, origin + 8s) == BankLoopAction::none && loop.Collecting(),
            "New supporting cards must cancel pending departure");
        apply(o, ObserveBankLoopLoot(entries, true, true, false, target, all_attempted));
        Check(loop.Tick(o, origin + 11s) == BankLoopAction::refresh_loot,
            "Attempting the new cards must allow confirmation again");
        ++o.loot_revision;
        static_cast<void>(loop.Tick(o, origin + 12s));
        Check(loop.Phase() == BankLoopPhase::waiting_extraction,
            "An unchanged card-only snapshot must allow finding an exit");
        o.extraction_available = true;
        Check(loop.Tick(o, origin + 13s) == BankLoopAction::teleport_extraction,
            "Attempted cards must not send the loop back to collection");
        o.at_extraction = true;
        Check(loop.Tick(o, origin + 15s) == BankLoopAction::evacuate,
            "Card-only leftovers must permit the normal evacuation request");

        // A normal valued item left after a successful request still blocks exit.
        entries[1].rob_bank.is_access_card = false;
        entries[1].rob_bank.fons_value = 50000;
        const auto remaining = ObserveBankLoopLoot(entries, true, true, false, target, all_attempted);
        Check(remaining.has_targets && remaining.target_count == 1 &&
            !remaining.has_unattempted_targets,
            "Only supporting cards can be ignored after an attempt, never valued loot");
        loop.Start(origin);
        apply(o, remaining);
        static_cast<void>(loop.Tick(o, origin));
        static_cast<void>(loop.Tick(o, origin + 8s));
        Check(loop.Failure() == BankLoopFailure::pickup_unconfirmed && loop.Rounds() == 0,
            "A valued item still pickable must stop before evacuation");
    }
    {
        BankLoop loop;
        auto o = Ready(false);
        o.loot_present = false;
        o.extraction_available = true;
        loop.Start(origin);
        Check(loop.Tick(o, origin) == BankLoopAction::enter, "Request initial bank entry");
        o.in_bank = true;
        Check(loop.Tick(o, origin + 1s) == BankLoopAction::none, "Extraction appears before bank loot");
        for (const auto delay : {4s, 15s, 30s}) {
            ++o.loot_revision;
            Check(loop.Tick(o, origin + delay) == BankLoopAction::none &&
                loop.Phase() == BankLoopPhase::waiting_loot && loop.Rounds() == 0,
                "Empty early scans with an active exit must wait for actual loot");
        }
        o.loot_present = true;
        o.has_targets = o.has_unattempted_targets = true;
        Check(loop.Tick(o, origin + 35s) == BankLoopAction::none && loop.Collecting(),
            "Collection starts when the delayed BankBoxes are observed");
        static_cast<void>(loop.Tick(o, origin + 36s));
        o.has_targets = o.has_unattempted_targets = false;
        o.loot_present = false;
        Check(loop.Tick(o, origin + 39s) == BankLoopAction::refresh_loot,
            "After actual collection an empty bank can be confirmed");
        ++o.loot_revision;
        static_cast<void>(loop.Tick(o, origin + 40s));
        Check(loop.Tick(o, origin + 41s) == BankLoopAction::teleport_extraction,
            "The active exit becomes usable after real loot was collected");
        o.at_extraction = true;
        Check(loop.Tick(o, origin + 43s) == BankLoopAction::evacuate, "Evacuate the collected round");
        o.in_bank = false;
        static_cast<void>(loop.Tick(o, origin + 45s));
        Check(loop.Tick(o, origin + 53s) == BankLoopAction::enter, "Request the next round");
        o.in_bank = true;
        static_cast<void>(loop.Tick(o, origin + 54s));
        Check(loop.Tick(o, origin + 80s) == BankLoopAction::none &&
            loop.Phase() == BankLoopPhase::waiting_loot,
            "Every new round must independently wait for its own loot");
    }
    {
        BankLoop loop;
        auto o = Ready(false);
        loop.Start(origin);
        Check(loop.Tick(o, origin) == BankLoopAction::enter, "Outside start must request entry");
        Check(loop.Tick(o, origin + 5s) == BankLoopAction::none, "Do not flood entry requests");
        o.bank_known = false;
        Check(loop.Tick(o, origin + 20s) == BankLoopAction::none && loop.Rounds() == 0,
            "Loading must not count as a round");
        o = Ready(true);
        Check(loop.Tick(o, origin + 25s) == BankLoopAction::none && loop.Collecting(),
            "Collection starts after bank entry is observed");
        o.has_targets = o.has_unattempted_targets = true;
        Check(loop.Tick(o, origin + 30s) == BankLoopAction::none, "Remaining filtered loot prevents departure");
        o.has_targets = o.has_unattempted_targets = false;
        o.pickup_busy = true;
        Check(loop.Tick(o, origin + 40s) == BankLoopAction::none, "A pickup in flight prevents departure");
        o.pickup_busy = false;
        Check(loop.Tick(o, origin + 43s) == BankLoopAction::refresh_loot, "Refresh after the final pickup settles");
        o.has_targets = o.has_unattempted_targets = true;
        ++o.loot_revision;
        Check(loop.Tick(o, origin + 44s) == BankLoopAction::none && loop.Collecting(),
            "A newly unlocked target cancels departure");
    }
    {
        BankLoop loop;
        BankLoopObservation o;
        ReachExtraction(loop, o);
        Check(loop.Tick(o, origin + 6s) == BankLoopAction::none, "Inactive exits cannot be used");
        o.extraction_available = true;
        Check(loop.Tick(o, origin + 7s) == BankLoopAction::teleport_extraction, "Travel to an active exit");
        Check(loop.Tick(o, origin + 9s) == BankLoopAction::none, "An issued teleport is not arrival");
        o.at_extraction = true;
        Check(loop.Tick(o, origin + 10s) == BankLoopAction::evacuate, "Evacuate after confirmed arrival");
        Check(loop.Tick(o, origin + 11s) == BankLoopAction::none, "Do not repeat the evacuation request");
        o.world_ready = false;
        o.in_bank = false;
        Check(loop.Tick(o, origin + 15s) == BankLoopAction::none && loop.Rounds() == 0,
            "A loading screen is not confirmed evacuation");
        o = Ready(false);
        Check(loop.Tick(o, origin + 20s) == BankLoopAction::none && loop.Rounds() == 1,
            "Only returning outside completes a round");
        Check(loop.Tick(o, origin + 27s) == BankLoopAction::none, "Wait between rounds");
        Check(loop.Tick(o, origin + 28s) == BankLoopAction::enter, "Begin the next bank round");
        loop.Stop();
        Check(loop.Tick(o, origin + 50s) == BankLoopAction::none && !loop.Running(), "Stop cancels future entry");
    }
    {
        BankLoop loop;
        BankLoopObservation o;
        ReachExtraction(loop, o);
        Check(loop.Tick(o, origin + 66s) == BankLoopAction::none && loop.Phase() == BankLoopPhase::failed,
            "No active extraction must stop without pretending a round succeeded");
        Check(loop.Rounds() == 0, "A failure cannot increment the completed count");
        Check(loop.Failure() == BankLoopFailure::extraction_unavailable,
            "A missing extraction needs a specific failure reason");
    }
    {
        BankLoop loop;
        auto o = Ready(true);
        loop.Start(origin);
        static_cast<void>(loop.Tick(o, origin));
        o.has_targets = o.has_unattempted_targets = true;
        static_cast<void>(loop.Tick(o, origin + 1s));
        o.has_unattempted_targets = false;
        Check(loop.Tick(o, origin + 4s) == BankLoopAction::none && loop.Collecting(),
            "Wait for server pickup confirmation");
        static_cast<void>(loop.Tick(o, origin + 9s));
        Check(loop.Phase() == BankLoopPhase::failed && loop.Rounds() == 0,
            "Uncollected attempted loot must stop, not be treated as an empty bank");
        Check(loop.Failure() == BankLoopFailure::pickup_unconfirmed,
            "Unconfirmed pickup must not be reported as a generic game timeout");
    }
    {
        BankLoop loop;
        auto o = Ready(true);
        loop.Start(origin);
        static_cast<void>(loop.Tick(o, origin));
        o.in_bank = false;
        static_cast<void>(loop.Tick(o, origin + 1s));
        Check(loop.Phase() == BankLoopPhase::failed && loop.Rounds() == 0,
            "Manual exit or death must not start another round");
        loop.Start(origin + 2s);
        Check(loop.Tick(o, origin + 2s) == BankLoopAction::enter && loop.Rounds() == 0,
            "Explicit restart clears the previous run");
        Check(loop.Failure() == BankLoopFailure::none, "Restart must clear the failure reason");
    }
    {
        BankLoop loop;
        loop.Start(origin);
        auto o = Ready(true);
        o.world_ready = false;
        static_cast<void>(loop.Tick(o, origin + 90s));
        Check(loop.Failure() == BankLoopFailure::world_unavailable,
            "An unavailable player/world must be distinguishable from missing loot");
        loop.Start(origin);
        o.world_ready = true;
        o.loot_ready = false;
        static_cast<void>(loop.Tick(o, origin));
        static_cast<void>(loop.Tick(o, origin + 90s));
        Check(loop.Failure() == BankLoopFailure::loot_unavailable,
            "Ready players with unavailable pickup context need a loot preparation error");
    }
    std::cout << "bank loop transitions: passed\n";
}
