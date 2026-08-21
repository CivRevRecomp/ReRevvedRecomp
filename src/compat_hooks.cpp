// Hooks preserve title behavior unless documented as a compatibility repair.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game_state.h"

REXCVAR_DEFINE_BOOL(qol_probe_gameplay_frame, false, "ReRevved/Diagnostics", "Log the candidate playable-frame cadence and thread")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_gameplay_state, false, "ReRevved/Diagnostics", "Log conservative gameplay-state gate transitions")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_unit_movement, false, "ReRevved/Diagnostics", "Log bounded unit-move command and simulation edges")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_combat, false, "ReRevved/Diagnostics", "Log bounded combat resolution and presentation edges")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_civ_bonuses, false, "ReRevved/Diagnostics", "Log bounded civilization bonus table queries")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_unit_stats, false, "ReRevved/Diagnostics", "Compare combat and AI unit-stat table readers")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_rush_cost, false, "ReRevved/Diagnostics", "Log displayed and applied unit rush-cost arithmetic")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_save_slots, false, "ReRevved/Diagnostics", "Log selected save-slot identifiers at native load and save boundaries")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_fast_combat, false, "ReRevved/QoL", "Use the native 0.5 pace divisor on the mapped combat presentation path")
    .debug_only();

namespace
{

rex::system::IGraphicsSystem* GetGraphicsSystem()
{
    auto* kernel_state = REX_KERNEL_STATE();
    if (!kernel_state || !kernel_state->emulator())
    {
        return nullptr;
    }

    return kernel_state->emulator()->graphics_system();
}

bool IsGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

struct CheckedAddressRange
{
    uint32_t end;
    bool     valid;
};

constexpr CheckedAddressRange MakeCheckedAddressRange(uint32_t base,
                                                      uint32_t extent,
                                                      uint32_t ceiling)
{
    return extent == 0 || extent > ceiling || base > UINT32_MAX - extent
               ? CheckedAddressRange{ 0, false }
               : CheckedAddressRange{ base + extent, true };
}

bool IsGuestReadableRange(uint32_t address, uint32_t extent)
{
    const auto range = MakeCheckedAddressRange(address, extent, UINT32_MAX);
    if (!range.valid || !IsGuestPointer(address) ||
        !IsGuestPointer(range.end - 1))
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    return heap && memory->LookupHeap(range.end - 1) == heap &&
           heap->QueryRangeAccess(address, range.end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint32_t ReadGuestU32(uint32_t address)
{
    if (!IsGuestPointer(address))
    {
        return 0;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    return (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
           (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
}

bool TryReadGuestU8(uint32_t address, uint8_t& value)
{
    if (!IsGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    value              = source[0];
    return true;
}

bool TryReadGuestU32(uint32_t address, uint32_t& value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    value = ReadGuestU32(address);
    return true;
}

bool TryReadGuestF32(uint32_t address, float& value)
{
    uint32_t bits = 0;
    if (!TryReadGuestU32(address, bits))
    {
        return false;
    }
    value = std::bit_cast<float>(bits);
    return true;
}

template <size_t Size>
bool TryReadGuestText(uint32_t address, std::array<char, Size>& text)
{
    static_assert(Size > 1);
    text = {};
    if (!IsGuestReadableRange(address, Size))
    {
        return false;
    }

    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    for (size_t index = 0; index < Size - 1; ++index)
    {
        const uint8_t value = source[index];
        if (value == 0)
        {
            return index != 0;
        }
        text[index] = value >= 0x20 && value <= 0x7E
                          ? static_cast<char>(value)
                          : '?';
    }
    return false;
}

bool WriteGuestU32Safely(uint32_t address, uint32_t value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    auto* memory      = REX_KERNEL_MEMORY();
    auto* destination = memory->TranslateVirtual<uint8_t*>(address);
    destination[0]    = static_cast<uint8_t>(value >> 24);
    destination[1]    = static_cast<uint8_t>(value >> 16);
    destination[2]    = static_cast<uint8_t>(value >> 8);
    destination[3]    = static_cast<uint8_t>(value);
    return true;
}

thread_local bool resume_after_ring_initialize = false;

struct GfxRenderCapsState
{
    uint32_t renderer;
    uint32_t output;
    uint32_t caller;
    bool     observing;
};

thread_local GfxRenderCapsState gfx_render_caps{};
thread_local uint32_t           gfx_render_config_candidate      = 0;
thread_local uint32_t           gfx_render_config_renderer       = 0;
std::atomic_uint32_t            gfx_stale_render_config          = 0;
std::atomic_uint32_t            gfx_stale_render_config_renderer = 0;

std::atomic_bool     gameplay_start_seen   = false;
std::atomic_uint64_t gameplay_start_thread = 0;

struct GameplayFrameProbeState
{
    bool                                  active = false;
    uint64_t                              frames = 0;
    std::chrono::steady_clock::time_point first_frame;
};

thread_local GameplayFrameProbeState gameplay_frame_probe{};

struct UnitMoveApplyProbeState
{
    bool                                  active                      = false;
    bool                                  presentation_active         = false;
    uint64_t                              sequence                    = 0;
    uint64_t                              presentation_polls          = 0;
    uint32_t                              presentation_target         = 0;
    uint32_t                              presentation_result         = 0;
    uint32_t                              unit_record_address         = 0;
    bool                                  apply_snapshot_valid        = false;
    bool                                  presentation_snapshot_valid = false;
    bool                                  completion_snapshot_valid   = false;
    std::array<uint8_t, 84>               apply_snapshot{};
    std::array<uint8_t, 84>               presentation_snapshot{};
    std::array<uint8_t, 84>               completion_snapshot{};
    std::chrono::steady_clock::time_point begin;
    std::chrono::steady_clock::time_point map_update_begin;
    std::chrono::steady_clock::time_point presentation_begin;
    std::chrono::steady_clock::time_point presentation_wait_begin;
};

thread_local UnitMoveApplyProbeState unit_move_apply_probe{};
std::atomic_uint64_t                 unit_move_submit_sequence = 0;

struct CombatRecordPairSnapshot
{
    bool                    attacker_valid = false;
    bool                    defender_valid = false;
    std::array<uint8_t, 84> attacker{};
    std::array<uint8_t, 84> defender{};
};

struct CombatProbeState
{
    bool                                  active                  = false;
    bool                                  participants_valid      = false;
    bool                                  presentation_active     = false;
    uint64_t                              sequence                = 0;
    uint64_t                              resolve_calls           = 0;
    uint64_t                              presentation_polls      = 0;
    uint32_t                              attacker_player         = UINT32_MAX;
    uint32_t                              attacker_unit           = UINT32_MAX;
    uint32_t                              defender_player         = UINT32_MAX;
    uint32_t                              defender_unit           = UINT32_MAX;
    uint32_t                              attacker_record_address = 0;
    uint32_t                              defender_record_address = 0;
    uint32_t                              presentation_target     = 0;
    CombatRecordPairSnapshot              participant_snapshot{};
    CombatRecordPairSnapshot              presentation_snapshot{};
    CombatRecordPairSnapshot              completion_snapshot{};
    std::chrono::steady_clock::time_point begin;
    std::chrono::steady_clock::time_point presentation_begin;
};

thread_local CombatProbeState combat_probe{};
std::atomic_uint64_t          combat_sequence = 0;

struct CombatPaceProbeState
{
    bool     step_active = false;
    uint64_t setup       = 0;
    uint64_t steps       = 0;
    int32_t  counter     = 0;
    int32_t  progress    = 0;
    float    divisor     = 0.0f;
};

thread_local CombatPaceProbeState combat_pace_probe{};

struct CivilizationBonusPlayerProbeState
{
    bool                    valid = false;
    int32_t                 era   = -1;
    uint32_t                civ   = UINT32_MAX;
    uint64_t                seen  = 0;
    std::array<uint32_t, 4> bonuses{};
};

struct CivilizationBonusProbeState
{
    bool                                             active = false;
    std::array<CivilizationBonusPlayerProbeState, 6> players{};
};

thread_local CivilizationBonusProbeState civilization_bonus_probe{};

struct UnitStatProbeState
{
    size_t                    seen_count = 0;
    std::array<uint32_t, 384> seen{};
};

thread_local UnitStatProbeState unit_stat_probe{};

struct RushCostDisplaySnapshot
{
    uint32_t city       = UINT32_MAX;
    uint32_t player     = UINT32_MAX;
    uint32_t civ        = UINT32_MAX;
    int32_t  era        = -1;
    int32_t  item       = -1;
    int32_t  remaining  = -1;
    int32_t  multiplier = -1;
    int32_t  divisor    = -1;
    int32_t  native     = -1;
    int32_t  corrected  = -1;

    bool operator==(const RushCostDisplaySnapshot&) const = default;
};

struct RushCostProbeState
{
    bool                    display_valid = false;
    RushCostDisplaySnapshot display;
};

thread_local RushCostProbeState rush_cost_probe{};

bool TryReadCivilizationBonusRow(uint32_t                 civ,
                                 std::array<uint32_t, 4>& bonuses)
{
    constexpr uint32_t kCivilizationBonusTable = 0x82F6F950;
    constexpr uint32_t kCivilizationCount      = 16;
    if (civ >= kCivilizationCount)
    {
        return false;
    }

    for (uint32_t era = 0; era < bonuses.size(); ++era)
    {
        if (!TryReadGuestU32(
                kCivilizationBonusTable +
                    (civ * bonuses.size() + era) * sizeof(uint32_t),
                bonuses[era]))
        {
            return false;
        }
    }
    return true;
}

bool TryCalculateCorrectedRushCost(int32_t  multiplier,
                                   int32_t  divisor,
                                   int32_t  remaining,
                                   int32_t& corrected)
{
    if (multiplier <= 0 || divisor <= 0 || remaining < 0)
    {
        return false;
    }

    const int64_t product = static_cast<int64_t>(multiplier) * remaining;
    const int64_t value   = (product + divisor - 1) / divisor;
    if (value > std::numeric_limits<int32_t>::max())
    {
        return false;
    }

    corrected = static_cast<int32_t>(value);
    return true;
}

void ResolveRushCostOwner(uint32_t  city_offset,
                          uint32_t& city,
                          uint32_t& player,
                          uint32_t& civ)
{
    constexpr uint32_t kCityTable   = 0x8314FE28;
    constexpr uint32_t kCitySize    = 188;
    constexpr uint32_t kPlayerCivs  = 0x830ECD28;
    constexpr uint32_t kPlayerCount = 6;

    city   = UINT32_MAX;
    player = UINT32_MAX;
    civ    = UINT32_MAX;
    if (city_offset % kCitySize != 0)
    {
        return;
    }

    uint8_t owner = 0;
    if (!TryReadGuestU8(kCityTable + city_offset, owner) ||
        owner >= kPlayerCount)
    {
        return;
    }

    city   = city_offset / kCitySize;
    player = owner;
    TryReadGuestU32(kPlayerCivs + player * sizeof(uint32_t), civ);
}

bool CaptureUnitRecord(uint32_t address, std::array<uint8_t, 84>& snapshot)
{
    if (!IsGuestReadableRange(address, snapshot.size()))
    {
        return false;
    }

    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    std::copy_n(source, snapshot.size(), snapshot.begin());
    return true;
}

uint32_t UnitRecordAddress(uint32_t player, uint32_t unit)
{
    constexpr uint32_t kUnitTable = 0x830F2BF0;
    constexpr uint32_t kUnitSize  = 84;
    return kUnitTable + ((player << 8) + unit) * kUnitSize;
}

CombatRecordPairSnapshot CaptureCombatRecordPair()
{
    CombatRecordPairSnapshot snapshot{};
    snapshot.attacker_valid = CaptureUnitRecord(
        combat_probe.attacker_record_address,
        snapshot.attacker);
    snapshot.defender_valid = CaptureUnitRecord(
        combat_probe.defender_record_address,
        snapshot.defender);
    return snapshot;
}

void LogCombatRecordDiff(const char*                    phase,
                         const char*                    role,
                         const std::array<uint8_t, 84>& before,
                         const std::array<uint8_t, 84>& after)
{
    bool changed = false;
    for (size_t offset = 0; offset < before.size(); offset += 2)
    {
        const uint16_t before_value =
            (uint16_t{ before[offset] } << 8) | before[offset + 1];
        const uint16_t after_value =
            (uint16_t{ after[offset] } << 8) | after[offset + 1];
        if (before_value != after_value)
        {
            changed = true;
            REXLOG_INFO(
                "QoL combat probe: record-diff={} phase={} role={} "
                "offset={:02X} before={:04X} after={:04X}",
                combat_probe.sequence,
                phase,
                role,
                offset,
                before_value,
                after_value);
        }
    }
    if (!changed)
    {
        REXLOG_INFO(
            "QoL combat probe: record-diff={} phase={} role={} unchanged",
            combat_probe.sequence,
            phase,
            role);
    }
}

void LogCombatPairDiff(const char*                     phase,
                       const CombatRecordPairSnapshot& before,
                       const CombatRecordPairSnapshot& after)
{
    if (before.attacker_valid && after.attacker_valid)
    {
        LogCombatRecordDiff(phase, "attacker", before.attacker, after.attacker);
    }
    if (before.defender_valid && after.defender_valid)
    {
        LogCombatRecordDiff(phase, "defender", before.defender, after.defender);
    }
}

bool ResolveCombatParticipants(uint32_t stack_pointer)
{
    uint32_t attacker_player = 0;
    uint32_t attacker_unit   = 0;
    uint32_t defender_player = 0;
    uint32_t defender_unit   = 0;
    if (!TryReadGuestU32(stack_pointer + 1572, attacker_player) ||
        !TryReadGuestU32(stack_pointer + 1580, attacker_unit) ||
        !TryReadGuestU32(stack_pointer + 1596, defender_player) ||
        !TryReadGuestU32(stack_pointer + 1604, defender_unit) ||
        attacker_player >= 6 || defender_player >= 6 || attacker_unit >= 256 ||
        defender_unit >= 256)
    {
        return false;
    }

    combat_probe.attacker_player = attacker_player;
    combat_probe.attacker_unit   = attacker_unit;
    combat_probe.defender_player = defender_player;
    combat_probe.defender_unit   = defender_unit;
    combat_probe.attacker_record_address =
        UnitRecordAddress(attacker_player, attacker_unit);
    combat_probe.defender_record_address =
        UnitRecordAddress(defender_player, defender_unit);
    combat_probe.participants_valid = true;
    return true;
}

void LogUnitRecordDiff(uint64_t                       sequence,
                       const char*                    phase,
                       const std::array<uint8_t, 84>& before,
                       const std::array<uint8_t, 84>& after)
{
    for (size_t offset = 0; offset < before.size(); offset += 2)
    {
        const uint16_t before_value =
            (uint16_t{ before[offset] } << 8) | before[offset + 1];
        const uint16_t after_value =
            (uint16_t{ after[offset] } << 8) | after[offset + 1];
        if (before_value != after_value)
        {
            REXLOG_INFO(
                "QoL movement probe: unit-diff={} phase={} offset={:02X} "
                "before={:04X} after={:04X}",
                sequence,
                phase,
                offset,
                before_value,
                after_value);
        }
    }
}

uint16_t UnitRecordU16(const std::array<uint8_t, 84>& snapshot, size_t offset)
{
    return (uint16_t{ snapshot[offset] } << 8) | snapshot[offset + 1];
}

uint32_t UnitRecordU32(const std::array<uint8_t, 84>& snapshot, size_t offset)
{
    return (uint32_t{ snapshot[offset] } << 24) |
           (uint32_t{ snapshot[offset + 1] } << 16) |
           (uint32_t{ snapshot[offset + 2] } << 8) |
           snapshot[offset + 3];
}

void LogUnitRecordSnapshot(uint64_t                       sequence,
                           const char*                    phase,
                           uint32_t                       address,
                           bool                           valid,
                           const std::array<uint8_t, 84>& snapshot)
{
    if (!valid)
    {
        REXLOG_INFO(
            "QoL movement probe: unit-state={} phase={} record={:08X} "
            "unavailable",
            sequence,
            phase,
            address);
        return;
    }

    REXLOG_INFO(
        "QoL movement probe: unit-state={} phase={} record={:08X} "
        "v02={:04X} flags0C={:08X} v14={:08X} v18={:04X} "
        "pos={:04X}/{:04X} v20={:04X} dest={:04X}/{:04X} "
        "v26={:04X} v4E={:04X}",
        sequence,
        phase,
        address,
        UnitRecordU16(snapshot, 0x02),
        UnitRecordU32(snapshot, 0x0C),
        UnitRecordU32(snapshot, 0x14),
        UnitRecordU16(snapshot, 0x18),
        UnitRecordU16(snapshot, 0x1C),
        UnitRecordU16(snapshot, 0x1E),
        UnitRecordU16(snapshot, 0x20),
        UnitRecordU16(snapshot, 0x22),
        UnitRecordU16(snapshot, 0x24),
        UnitRecordU16(snapshot, 0x26),
        UnitRecordU16(snapshot, 0x4E));
}

enum class UnitStatConsumer : uint8_t
{
    combat,
    unit_choice,
    turn_evaluation,
    turn_filter,
    unknown,
};

UnitStatConsumer ClassifyUnitStatConsumer(uint32_t caller)
{
    if (caller >= 0x82CD9970 && caller < 0x82CDDEF8)
    {
        return UnitStatConsumer::combat;
    }
    if (caller >= 0x82CB44E0 && caller < 0x82CB6E48)
    {
        return UnitStatConsumer::unit_choice;
    }
    if (caller >= 0x82CB6E48 && caller < 0x82CBC8E8)
    {
        return UnitStatConsumer::turn_evaluation;
    }
    if (caller >= 0x82CBF570 && caller < 0x82CC1850)
    {
        return UnitStatConsumer::turn_filter;
    }
    return UnitStatConsumer::unknown;
}

const char* UnitStatConsumerName(UnitStatConsumer consumer)
{
    switch (consumer)
    {
        case UnitStatConsumer::combat:
            return "combat";
        case UnitStatConsumer::unit_choice:
            return "unit-choice";
        case UnitStatConsumer::turn_evaluation:
            return "turn-evaluation";
        case UnitStatConsumer::turn_filter:
            return "turn-filter";
        default:
            return "unknown";
    }
}

void RecordUnitStatLookup(const char*  stat,
                          uint32_t     stat_index,
                          uint32_t     field_offset,
                          PPCRegister& r3,
                          PPCRegister& r4,
                          uint64_t     lr)
{
    if (!REXCVAR_GET(qol_probe_unit_stats))
    {
        unit_stat_probe = {};
        return;
    }

    constexpr uint32_t kUnitDefinitionTable = 0x82F700D8;
    constexpr uint32_t kUnitDefinitionSize  = 0x94;
    constexpr uint32_t kUnitDefinitionCount = 29;
    constexpr uint32_t kPlayerCount         = 6;
    const uint32_t     player               = r3.u32;
    const uint32_t     unit_type            = r4.u32;
    const uint32_t     caller               = static_cast<uint32_t>(lr);
    const auto         consumer             = ClassifyUnitStatConsumer(caller);
    if (consumer == UnitStatConsumer::unknown || player >= kPlayerCount ||
        unit_type >= kUnitDefinitionCount)
    {
        return;
    }

    uint32_t active_player_state = UINT32_MAX;
    uint32_t human_player_mask   = 0;
    rerevved::gameplay::ReadProbePlayerState(active_player_state, human_player_mask);
    const uint32_t active_player =
        active_player_state < kPlayerCount ? active_player_state : 7;
    const uint32_t consumer_index = static_cast<uint32_t>(consumer);
    const uint32_t key            = (((consumer_index * 2 + stat_index) * kPlayerCount +
                                      player) *
                                         8 +
                                     active_player);
    if (std::find(
            unit_stat_probe.seen.begin(),
            unit_stat_probe.seen.begin() + unit_stat_probe.seen_count,
            key) != unit_stat_probe.seen.begin() + unit_stat_probe.seen_count)
    {
        return;
    }
    if (unit_stat_probe.seen_count == unit_stat_probe.seen.size())
    {
        return;
    }
    unit_stat_probe.seen[unit_stat_probe.seen_count++] = key;

    uint8_t base_value = 0;
    if (!TryReadGuestU8(
            kUnitDefinitionTable + unit_type * kUnitDefinitionSize +
                field_offset,
            base_value))
    {
        return;
    }

    const bool player_human =
        player < 32 &&
        (human_player_mask & (uint32_t{ 1 } << player)) != 0;
    REXLOG_INFO(
        "QoL unit-stat probe: consumer={} stat={} player={} unit={} base={} "
        "caller={:08X} active-player={} human-mask={:08X} player-human={}",
        UnitStatConsumerName(consumer),
        stat,
        player,
        unit_type,
        static_cast<int32_t>(static_cast<int8_t>(base_value)),
        caller,
        active_player_state,
        human_player_mask,
        player_human);
}

uint64_t CurrentThreadToken()
{
    return static_cast<uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

} // namespace

void ReRevvedProbeGameStart()
{
    if (!REXCVAR_GET(qol_probe_gameplay_frame))
    {
        return;
    }

    const uint64_t thread = CurrentThreadToken();
    gameplay_start_thread.store(thread, std::memory_order_release);
    gameplay_start_seen.store(true, std::memory_order_release);
    REXLOG_INFO("QoL frame probe: AudioGameStartInit thread={:016X}", thread);
}

void ReRevvedProbeGameplayFrame()
{
    rerevved::gameplay::PublishFrameSnapshot();

    const bool probe_frame = REXCVAR_GET(qol_probe_gameplay_frame);
    const bool probe_state = REXCVAR_GET(qol_probe_gameplay_state);
    rerevved::gameplay::ProbePublishedSnapshot(probe_state);
    if (!probe_frame && !probe_state)
    {
        gameplay_frame_probe = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (probe_frame && !gameplay_frame_probe.active)
    {
        gameplay_frame_probe.active      = true;
        gameplay_frame_probe.first_frame = now;
    }

    if (probe_frame)
    {
        ++gameplay_frame_probe.frames;
        if (gameplay_frame_probe.frames == 1 ||
            gameplay_frame_probe.frames % 300 == 0)
        {
            const uint64_t thread       = CurrentThreadToken();
            const bool     start_seen   = gameplay_start_seen.load(std::memory_order_acquire);
            const uint64_t start_thread = gameplay_start_thread.load(std::memory_order_acquire);
            const double   elapsed =
                std::chrono::duration<double>(now - gameplay_frame_probe.first_frame)
                    .count();
            const double cadence =
                elapsed > 0.0 ? (gameplay_frame_probe.frames - 1) / elapsed : 0.0;

            REXLOG_INFO(
                "QoL frame probe: frames={} elapsed={:.3f}s cadence={:.2f}Hz "
                "thread={:016X} game_start_seen={} start_thread_match={}",
                gameplay_frame_probe.frames,
                elapsed,
                cadence,
                thread,
                start_seen,
                start_seen && thread == start_thread);
        }
    }
}

void ReRevvedProbeUnitMoveSubmit(PPCRegister& r3,
                                 PPCRegister& r4,
                                 PPCRegister& r5,
                                 PPCRegister& r6,
                                 PPCRegister& r7,
                                 PPCRegister& r8)
{
    constexpr uint32_t kMoveCommand = 0x11;
    if (!REXCVAR_GET(qol_probe_unit_movement) || r3.u32 != kMoveCommand)
    {
        return;
    }

    const uint64_t sequence =
        unit_move_submit_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    REXLOG_INFO(
        "QoL movement probe: submit={} player={:08X} unit={:08X} "
        "target={:08X} aux={:08X}/{:08X} thread={:016X}",
        sequence,
        r4.u32,
        r5.u32,
        r6.u32,
        r7.u32,
        r8.u32,
        CurrentThreadToken());
}

void ReRevvedProbeUnitMoveApplyBegin(PPCRegister& r31,
                                     PPCRegister& r15,
                                     PPCRegister& r29)
{
    if (!REXCVAR_GET(qol_probe_unit_movement))
    {
        unit_move_apply_probe = {};
        return;
    }

    unit_move_apply_probe.active = true;
    unit_move_apply_probe.sequence =
        unit_move_submit_sequence.load(std::memory_order_relaxed);
    unit_move_apply_probe.begin = std::chrono::steady_clock::now();
    if (r31.u32 < 6 && r15.u32 < 256)
    {
        constexpr uint32_t kUnitTable = 0x830F2BF0;
        constexpr uint32_t kUnitSize  = 84;
        const uint32_t     index      = (r31.u32 << 8) + r15.u32;
        unit_move_apply_probe.unit_record_address =
            kUnitTable + index * kUnitSize;
        unit_move_apply_probe.apply_snapshot_valid = CaptureUnitRecord(
            unit_move_apply_probe.unit_record_address,
            unit_move_apply_probe.apply_snapshot);
    }
    LogUnitRecordSnapshot(
        unit_move_apply_probe.sequence,
        "apply-begin",
        unit_move_apply_probe.unit_record_address,
        unit_move_apply_probe.apply_snapshot_valid,
        unit_move_apply_probe.apply_snapshot);
    REXLOG_INFO(
        "QoL movement probe: apply-begin={} player={:08X} unit={:08X} "
        "target={:08X} thread={:016X}",
        unit_move_apply_probe.sequence,
        r31.u32,
        r15.u32,
        r29.u32,
        CurrentThreadToken());
}

void ReRevvedProbeUnitMoveApplyEnd()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    std::array<uint8_t, 84> final_snapshot{};
    const bool              final_snapshot_valid = CaptureUnitRecord(
        unit_move_apply_probe.unit_record_address,
        final_snapshot);
    if (final_snapshot_valid)
    {
        if (unit_move_apply_probe.completion_snapshot_valid)
        {
            LogUnitRecordDiff(
                unit_move_apply_probe.sequence,
                "completion-to-apply-end",
                unit_move_apply_probe.completion_snapshot,
                final_snapshot);
        }
        else if (unit_move_apply_probe.apply_snapshot_valid)
        {
            LogUnitRecordDiff(
                unit_move_apply_probe.sequence,
                "apply-to-end",
                unit_move_apply_probe.apply_snapshot,
                final_snapshot);
        }
    }
    LogUnitRecordSnapshot(
        unit_move_apply_probe.sequence,
        "apply-end",
        unit_move_apply_probe.unit_record_address,
        final_snapshot_valid,
        final_snapshot);

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - unit_move_apply_probe.begin)
            .count();
    REXLOG_INFO(
        "QoL movement probe: apply-end={} elapsed={:.3f}ms thread={:016X}",
        unit_move_apply_probe.sequence,
        elapsed_ms,
        CurrentThreadToken());
    unit_move_apply_probe = {};
}

void ReRevvedProbeUnitMoveMapUpdateBegin()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    unit_move_apply_probe.map_update_begin = std::chrono::steady_clock::now();
}

void ReRevvedProbeUnitMoveMapUpdateEnd()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            unit_move_apply_probe.map_update_begin)
            .count();
    REXLOG_INFO(
        "QoL movement probe: map-update={} elapsed={:.3f}ms",
        unit_move_apply_probe.sequence,
        elapsed_ms);
}

void ReRevvedProbeUnitMovePresentationBegin(PPCRegister& ctr)
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    unit_move_apply_probe.presentation_active = true;
    unit_move_apply_probe.presentation_target = ctr.u32;
    unit_move_apply_probe.presentation_begin  = std::chrono::steady_clock::now();
    REXLOG_INFO(
        "QoL movement probe: presentation-begin={} target={:08X}",
        unit_move_apply_probe.sequence,
        unit_move_apply_probe.presentation_target);
}

void ReRevvedProbeUnitMoveCooldownSet(PPCRegister& r11)
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    REXLOG_INFO(
        "QoL movement probe: cooldown-set={} value={}ms",
        unit_move_apply_probe.sequence,
        r11.u32);
}

void ReRevvedProbeUnitMoveAnimationBegin()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.active)
    {
        return;
    }

    unit_move_apply_probe.presentation_active         = true;
    unit_move_apply_probe.presentation_target         = 0x82D11AD8;
    unit_move_apply_probe.presentation_begin          = std::chrono::steady_clock::now();
    unit_move_apply_probe.presentation_snapshot_valid = CaptureUnitRecord(
        unit_move_apply_probe.unit_record_address,
        unit_move_apply_probe.presentation_snapshot);
    if (unit_move_apply_probe.apply_snapshot_valid &&
        unit_move_apply_probe.presentation_snapshot_valid)
    {
        LogUnitRecordDiff(
            unit_move_apply_probe.sequence,
            "apply-to-presentation",
            unit_move_apply_probe.apply_snapshot,
            unit_move_apply_probe.presentation_snapshot);
    }
    LogUnitRecordSnapshot(
        unit_move_apply_probe.sequence,
        "presentation-begin",
        unit_move_apply_probe.unit_record_address,
        unit_move_apply_probe.presentation_snapshot_valid,
        unit_move_apply_probe.presentation_snapshot);
    REXLOG_INFO(
        "QoL movement probe: presentation-begin={} target={:08X}",
        unit_move_apply_probe.sequence,
        unit_move_apply_probe.presentation_target);
}

