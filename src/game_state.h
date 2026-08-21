#pragma once

#include <cstdint>

namespace rerevved::gameplay
{

void PublishFrameSnapshot();
void ProbePublishedSnapshot(bool enabled);
void ProbeCalendarState(bool enabled);
void ReadProbePlayerState(uint32_t& active_player, uint32_t& human_player_mask);

} // namespace rerevved::gameplay
