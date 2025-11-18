#ifndef CONTROL_PACKETS_H
#define CONTROL_PACKETS_H

#include <steamnetworkingtypes.h>

void handleControlPacket(const char* data, size_t size, HSteamNetConnection conn);

#endif // CONTROL_PACKETS_H