void ReRevvedProbeUnitMovePresentationReturned(PPCRegister& r3)
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.presentation_active)
    {
        return;
    }

    unit_move_apply_probe.presentation_result = r3.u32;
}

void ReRevvedProbeUnitMovePresentationPoll()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.presentation_active)
    {
        return;
    }

    if (unit_move_apply_probe.presentation_polls == 0)
    {
        unit_move_apply_probe.presentation_wait_begin =
            std::chrono::steady_clock::now();
    }
    ++unit_move_apply_probe.presentation_polls;
}

void ReRevvedProbeUnitMovePresentationEnd()
{
    if (!REXCVAR_GET(qol_probe_unit_movement) ||
        !unit_move_apply_probe.presentation_active)
    {
        return;
    }

    unit_move_apply_probe.completion_snapshot_valid = CaptureUnitRecord(
        unit_move_apply_probe.unit_record_address,
        unit_move_apply_probe.completion_snapshot);
    if (unit_move_apply_probe.presentation_snapshot_valid &&
        unit_move_apply_probe.completion_snapshot_valid)
    {
        LogUnitRecordDiff(
            unit_move_apply_probe.sequence,
            "during-presentation",
            unit_move_apply_probe.presentation_snapshot,
            unit_move_apply_probe.completion_snapshot);
    }
    LogUnitRecordSnapshot(
        unit_move_apply_probe.sequence,
        "presentation-end",
        unit_move_apply_probe.unit_record_address,
        unit_move_apply_probe.completion_snapshot_valid,
        unit_move_apply_probe.completion_snapshot);

    const auto   now = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            now - unit_move_apply_probe.presentation_begin)
            .count();
    const double wait_ms = unit_move_apply_probe.presentation_polls == 0
                               ? 0.0
                               : std::chrono::duration<double, std::milli>(
                                     now - unit_move_apply_probe.presentation_wait_begin)
                                     .count();
    REXLOG_INFO(
        "QoL movement probe: presentation-end={} target={:08X} "
        "result={:08X} polls={} elapsed={:.3f}ms wait={:.3f}ms",
        unit_move_apply_probe.sequence,
        unit_move_apply_probe.presentation_target,
        unit_move_apply_probe.presentation_result,
        unit_move_apply_probe.presentation_polls,
        elapsed_ms,
        wait_ms);
    unit_move_apply_probe.presentation_active = false;
}

