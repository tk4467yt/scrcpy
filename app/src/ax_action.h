//
//
//

#ifndef AX_ACTION_H
#define AX_ACTION_H

#include "input_manager.h"

int ax_start_action(const char *serial, struct sc_input_manager *sc_im);
int ax_stop_action(void);

void ax_update_client_info(int screen_width, int screen_height, int video_codec_id);

bool ax_should_send_video(void);
void ax_send_videoPacket(AVPacket *packet);
void ax_set_missed_key_packet(AVPacket *keyPacket);

/// @brief client begin video
void sendBeginVideoPacket(void);

void auto_scroll_handle_4_left(void);

#endif // end AX_ACTION_H