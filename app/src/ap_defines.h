#ifndef AP_DEFINES_H
#define AP_DEFINES_H

#define AP_STREAM_CONTENT_TYPE_RAW_VIDEO 1
#define AP_STREAM_CONTENT_TYPE_JSON 2

// packets define
#define AX_JSON_COMMAND_SET_CLIENT_INFO "set_client_info" // set client info (client ==>> server)
#define AX_JSON_COMMAND_CLIENT_BEGIN_VIDEO "client_begin_video" // client begin transfer video AVPacket from next packet (client ==>> server)

#define AX_JSON_COMMAND_AUTO_SCROLL "auto_scroll" // let client auto scroll (server ==>> client)
#define AX_JSON_COMMAND_SWITCH_TO_VIDEO_MODE "switch_to_video_mode" // let client switch to video mode (server ==>> client)

#define AX_JSON_COMMAND_CMD_RESPONSE "cmd_response" // response to received command (client <<==>> server)

#define AX_JSON_CONTENT_KEY_COMMAND "command"
#define AX_JSON_CONTENT_KEY_RESPONSE_2_COMMAND "response_2_command"
#define AX_JSON_CONTENT_KEY_UNIQUE_ID "unique_id"
#define AX_JSON_CONTENT_KEY_CONTENT "content"
#define AX_JSON_CONTENT_KEY_ERR_CODE "err_code"

#define AX_SCROLL_DIRECTION_UP "up"
#define AX_SCROLL_DIRECTION_DOWN "down"
#define AX_SCROLL_DIRECTION_LEFT "left"
#define AX_SCROLL_DIRECTION_RIGHT "right"

#define AX_ERR_CODE_SUCCESS 0
#define AX_ERR_CODE_FAILED 1

#define AX_JSON_KEY_CLIENT_ID "client_id"
#define AX_JSON_KEY_SCREEN_WIDTH "screen_width"
#define AX_JSON_KEY_SCREEN_HEIGHT "screen_height"
#define AX_JSON_KEY_AV_VIDEO_CODEC_ID "av_video_codec_id"

#define AX_PACKET_HEADER_LEN 4

// libuv relate
#define AX_SERIAL_MAX_LEN 128

#define AX_SERVER_ADDR "127.0.0.1"
#define AX_SERVER_PORT 10748

#define AX_REPEAT_TIMER_REPEAT_VAL 20 // ms

#define AX_BUF_SIZE 4096
#define AX_SEND_RAW_VIDEO_BUFFER_MAX_LEN (10 * 1024 * 1024) // 10M

#endif // end AP_DEFINES_H