void ReRevvedProbeCombatApplyBegin(PPCRegister& r31,
                                   PPCRegister& r15,
                                   PPCRegister& r29)
{
    if (!REXCVAR_GET(qol_probe_combat))
    {
        combat_probe = {};
        return;
    }
    if (combat_probe.active)
    {
        return;
    }

    combat_probe        = {};
    combat_probe.active = true;
    combat_probe.sequence =
        combat_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    combat_probe.begin = std::chrono::steady_clock::now();
    REXLOG_INFO(
        "QoL combat probe: apply-begin={} player={} unit={} move_dir={} "
        "thread={:016X}",
        combat_probe.sequence,
        r31.u32,
        r15.u32,
        r29.u32,
        CurrentThreadToken());
}

void ReRevvedProbeCombatResolveBegin(PPCRegister& r3,
                                     PPCRegister& r4,
                                     PPCRegister& r5,
                                     PPCRegister& r6,
                                     PPCRegister& r7)
{
    if (!REXCVAR_GET(qol_probe_combat))
    {
        combat_probe = {};
        return;
    }
    if (!combat_probe.active)
    {
        combat_probe.active = true;
        combat_probe.sequence =
            combat_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        combat_probe.begin = std::chrono::steady_clock::now();
    }
    ++combat_probe.resolve_calls;
    if (combat_probe.resolve_calls != 1)
    {
        return;
    }

    REXLOG_INFO(
        "QoL combat probe: resolve-begin={} player={} unit={} "
        "mode={:08X} defender={:08X}/{:08X} thread={:016X}",
        combat_probe.sequence,
        r3.u32,
        r4.u32,
        r5.u32,
        r6.u32,
        r7.u32,
        CurrentThreadToken());
}

