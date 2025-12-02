#pragma once
#include <cstdint>

// Packet types for our binary protocol
enum PacketType : uint8_t {
    PKT_JOIN  = 1,   // client joins a group
    PKT_MSG   = 2,   // normal chat message
    PKT_LEAVE = 3    // client leaves
};

// Binary packet format used between client and server
struct ChatPacket {
    uint8_t  type;        // PacketType
    uint16_t groupID;     // chat group ID
    uint32_t timestamp;   // set by server
    char     payload[256];// message text (null-terminated if shorter)
};
