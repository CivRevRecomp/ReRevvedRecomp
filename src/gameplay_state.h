#pragma once

#include <cstdint>

namespace rerevved::gameplay
{

struct Snapshot
{
    uint64_t frame_sequence    = 0;
    uint32_t frontend_root     = 0;
    uint32_t frontend_state    = 0;
    uint32_t frontend_key      = UINT32_MAX;
    uint32_t active_player     = UINT32_MAX;
    uint32_t human_player_mask = 0;
    uint32_t interface_gate    = 0;
    bool     frontend_known    = false;
    bool     gameplay_active   = false;
    bool     turn_owner_known  = false;
    bool     human_turn        = false;
    bool     interface_known   = false;
    bool     interface_update  = false;
    bool     available         = false;

    bool SameState(const Snapshot& other) const
    {
        return frontend_root == other.frontend_root &&
               frontend_state == other.frontend_state &&
               frontend_key == other.frontend_key &&
               active_player == other.active_player &&
               human_player_mask == other.human_player_mask &&
               interface_gate == other.interface_gate &&
               frontend_known == other.frontend_known &&
               gameplay_active == other.gameplay_active &&
               turn_owner_known == other.turn_owner_known &&
               human_turn == other.human_turn &&
               interface_known == other.interface_known &&
               interface_update == other.interface_update &&
               available == other.available;
    }
};

Snapshot ReadGuestSnapshot();
void     PublishFrameSnapshot();
bool     GetPublishedSnapshot(Snapshot& out);

} // namespace rerevved::gameplay