void ReRevvedProbeCombatParticipants(PPCRegister& r1)
{
    if (!REXCVAR_GET(qol_probe_combat) || !combat_probe.active)
    {
        return;
    }
    if (combat_probe.participants_valid)
    {
        return;
    }

    if (!ResolveCombatParticipants(r1.u32))
    {
        REXLOG_INFO(
            "QoL combat probe: participants={} unavailable stack={:08X}",
            combat_probe.sequence,
            r1.u32);
        return;
    }

    combat_probe.participant_snapshot = CaptureCombatRecordPair();
    REXLOG_INFO(
        "QoL combat probe: participants={} attacker={}/{} record={:08X} "
        "valid={} defender={}/{} record={:08X} valid={} thread={:016X}",
        combat_probe.sequence,
        combat_probe.attacker_player,
        combat_probe.attacker_unit,
        combat_probe.attacker_record_address,
        combat_probe.participant_snapshot.attacker_valid,
        combat_probe.defender_player,
        combat_probe.defender_unit,
        combat_probe.defender_record_address,
        combat_probe.participant_snapshot.defender_valid,
        CurrentThreadToken());
}

void ReRevvedProbeCombatAuxPresentation(PPCRegister& r3,
                                        PPCRegister& r4,
                                        PPCRegister& r1)
{
    if (!REXCVAR_GET(qol_probe_combat) || !combat_probe.active)
    {
        return;
    }

    if (!combat_probe.participants_valid)
    {
        ResolveCombatParticipants(r1.u32);
    }
    const CombatRecordPairSnapshot snapshot = CaptureCombatRecordPair();
    if (combat_probe.participants_valid)
    {
        LogCombatPairDiff(
            "participants-to-aux-presentation",
            combat_probe.participant_snapshot,
            snapshot);
    }
    REXLOG_INFO(
        "QoL combat probe: aux-presentation={} kind={:08X} wait={:08X} "
        "thread={:016X}",
        combat_probe.sequence,
        r3.u32,
        r4.u32,
        CurrentThreadToken());
}

