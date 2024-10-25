#ifndef AP_PACKET_H
#define AP_PACKET_H

#include <stdlib.h>
#include <libavcodec/avcodec.h>

size_t apPacketHeaderLength(int contentType);

char *makeAPPacket(char *out_buf, char *cmd_str);
int makeAPVideoPacket(AVPacket *packet, uint8_t *out_buf);

char *makeNextUVUniqueID(void);

size_t getAPPacketLength(char *packet_buf);
int getAPPacketContentType(char *packet_buf);

char *makeAXCommand(char *cmd, char *content);
char *makeSetClientInfoJson(char *clientID, int screen_width, int screen_height);
char *makeBeginVideoModeJson(uint32_t codec_id);

#endif // end AP_PACKET_H