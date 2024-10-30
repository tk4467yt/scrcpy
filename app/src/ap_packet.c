#include <stdlib.h>

#include "util/log.h"

// MIN, MAX macro redefined in "common.h" and <sys/param.h>
// so declare uv.h after scrcpy's header
// #include <uv.h>

#include "cJSON.h"
#include "ap_packet.h"
#include "ap_defines.h"

static char ax_cmd_buf[AX_BUF_SIZE];
static char ax_content_buf[AX_BUF_SIZE];

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

int makeAPVideoPacket(AVPacket *packet, uint8_t *out_buf)
{
    int retLen = -1;

    int pktSize = packet->size;
    int headerSize = apPacketHeaderLength(AP_STREAM_CONTENT_TYPE_RAW_VIDEO);
    int totalSize = headerSize + pktSize;
    if (totalSize > AX_SEND_RAW_VIDEO_BUFFER_MAX_LEN)
    {
        LOGE("AX video too long: %d", totalSize);
        return retLen;
    }

    // total size
    out_buf[0] = (totalSize >> 24) & 0xff;
    out_buf[1] = (totalSize >> 16) & 0xff;
    out_buf[2] = (totalSize >> 8) & 0xff;
    out_buf[3] = totalSize & 0xff;
    // type
    out_buf[4] = AP_STREAM_CONTENT_TYPE_RAW_VIDEO;
    // pts
    int64_t pts = packet->pts;
    out_buf[5] = (pts >> 56) & 0xff;
    out_buf[6] = (pts >> 48) & 0xff;
    out_buf[7] = (pts >> 40) & 0xff;
    out_buf[8] = (pts >> 32) & 0xff;
    out_buf[9] = (pts >> 24) & 0xff;
    out_buf[10] = (pts >> 16) & 0xff;
    out_buf[11] = (pts >> 8) & 0xff;
    out_buf[12] = pts & 0xff;
    // flag
    int flags = packet->flags;
    out_buf[13] = (flags >> 24) & 0xff;
    out_buf[14] = (flags >> 16) & 0xff;
    out_buf[15] = (flags >> 8) & 0xff;
    out_buf[16] = flags & 0xff;

    memcpy(out_buf + headerSize, packet->data, pktSize);

    retLen = totalSize;

    return retLen;
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

char *makeAXCommand(char *cmd, char *content)
{
    cJSON *cmdJson = cJSON_CreateObject();
    cJSON_AddStringToObject(cmdJson, AX_JSON_KEY_COMMAND, cmd);
    cJSON_AddStringToObject(cmdJson, AX_JSON_KEY_UNIQUE_ID, makeNextUVUniqueID());
    cJSON_AddStringToObject(cmdJson, AX_JSON_KEY_CONTENT, content);

    cJSON_PrintPreallocated(cmdJson, ax_cmd_buf, AX_BUF_SIZE, false);

    cJSON_Delete(cmdJson);

    return ax_cmd_buf;
}

char *makeSetClientInfoJson(char *clientID, int screen_width, int screen_height)
{
    cJSON *contentJson = cJSON_CreateObject();
    cJSON_AddStringToObject(contentJson, AX_JSON_KEY_CLIENT_ID, clientID);
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_SCREEN_WIDTH, screen_width);
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_SCREEN_HEIGHT, screen_height);

    cJSON_PrintPreallocated(contentJson, ax_content_buf, AX_BUF_SIZE, false);

    cJSON_Delete(contentJson);

    char *cmd_str = makeAXCommand(AX_JSON_COMMAND_SET_CLIENT_INFO, ax_content_buf);

    return cmd_str;
}

char *makeBeginVideoModeJson(uint32_t codec_id)
{
    cJSON *contentJson = cJSON_CreateObject();
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_AV_VIDEO_CODEC_ID, codec_id);

    cJSON_PrintPreallocated(contentJson, ax_content_buf, AX_BUF_SIZE, false);

    cJSON_Delete(contentJson);

    char *cmd_str = makeAXCommand(AX_JSON_COMMAND_CLIENT_BEGIN_VIDEO, ax_content_buf);

    return cmd_str;
}