void ReRevvedProbeCombatPresentationBegin(PPCRegister& ctr, PPCRegister& r1)
{
    if (!REXCVAR_GET(qol_probe_combat) || !combat_probe.active)
    {
        return;
    }

    if (!combat_probe.participants_valid)
    {
        ResolveCombatParticipants(r1.u32);
    }
    combat_probe.presentation_snapshot = CaptureCombatRecordPair();
    if (combat_probe.participants_valid)
    {
        LogCombatPairDiff(
            "participants-to-presentation",
            combat_probe.participant_snapshot,
            combat_probe.presentation_snapshot);
    }
    combat_probe.presentation_active = true;
    combat_probe.presentation_target = ctr.u32;
    combat_probe.presentation_polls  = 0;
    combat_probe.presentation_begin  = std::chrono::steady_clock::now();
    REXLOG_INFO(
        "QoL combat probe: presentation-begin={} target={:08X} "
        "thread={:016X}",
        combat_probe.sequence,
        combat_probe.presentation_target,
        CurrentThreadToken());
}

void ReRevvedProbeCombatPresentationPoll()
{
    if (!REXCVAR_GET(qol_probe_combat) ||
        !combat_probe.presentation_active)
    {
        return;
    }
    ++combat_probe.presentation_polls;
}

void ReRevvedProbeCombatPresentationEnd(PPCRegister& r1)
{
    if (!REXCVAR_GET(qol_probe_combat) ||
        !combat_probe.presentation_active)
    {
        return;
    }

    if (!combat_probe.participants_valid)
    {
        ResolveCombatParticipants(r1.u32);
    }
    combat_probe.completion_snapshot = CaptureCombatRecordPair();
    LogCombatPairDiff(
        "during-presentation",
        combat_probe.presentation_snapshot,
        combat_probe.completion_snapshot);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - combat_probe.presentation_begin)
            .count();
    REXLOG_INFO(
        "QoL combat probe: presentation-end={} target={:08X} polls={} "
        "resolve-calls={} elapsed={:.3f}ms thread={:016X}",
        combat_probe.sequence,
        combat_probe.presentation_target,
        combat_probe.presentation_polls,
        combat_probe.resolve_calls,
        elapsed_ms,
        CurrentThreadToken());
    combat_probe.presentation_active = false;
}

