// Hooks preserve title behavior unless documented as a compatibility repair.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(qol_probe_gameplay_frame, false, "ReRevved/Diagnostics", "Log the candidate playable-frame cadence and thread")
    .debug_only();
REXCVAR_DEFINE_BOOL(qol_probe_gameplay_state, false, "ReRevved/Diagnostics", "Log conservative gameplay-state gate transitions")
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

struct GameplayStateSnapshot
{
    uint32_t frontend_root     = 0;
    uint32_t frontend_state    = 0;
    uint32_t frontend_key      = UINT32_MAX;
    uint32_t active_player     = UINT32_MAX;
    uint32_t human_player_mask = 0;
    uint32_t interface_gate    = 0;
    bool     gameplay_active   = false;
    bool     turn_owner_known  = false;
    bool     human_turn        = false;
    bool     interface_update  = false;
    bool     api_available     = false;

    bool operator==(const GameplayStateSnapshot&) const = default;
};

struct GameplayStateProbeState
{
    bool                  active = false;
    uint64_t              frames = 0;
    GameplayStateSnapshot last;
};

thread_local GameplayStateProbeState gameplay_state_probe{};

constexpr uint32_t kInterfaceGateGlobal   = 0x8314F28C;
constexpr uint32_t kFrontendRootGlobal    = 0x82FFD624;
constexpr uint32_t kActivePlayerGlobal    = 0x8312B8E8;
constexpr uint32_t kHumanPlayerMaskGlobal = 0x8312E608;

GameplayStateSnapshot ReadGameplayState()
{
    GameplayStateSnapshot state{};

    if (TryReadGuestU32(kFrontendRootGlobal, state.frontend_root) &&
        state.frontend_root != 0 &&
        TryReadGuestU32(state.frontend_root + 0x70, state.frontend_state) &&
        state.frontend_state != 0 &&
        TryReadGuestU32(state.frontend_state + 4, state.frontend_key))
    {
        state.gameplay_active = state.frontend_key == 2;
    }

    if (TryReadGuestU32(kActivePlayerGlobal, state.active_player) &&
        TryReadGuestU32(kHumanPlayerMaskGlobal, state.human_player_mask) &&
        state.active_player < 32 && state.human_player_mask != 0)
    {
        state.turn_owner_known = true;
        state.human_turn =
            (state.human_player_mask & (uint32_t{ 1 } << state.active_player)) != 0;
    }

    uint8_t byte = 0;
    if (TryReadGuestU32(kInterfaceGateGlobal, state.interface_gate) &&
        state.interface_gate != 0 &&
        TryReadGuestU8(state.interface_gate + 5, byte))
    {
        state.interface_update = byte != 0;
    }

    state.api_available = state.gameplay_active && state.interface_update &&
                          state.turn_owner_known && state.human_turn;
    return state;
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
    const bool probe_frame = REXCVAR_GET(qol_probe_gameplay_frame);
    const bool probe_state = REXCVAR_GET(qol_probe_gameplay_state);
    if (!probe_frame && !probe_state)
    {
        gameplay_frame_probe = {};
        gameplay_state_probe = {};
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

    if (probe_state)
    {
        ++gameplay_state_probe.frames;
        const GameplayStateSnapshot state = ReadGameplayState();
        if (!gameplay_state_probe.active ||
            !(state == gameplay_state_probe.last))
        {
            gameplay_state_probe.active = true;
            gameplay_state_probe.last   = state;
            REXLOG_INFO(
                "QoL state probe: frame={} frontend_root={:08X} "
                "frontend_state={:08X} frontend_key={:08X} gameplay_active={} "
                "active_player={:08X} human_mask={:08X} turn_owner_known={} "
                "human_turn={} interface_gate={:08X} interface_update={} "
                "api_available={}",
                gameplay_state_probe.frames,
                state.frontend_root,
                state.frontend_state,
                state.frontend_key,
                state.gameplay_active,
                state.active_player,
                state.human_player_mask,
                state.turn_owner_known,
                state.human_turn,
                state.interface_gate,
                state.interface_update,
                state.api_available);
        }
    }
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

void ReRevvedCompatExpandGfxVectorGlyphCache(PPCRegister& r31)
{
    constexpr uint32_t kManagerField          = 72;
    constexpr uint32_t kVectorCacheLimitField = 2512;
    constexpr uint32_t kDefaultLimit          = 512;
    constexpr uint32_t kCompatLimit           = 1024;

    if (!IsGuestReadableRange(r31.u32, kManagerField + sizeof(uint32_t)))
    {
        return;
    }
    const uint32_t manager = ReadGuestU32(r31.u32 + kManagerField);
    if (manager > UINT32_MAX - kVectorCacheLimitField)
    {
        return;
    }
    const uint32_t limit = manager + kVectorCacheLimitField;
    if (IsGuestReadableRange(limit, sizeof(uint32_t)) &&
        ReadGuestU32(limit) == kDefaultLimit)
    {
        WriteGuestU32Safely(limit, kCompatLimit);
    }
}
