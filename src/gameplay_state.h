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
};

Snapshot ReadGuestSnapshot();
void     PublishFrameSnapshot();
bool     GetPublishedSnapshot(Snapshot& out);

} // namespace rerevved::gameplay