void ReRevvedProbeCombatResolveEnd(PPCRegister& r1)
{
    if (!REXCVAR_GET(qol_probe_combat) || !combat_probe.active)
    {
        return;
    }

    if (!combat_probe.participants_valid)
    {
        ResolveCombatParticipants(r1.u32);
    }
    const CombatRecordPairSnapshot final_snapshot = CaptureCombatRecordPair();
    if (combat_probe.completion_snapshot.attacker_valid ||
        combat_probe.completion_snapshot.defender_valid)
    {
        LogCombatPairDiff(
            "completion-to-resolve-end",
            combat_probe.completion_snapshot,
            final_snapshot);
    }
    else
    {
        LogCombatPairDiff(
            "participants-to-resolve-end",
            combat_probe.participant_snapshot,
            final_snapshot);
    }
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - combat_probe.begin)
            .count();
    REXLOG_INFO(
        "QoL combat probe: resolve-end={} resolve-calls={} elapsed={:.3f}ms "
        "thread={:016X}",
        combat_probe.sequence,
        combat_probe.resolve_calls,
        elapsed_ms,
        CurrentThreadToken());
    combat_probe = {};
}

void ReRevvedProbeCombatPaceSelected()
{
    if (!REXCVAR_GET(qol_probe_combat))
    {
        combat_pace_probe = {};
        return;
    }

    constexpr uint32_t kCombatPaceDivisor = 0x82F79FBC;
    combat_pace_probe.step_active         = false;
    ++combat_pace_probe.setup;
    combat_pace_probe.steps = 0;
    if (!TryReadGuestF32(kCombatPaceDivisor, combat_pace_probe.divisor))
    {
        REXLOG_INFO(
            "QoL combat pace probe: selected={} divisor=unavailable",
            combat_pace_probe.setup);
        return;
    }

    REXLOG_INFO(
        "QoL combat pace probe: selected={} divisor={:.3f}",
        combat_pace_probe.setup,
        combat_pace_probe.divisor);
}

void ReRevvedProbeCombatPaceStepBegin(PPCRegister& r31)
{
    if (!REXCVAR_GET(qol_probe_combat))
    {
        combat_pace_probe = {};
        return;
    }

    constexpr uint32_t kCombatPaceDivisor = 0x82F79FBC;
    constexpr uint32_t kFrameProgress     = 0x8314F04C;
    uint32_t           counter            = 0;
    uint32_t           progress           = 0;
    float              divisor            = 0.0f;
    combat_pace_probe.step_active =
        TryReadGuestU32(r31.u32 - 4, counter) &&
        TryReadGuestU32(kFrameProgress, progress) &&
        TryReadGuestF32(kCombatPaceDivisor, divisor);
    if (!combat_pace_probe.step_active)
    {
        return;
    }

    combat_pace_probe.counter  = static_cast<int32_t>(counter);
    combat_pace_probe.progress = static_cast<int32_t>(progress);
    combat_pace_probe.divisor  = divisor;
}

