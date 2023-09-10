//
//
//

#ifndef AX_ACTION_H
#define AX_ACTION_H

#include "input_manager.h"

int ax_start_action(const char *serial, struct sc_input_manager *sc_im);
int ax_stop_action();

void ax_update_client_info(int screen_width, int screen_height, int video_codec_id);

bool ax_should_send_video();
void ax_send_videoPacket(uint8_t *data, int length);

/// @brief client begin video
void sendBeginVideoPacket();

#endif // end AX_ACTION_H