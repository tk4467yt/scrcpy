#include <stdlib.h>

#include "util/log.h"

// MIN, MAX macro redefined in "common.h" and <sys/param.h>
// so declare uv.h after scrcpy's header
// #include <uv.h>

// #include "cJSON.h"
#include "ap_packet.h"
#include "ap_defines.h"

size_t apPacketHeaderLength(int contentType)
{
    size_t retLen = 4 + 1;

    if (contentType == AP_STREAM_CONTENT_TYPE_RAW_VIDEO)
    {
        retLen += 12;
    }

    return retLen;
}

char *makeAPPacket(char *out_buf, char *cmd_str)
{
    size_t headerLen = apPacketHeaderLength(AP_STREAM_CONTENT_TYPE_JSON);
    size_t cmd_len = strlen(cmd_str);
    size_t packet_len = headerLen + cmd_len;

    out_buf[0] = (packet_len >> 24) & 0xff;
    out_buf[1] = (packet_len >> 16) & 0xff;
    out_buf[2] = (packet_len >> 8) & 0xff;
    out_buf[3] = packet_len & 0xff;

    out_buf[4] = AP_STREAM_CONTENT_TYPE_JSON;

    memcpy(out_buf + headerLen, cmd_str, cmd_len);

    return out_buf;
}

char *makeNextUVUniqueID(void)
{
    static char unique_id_buf[100];
    static int lastUVUniqueID = 0;

    ++lastUVUniqueID;

    SDL_itoa(lastUVUniqueID, unique_id_buf, 10);

    return unique_id_buf;
}

size_t getAPPacketLength(char *packet_buf)
{
    uint8_t ub0 = packet_buf[0];
    uint8_t ub1 = packet_buf[1];
    uint8_t ub2 = packet_buf[2];
    uint8_t ub3 = packet_buf[3];

    size_t packet_len = (ub0 << 24) + (ub1 << 16) + (ub2 << 8) + ub3;
    return packet_len;
}

int getAPPacketContentType(char *packet_buf)
{
    int content_type = packet_buf[4];
    return content_type;
}