void ReRevvedProbeCombatPaceStepEnd(PPCRegister& r11)
{
    if (!REXCVAR_GET(qol_probe_combat) ||
        !combat_pace_probe.step_active)
    {
        return;
    }

    combat_pace_probe.step_active = false;
    ++combat_pace_probe.steps;
    if (combat_pace_probe.steps <= 8 || combat_pace_probe.steps % 60 == 0)
    {
        REXLOG_INFO(
            "QoL combat pace probe: step={} setup={} counter={} progress={} "
            "divisor={:.3f} result={}",
            combat_pace_probe.steps,
            combat_pace_probe.setup,
            combat_pace_probe.counter,
            combat_pace_probe.progress,
            combat_pace_probe.divisor,
            r11.s32);
    }
}

void ReRevvedProbeCombatObjectScaleSet()
{
    if (!REXCVAR_GET(qol_probe_combat))
    {
        return;
    }

    constexpr uint32_t kCombatObjectRoot = 0x8314F288;
    constexpr uint32_t kSavedObjectScale = 0x82F79FFC;
    uint32_t           root              = 0;
    uint32_t           object            = 0;
    float              saved             = 0.0f;
    float              active            = 0.0f;
    if (!TryReadGuestU32(kCombatObjectRoot, root) ||
        !TryReadGuestU32(root + 4, object) ||
        !TryReadGuestF32(kSavedObjectScale, saved) ||
        !TryReadGuestF32(object + 72, active))
    {
        REXLOG_INFO("QoL combat pace probe: object-scale unavailable");
        return;
    }

    REXLOG_INFO(
        "QoL combat pace probe: object={:08X} saved-scale={:.3f} "
        "active-scale={:.3f}",
        object,
        saved,
        active);
}

void ReRevvedApplyCombatPaceOverride()
{
    if (!REXCVAR_GET(qol_fast_combat))
    {
        return;
    }

    constexpr uint32_t kCombatPaceDivisor = 0x82F79FBC;
    constexpr float    kObservedNormal    = 2.0f;
    constexpr float    kNativeFast        = 0.5f;
    float              selected           = 0.0f;
    if (!TryReadGuestF32(kCombatPaceDivisor, selected) ||
        selected != kObservedNormal)
    {
        if (REXCVAR_GET(qol_probe_combat))
        {
            REXLOG_INFO(
                "QoL combat pace probe: override skipped divisor={:.3f}",
                selected);
        }
        return;
    }

    const bool applied = WriteGuestU32Safely(
        kCombatPaceDivisor,
        std::bit_cast<uint32_t>(kNativeFast));
    if (REXCVAR_GET(qol_probe_combat))
    {
        REXLOG_INFO(
            "QoL combat pace probe: override normal={:.3f} fast={:.3f} "
            "applied={}",
            selected,
            kNativeFast,
            applied);
    }
}

void ReRevvedProbeCivilizationBonusLookup(PPCRegister& r3,
                                          PPCRegister& r4,
                                          PPCRegister& r5)
{
    if (!REXCVAR_GET(qol_probe_civ_bonuses))
    {
        if (civilization_bonus_probe.active)
        {
            civilization_bonus_probe = {};
        }
        return;
    }

    civilization_bonus_probe.active         = true;
    constexpr uint32_t kPlayerEras          = 0x830ECD08;
    constexpr uint32_t kPlayerCivilizations = 0x830ECD28;
    constexpr uint32_t kExcludedBonusPlayer = 0x82F700B0;
    constexpr int32_t  kPlayerCount         = 6;
    constexpr int32_t  kBonusCount          = 64;
    const int32_t      bonus                = r3.s32;
    const int32_t      player               = r4.s32;
    const int32_t      exact                = r5.s32;
    if (player < 0 || player >= kPlayerCount || bonus < 0 ||
        bonus >= kBonusCount)
    {
        return;
    }

    uint32_t era_word        = 0;
    uint32_t civ             = UINT32_MAX;
    uint32_t excluded_player = UINT32_MAX;
    if (!TryReadGuestU32(
            kPlayerEras + player * sizeof(uint32_t),
            era_word) ||
        !TryReadGuestU32(
            kPlayerCivilizations + player * sizeof(uint32_t),
            civ) ||
        !TryReadGuestU32(kExcludedBonusPlayer, excluded_player))
    {
        return;
    }

    const int32_t era      = static_cast<int32_t>(era_word);
    auto&         snapshot = civilization_bonus_probe.players[player];
    if (!snapshot.valid || snapshot.era != era || snapshot.civ != civ)
    {
        std::array<uint32_t, 4> bonuses{};
        if (!TryReadCivilizationBonusRow(civ, bonuses))
        {
            return;
        }
        snapshot = { true, era, civ, 0, bonuses };
        REXLOG_INFO(
            "QoL civilization-bonus probe: state player={} civ={} era={} "
            "row=[{},{},{},{}] excluded={}",
            player,
            civ,
            era,
            bonuses[0],
            bonuses[1],
            bonuses[2],
            bonuses[3],
            excluded_player);
    }

    const uint64_t seen_bit = UINT64_C(1) << bonus;
    if ((snapshot.seen & seen_bit) != 0)
    {
        return;
    }
    snapshot.seen |= seen_bit;

    int32_t active = 0;
    if (static_cast<uint32_t>(player) != excluded_player)
    {
        if (exact != 0)
        {
            active = era >= 0 && era < static_cast<int32_t>(snapshot.bonuses.size())
                         ? snapshot.bonuses[era] == static_cast<uint32_t>(bonus)
                         : -1;
        }
        else
        {
            const int32_t last_era = std::clamp(
                era,
                0,
                static_cast<int32_t>(snapshot.bonuses.size()) - 1);
            for (int32_t index = 0; index <= last_era; ++index)
            {
                if (snapshot.bonuses[index] == static_cast<uint32_t>(bonus))
                {
                    active = 1;
                    break;
                }
            }
        }
    }

    REXLOG_INFO(
        "QoL civilization-bonus probe: query player={} civ={} era={} "
        "bonus={} exact={} active={}",
        player,
        civ,
        era,
        bonus,
        exact,
        active);
}

void ReRevvedProbeUnitAttackLookup(PPCRegister& r3,
                                   PPCRegister& r4,
                                   uint64_t     lr)
{
    RecordUnitStatLookup("attack", 0, 0x40, r3, r4, lr);
}

void ReRevvedProbeUnitDefenseLookup(PPCRegister& r3,
                                    PPCRegister& r4,
                                    uint64_t     lr)
{
    RecordUnitStatLookup("defense", 1, 0x41, r3, r4, lr);
}

