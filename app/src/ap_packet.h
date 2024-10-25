#ifndef AP_PACKET_H
#define AP_PACKET_H

#include <stdlib.h>

size_t apPacketHeaderLength(int contentType);

char *makeAPPacket(char *out_buf, char *cmd_str);

char *makeNextUVUniqueID(void);

size_t getAPPacketLength(char *packet_buf);
int getAPPacketContentType(char *packet_buf);

#endif // end AP_PACKET_H