void ReRevvedProbeRushCostDisplay(PPCRegister& r27,
                                  PPCRegister& r29,
                                  PPCRegister& r30,
                                  PPCRegister& r31,
                                  PPCRegister& r6,
                                  PPCRegister& r7,
                                  PPCRegister& r11)
{
    if (!REXCVAR_GET(qol_probe_rush_cost))
    {
        rush_cost_probe = {};
        return;
    }

    RushCostDisplaySnapshot snapshot{};
    ResolveRushCostOwner(
        r27.u32,
        snapshot.city,
        snapshot.player,
        snapshot.civ);
    snapshot.era        = r29.s32;
    snapshot.item       = r30.s32;
    snapshot.remaining  = r7.s32;
    snapshot.multiplier = r31.s32;
    snapshot.divisor    = r6.s32;
    snapshot.native     = r11.s32;
    if (!TryCalculateCorrectedRushCost(
            snapshot.multiplier,
            snapshot.divisor,
            snapshot.remaining,
            snapshot.corrected))
    {
        snapshot.corrected = -1;
    }

    if (rush_cost_probe.display_valid &&
        rush_cost_probe.display == snapshot)
    {
        return;
    }

    rush_cost_probe.display_valid = true;
    rush_cost_probe.display       = snapshot;
    REXLOG_INFO(
        "QoL rush-cost probe: display city={} player={} civ={} era={} "
        "item={} remaining={} multiplier={} divisor={} native={} "
        "corrected={}",
        snapshot.city,
        snapshot.player,
        snapshot.civ,
        snapshot.era,
        snapshot.item,
        snapshot.remaining,
        snapshot.multiplier,
        snapshot.divisor,
        snapshot.native,
        snapshot.corrected);
}

void ReRevvedProbeRushCostApply(PPCRegister& r24,
                                PPCRegister& r25,
                                PPCRegister& r26,
                                PPCRegister& r28,
                                PPCRegister& r30,
                                PPCRegister& r31,
                                PPCRegister& r3,
                                PPCRegister& r6,
                                PPCRegister& r8,
                                PPCRegister& r11)
{
    if (!REXCVAR_GET(qol_probe_rush_cost))
    {
        return;
    }

    uint32_t city   = UINT32_MAX;
    uint32_t player = UINT32_MAX;
    uint32_t civ    = UINT32_MAX;
    ResolveRushCostOwner(r28.u32, city, player, civ);
    REXLOG_INFO(
        "QoL rush-cost probe: apply city={} command-city={} player={} civ={} "
        "era={} item={} command-cost={} multiplier={} "
        "production-before={} production-bought={} production-after={} "
        "treasury-after={}",
        city,
        r24.u32,
        player,
        civ,
        r30.s32,
        r25.s32,
        r26.s32,
        r31.s32,
        r6.s32,
        r8.s32,
        r3.s32,
        r11.s32);
}

void ReRevvedProbeSaveSlotLoadBegin(PPCRegister& r4)
{
    if (!REXCVAR_GET(qol_probe_save_slots))
    {
        return;
    }

    constexpr uint32_t   kInternalSlotIdOffset = 0x108;
    std::array<char, 43> slot{};
    const uint32_t       slot_address = r4.u32 + kInternalSlotIdOffset;
    const bool           valid        = r4.u32 <= UINT32_MAX - kInternalSlotIdOffset &&
                                        TryReadGuestText(slot_address, slot);
    REXLOG_INFO(
        "QoL save-slot probe: load-begin record={:08X} slot={} valid={}",
        r4.u32,
        valid ? slot.data() : "<unavailable>",
        valid);
}

void ReRevvedProbeSaveSlotSaveOutcome(PPCRegister& r25, PPCRegister& r29)
{
    if (!REXCVAR_GET(qol_probe_save_slots))
    {
        return;
    }

    std::array<char, 43> slot{};
    const bool           valid = TryReadGuestText(r29.u32, slot);
    REXLOG_INFO(
        "QoL save-slot probe: save-end slot={} valid={} success={}",
        valid ? slot.data() : "<unavailable>",
        valid,
        r25.u32 != 0);
}

void ReRevvedCompatNullOptionalDispatch(PPCRegister& r0, PPCRegister& r3)
{
    if (r0.u32 == 0)
    {
        r3.u64 = 0;
    }
}

void ReRevvedCompatRingInitializeBegin(PPCRegister&, PPCRegister&)
{
    auto* graphics_system = GetGraphicsSystem();
    resume_after_ring_initialize =
        graphics_system && graphics_system->PauseAndResetGpuWritePointer();
}

void ReRevvedCompatRingInitializeEnd()
{
    auto* graphics_system = GetGraphicsSystem();
    if (graphics_system && resume_after_ring_initialize)
    {
        graphics_system->ResumeGpu();
    }
    resume_after_ring_initialize = false;
}

void ReRevvedRememberGfxRenderConfig(PPCRegister& r3, PPCRegister& r4)
{
    gfx_render_config_candidate = r3.u32;
    gfx_render_config_renderer  = r4.u32;
}

void ReRevvedHandleGfxRenderCapsBegin(PPCRegister& r3,
                                      PPCRegister& r4,
                                      uint64_t     lr)
{
    if (gfx_render_caps.observing)
    {
        return;
    }

    gfx_render_caps = {
        r3.u32,
        r4.u32,
        static_cast<uint32_t>(lr),
        true,
    };
}

void ReRevvedHandleGfxRenderCapsEnd(PPCRegister& r3, PPCRegister& r31)
{
    if (!gfx_render_caps.observing)
    {
        return;
    }

    const auto state          = gfx_render_caps;
    gfx_render_caps           = {};
    const bool output_matches = r31.u32 == state.output;
    const bool output_readable =
        output_matches && IsGuestReadableRange(r31.u32, 16);
    // GFx copies the failed query output, so retain that config and refresh it
    // only from a valid result produced by the same renderer.
    if (state.caller == 0x82245130 && r3.u32 == 0 &&
        gfx_render_config_renderer == state.renderer &&
        IsGuestReadableRange(gfx_render_config_candidate + 0x14, 0x18))
    {
        gfx_stale_render_config.store(gfx_render_config_candidate,
                                      std::memory_order_release);
        gfx_stale_render_config_renderer.store(state.renderer,
                                               std::memory_order_release);
    }
    else if (r3.u32 != 0 && output_readable &&
             gfx_stale_render_config_renderer.load(std::memory_order_acquire) ==
                 state.renderer)
    {
        const uint32_t config =
            gfx_stale_render_config.load(std::memory_order_acquire);
        if (config != 0 && IsGuestReadableRange(config + 0x14, 0x18) &&
            ReadGuestU32(config + 0x14) == state.renderer &&
            WriteGuestU32Safely(config + 0x24, ReadGuestU32(r31.u32)) &&
            WriteGuestU32Safely(config + 0x28, ReadGuestU32(r31.u32 + 4)))
        {
            gfx_stale_render_config.store(0, std::memory_order_release);
            gfx_stale_render_config_renderer.store(0, std::memory_order_release);
        }
    }
}
