//
//
//
#include <stdlib.h>

#include "scrcpy.h"
#include "util/log.h"
#include "util/thread.h"
#include "util/vecdeque.h"
#include "screen.h"
#include "input_manager.h"

// MIN, MAX macro redefined in "common.h" and <sys/param.h>
// so declare uv.h after scrcpy's header
#include <uv.h>

#include "ax_action.h"
#include "cJSON.h"

// packets define
#define AX_JSON_COMMAND_SET_CLIENT_INFO "set_client_info" // set client info (client ==>> server)
#define AX_JSON_COMMAND_AUTO_SCROLL "auto_scroll" // let client auto scroll (server ==>> client)
#define AX_JSON_COMMAND_SWITCH_TO_VIDEO_MODE "switch_to_video_mode" // let client switch to video mode (server ==>> client)
#define AX_JSON_COMMAND_CLIENT_BEGIN_VIDEO "client_begin_video" // client begin transfer video AVPacket from next packet (client ==>> server)

#define AX_JSON_CONTENT_KEY_COMMAND "command"
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

#define AX_STREAM_CONTENT_TYPE_RAW_VIDEO 1
#define AX_STREAM_CONTENT_TYPE_JSON 2

// libuv relate
#define AX_SERIAL_MAX_LEN 128

#define AX_SERVER_ADDR "127.0.0.1"
#define AX_SERVER_PORT 10748

#define AX_REPEAT_TIMER_REPEAT_VAL 20 // ms

static sc_mutex ax_mutex;

static bool ax_thread_started = false;
static sc_thread ax_thread;

static uv_loop_t axUVLoop;
static uv_tcp_t tcpClientSocket;
static bool ax_running = false;

static uv_async_t stop_async;

static char android_serial[AX_SERIAL_MAX_LEN];
static struct sc_input_manager *ax_sc_im = NULL;
static int last_send_screen_width = 0;
static int last_send_screen_height = 0;
static int sc_screen_width = 0;
static int sc_screen_height = 0;
static uint32_t av_video_codec_id = 0;

#define AX_BUF_SIZE 4096
static char ax_content_buf[AX_BUF_SIZE];
static char ax_cmd_buf[AX_BUF_SIZE];

#define AX_READED_BUF_SIZE 8192
static char ax_readed_buf[AX_READED_BUF_SIZE];
static size_t readed_buf_used_size = 0;

#define ax_touch_type_down 0
#define ax_touch_type_up 1
#define ax_touch_type_move 2
struct ax_touch_action
{
    int touch_type; // like ax_touch_type_down

    int touch_x;
    int touch_y;

    int expire_count; // if > 0, valid
};
struct ax_touch_queue SC_VECDEQUE(struct ax_touch_action);
struct ax_touch_queue ax_pending_touch_queue;
struct ax_touch_action handling_touch_action = {0, 0, 0, -1};

// touch utility
static void add_ax_touch_action(struct ax_touch_action touchAction)
{
    sc_vecdeque_push(&ax_pending_touch_queue, touchAction);
}

// packets utility
static char * makeAXPacket(char *packet_buf, char *cmd_str)
{
    size_t cmd_len = strlen(cmd_str);
    size_t packet_len = cmd_len + AX_PACKET_HEADER_LEN;

    packet_buf[0] = (packet_len >> 8) & 0xff;
    packet_buf[1] = packet_len & 0xff;
    packet_buf[2] = AX_PACKET_HEADER_LEN;
    packet_buf[3] = AX_STREAM_CONTENT_TYPE_JSON;

    memcpy(packet_buf + AX_PACKET_HEADER_LEN, cmd_str, cmd_len);

    return packet_buf;
}

static char * makeNextUVUniqueID()
{
    static char unique_id_buf[100];
    static int lastUVUniqueID = 0;
    
    ++lastUVUniqueID;

    SDL_itoa(lastUVUniqueID, unique_id_buf, 10);

    return unique_id_buf;
}

static char * makeAXCommand(char *cmd, char *content)
{
    cJSON *cmdJson = cJSON_CreateObject();
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_COMMAND, cmd);
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_UNIQUE_ID, makeNextUVUniqueID());
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_CONTENT, content);

    cJSON_PrintPreallocated(cmdJson, ax_cmd_buf, AX_BUF_SIZE, false);

    cJSON_Delete(cmdJson);

    return ax_cmd_buf;
}

static char * makeSetClientInfoJson(char *clientID, int screen_width, int screen_height)
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

static char * makeBeginVideoModeJson()
{
    cJSON *contentJson = cJSON_CreateObject();
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_AV_VIDEO_CODEC_ID, av_video_codec_id);

    cJSON_PrintPreallocated(contentJson, ax_content_buf, AX_BUF_SIZE, false);

    cJSON_Delete(contentJson);

    char *cmd_str = makeAXCommand(AX_JSON_COMMAND_CLIENT_BEGIN_VIDEO, ax_content_buf);

    return cmd_str;
}

static int add_to_readed_buf(const uv_buf_t buf)
{
    if (readed_buf_used_size + buf.len > AX_READED_BUF_SIZE) {
        LOGE("AX add_to_readed_buf failed: %d %d", (int)readed_buf_used_size, (int)buf.len);
        return SCRCPY_EXIT_FAILURE;
    }

    memcpy(ax_readed_buf+readed_buf_used_size, buf.base, buf.len);
    readed_buf_used_size += buf.len;

    return SCRCPY_EXIT_SUCCESS;
}

static int remove_readed_buf_head(size_t count)
{
    if (0 == count) {
        return SCRCPY_EXIT_SUCCESS;
    }
    if (count > readed_buf_used_size) {
        LOGE("AX remove_readed_buf_head failed: %d", (int)count);

        return SCRCPY_EXIT_FAILURE;
    }

    if (count == readed_buf_used_size) {
        //all data removed, do not touch innerBuffer
        readed_buf_used_size = 0;
    } else {
        readed_buf_used_size -= count;

        memmove(ax_readed_buf, ax_readed_buf+count, readed_buf_used_size);
    }

    return SCRCPY_EXIT_SUCCESS;
}


// libuv utility
static int make_ax_jitter(int limit)
{
    int retJitter = 0;

    int jitter_val = rand() % limit;
    int jitter_direction = rand() % 2;

    if (jitter_direction > 0) {
        retJitter = jitter_val;
    } else {
        retJitter = 0 - jitter_val;
    }

    return retJitter;
}
static void auto_scroll_handle_4_up()
{
    LOGD("auto scroll up");

    int jitter_limit_small = 100;
    int jitter_limit_large = 300;

    int start_x = last_send_screen_width / 2 + make_ax_jitter(jitter_limit_large);
    int start_y = last_send_screen_height * 3 / 4  + make_ax_jitter(jitter_limit_small);

    int end_x = start_x + make_ax_jitter(jitter_limit_small);
    int end_y = start_y - last_send_screen_height / 4 + make_ax_jitter(jitter_limit_small);

    // start touch
    struct ax_touch_action beginTouch;
    beginTouch.touch_type = ax_touch_type_down;
    beginTouch.touch_x = start_x;
    beginTouch.touch_y = start_y;
    beginTouch.expire_count = 0;

    add_ax_touch_action(beginTouch);

    // moving
    int jitter_step = 2;
    int steps = 10 + make_ax_jitter(jitter_step);
    int step_x = (end_x - start_x) / steps;
    int step_y = (end_y - start_y) / steps;
    int expire_count = 0;
            
    int moving_x = start_x;
    int moving_y = start_y;
    while (moving_y > end_y) {
        struct ax_touch_action moveTouch;
        moveTouch.touch_type = ax_touch_type_move;
        moveTouch.touch_x = moving_x;
        moveTouch.touch_y = moving_y;
        moveTouch.expire_count = expire_count;

        add_ax_touch_action(moveTouch);

        moving_x += step_x;
        moving_y += step_y;
    }

    // end touch
    struct ax_touch_action endTouch;
    endTouch.touch_type = ax_touch_type_up;
    endTouch.touch_x = end_x;
    endTouch.touch_y = end_y;
    endTouch.expire_count = 0;

    add_ax_touch_action(endTouch);
}

void auto_scroll_handle_4_left()
{
    LOGD("auto scroll left");

    int jitter_limit_small = 100;
    int jitter_limit_large = 500;

    int start_x = last_send_screen_width * 3 / 4 + make_ax_jitter(jitter_limit_small);
    int start_y = last_send_screen_height / 2  + make_ax_jitter(jitter_limit_large);

    int end_x = start_x - last_send_screen_height / 4 + make_ax_jitter(jitter_limit_small);
    int end_y = start_y + make_ax_jitter(jitter_limit_small);

    // start touch
    struct ax_touch_action beginTouch;
    beginTouch.touch_type = ax_touch_type_down;
    beginTouch.touch_x = start_x;
    beginTouch.touch_y = start_y;
    beginTouch.expire_count = 0;

    add_ax_touch_action(beginTouch);

    // moving
    int jitter_step = 2;
    int steps = 10 + make_ax_jitter(jitter_step);
    int step_x = (end_x - start_x) / steps;
    int step_y = (end_y - start_y) / steps;
    int expire_count = 0;
            
    int moving_x = start_x;
    int moving_y = start_y;
    while (moving_x > end_x) {
        struct ax_touch_action moveTouch;
        moveTouch.touch_type = ax_touch_type_move;
        moveTouch.touch_x = moving_x;
        moveTouch.touch_y = moving_y;
        moveTouch.expire_count = expire_count;

        add_ax_touch_action(moveTouch);

        moving_x += step_x;
        moving_y += step_y;
    }

    // end touch
    struct ax_touch_action endTouch;
    endTouch.touch_type = ax_touch_type_up;
    endTouch.touch_x = end_x;
    endTouch.touch_y = end_y;
    endTouch.expire_count = 0;

    add_ax_touch_action(endTouch);
}

static void handle_ax_json_cmd(const uv_buf_t buf)
{
    cJSON *cmdJson = cJSON_ParseWithLength(buf.base, buf.len);
    cJSON_PrintPreallocated(cmdJson, ax_cmd_buf, AX_BUF_SIZE, false);

    LOGD("ax received cmd: %s", ax_cmd_buf);

    char *innerCmd = cJSON_GetStringValue(cJSON_GetObjectItem(cmdJson, AX_JSON_CONTENT_KEY_COMMAND));
    int innerErrCode = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cmdJson, AX_JSON_CONTENT_KEY_ERR_CODE));

    if (AX_ERR_CODE_SUCCESS == innerErrCode) {
        // LOGD("%s success", innerCmd);
        if (strcmp(innerCmd, AX_JSON_COMMAND_SET_CLIENT_INFO) == 0) {
            
        } else if (strcmp(innerCmd, AX_JSON_COMMAND_AUTO_SCROLL) == 0) {
            // origin at left-top

            char *innerContent = cJSON_GetStringValue(cJSON_GetObjectItem(cmdJson, AX_JSON_CONTENT_KEY_CONTENT));

            if (strcmp(innerContent, AX_SCROLL_DIRECTION_UP) == 0) {
                auto_scroll_handle_4_up();
            } else if (strcmp(innerContent, AX_SCROLL_DIRECTION_LEFT) == 0) {
                auto_scroll_handle_4_left();
            }
        } else if (strcmp(innerCmd, AX_JSON_COMMAND_SWITCH_TO_VIDEO_MODE) == 0) {
            sendBeginVideoPacket();
        }
    } else {
        LOGE("AX cmd: %s failed", innerCmd);
    }
    
    cJSON_Delete(cmdJson);
}

static void on_require_alloc_buf(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = (char *)malloc(AX_BUF_SIZE);
    buf->len = AX_BUF_SIZE;
}

static void ax_release_uv_buf(const uv_buf_t* buf)
{
    free(buf->base);
}

static void on_tcp_wrote_data(uv_write_t *req, int status) 
{
    LOGD("AX onTcpWroteData status: %d", status);

    if (status) {
        LOGE("AX onTcpWroteData failed: %s", uv_strerror(status));
    }

    // free
    uv_buf_t tmpBuf;
    tmpBuf.base = (char *)req->data;
    tmpBuf.len = AX_BUF_SIZE;

    ax_release_uv_buf(&tmpBuf);

    free(req);
}

static void sendAXCommand(char *cmd_str)
{
    LOGI("AX send cmd: %s", cmd_str);

    uv_write_t *ax_writer = (uv_write_t *)malloc(sizeof(uv_write_t));
    uv_buf_t writer_buf;
    on_require_alloc_buf(NULL, 0, &writer_buf);

    makeAXPacket(writer_buf.base, cmd_str);
    writer_buf.len = strlen(cmd_str) + AX_PACKET_HEADER_LEN;

    ax_writer->data = (void *)writer_buf.base;

    uv_write(ax_writer, (uv_stream_t *)&tcpClientSocket, &writer_buf, 1, on_tcp_wrote_data);
}

static void check_report_client_info()
{
    sc_mutex_lock(&ax_mutex);

    bool screen_size_updated = false;

    if (sc_screen_width != last_send_screen_width || sc_screen_height != last_send_screen_height) {
        last_send_screen_width = sc_screen_width;
        last_send_screen_height = sc_screen_height;

        screen_size_updated = true;
    }

    sc_mutex_unlock(&ax_mutex);

    if (screen_size_updated) {
        char *cmd_str = makeSetClientInfoJson(android_serial, last_send_screen_width, last_send_screen_height);

        sendAXCommand(cmd_str);
    }
}

void sendBeginVideoPacket()
{
    char *cmd_str = makeBeginVideoModeJson();
    
     sendAXCommand(cmd_str);
}

static void onAXRepeatTimerExpired(uv_timer_t *handle)
{
    (void)handle;

    check_report_client_info();

    if (handling_touch_action.expire_count < 0) {
        // handling touch_action invalid
        if (sc_vecdeque_is_empty(&ax_pending_touch_queue)) {
            // queue empty, nothing todo
        } else {
            handling_touch_action = sc_vecdeque_pop(&ax_pending_touch_queue);
        }
    }
    
    if (handling_touch_action.expire_count >= 0) {
        handling_touch_action.expire_count -= 1;

        if (handling_touch_action.expire_count < 0) {
            // LOGD("consumed touch_action: type(%d) --- x(%d) --- y(%d)", 
            //     handling_touch_action.touch_type, 
            //     handling_touch_action.touch_x, 
            //     handling_touch_action.touch_y);

            struct sc_point touchPoint = {.x = handling_touch_action.touch_x, .y = handling_touch_action.touch_y};
            
            if (handling_touch_action.touch_type == ax_touch_type_down || handling_touch_action.touch_type == ax_touch_type_up) {
                struct sc_mouse_click_event clickEvt = {
                    .position = {
                        .screen_size = ax_sc_im->screen->frame_size,
                        .point = touchPoint,
                    },
                    .action = handling_touch_action.touch_type == ax_touch_type_down ? SC_ACTION_DOWN : SC_ACTION_UP,
                    .button = SC_MOUSE_BUTTON_LEFT,
                    .pointer_id = ax_sc_im->forward_all_clicks ? POINTER_ID_MOUSE : POINTER_ID_GENERIC_FINGER,
                    .buttons_state = handling_touch_action.touch_type == ax_touch_type_down ? SC_MOUSE_BUTTON_LEFT : 0,
                };
                ax_sc_im->mp->ops->process_mouse_click(ax_sc_im->mp, &clickEvt);
            } else if (handling_touch_action.touch_type == ax_touch_type_move) {
                struct sc_mouse_motion_event motionEvt = {
                    .position = {
                        .screen_size = ax_sc_im->screen->frame_size,
                        .point = touchPoint,
                    },
                    .pointer_id = ax_sc_im->forward_all_clicks ? POINTER_ID_MOUSE : POINTER_ID_GENERIC_FINGER,
                    .xrel = 0,
                    .yrel = 0,
                    .buttons_state = SC_MOUSE_BUTTON_LEFT,
                };

                ax_sc_im->mp->ops->process_mouse_motion(ax_sc_im->mp, &motionEvt);
            }
        }
    }
}

static int handle_received_data(const uv_buf_t buf)
{
    int retVal = add_to_readed_buf(buf);

    if (SCRCPY_EXIT_SUCCESS == retVal) {
        while (readed_buf_used_size >= AX_PACKET_HEADER_LEN) {
            uint8_t ub0 = ax_readed_buf[0];
            uint8_t ub1 = ax_readed_buf[1];
            uint8_t ub2 = ax_readed_buf[2];
            uint8_t ub3 = ax_readed_buf[3];

            size_t packet_len = ub0 * 256 + ub1;
            size_t header_len = ub2;
            int content_type = ub3;

            if (packet_len < AX_PACKET_HEADER_LEN || 
                header_len != AX_PACKET_HEADER_LEN || 
                content_type != AX_STREAM_CONTENT_TYPE_JSON) {
                retVal = SCRCPY_EXIT_FAILURE;
                LOGE("AX invalid packet header");
                break;
            }

            if (packet_len > readed_buf_used_size) {
                LOGI("AX no enough packet data");
                break;
            } else {
                handle_ax_json_cmd(uv_buf_init(ax_readed_buf + AX_PACKET_HEADER_LEN, packet_len - AX_PACKET_HEADER_LEN));

                retVal = remove_readed_buf_head(packet_len);
            }
        }
    }

    return retVal;
}

static void on_readed_data(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    LOGD("on_readed_data: %d", (int)nread);

    if (nread > 0) {
        int retVal = handle_received_data(uv_buf_init(buf->base, nread));
        if (SCRCPY_EXIT_SUCCESS != retVal) {
            LOGE("AX on_readed_data handle failed");
            uv_close((uv_handle_t*) stream, NULL);

            uv_async_send(&stop_async);
        }
    } else {
        if (nread < 0) {
            if (nread != UV_EOF) {
                LOGE("AX uv_tcp_connect failed: %s", uv_strerror(nread));
            }
            
            uv_close((uv_handle_t*) stream, NULL);
            uv_async_send(&stop_async);
        }
    }

    ax_release_uv_buf(buf);
}

static void on_tcp_connected(uv_connect_t* req, int status)
{
    if (status) {
        LOGE("AX uv_tcp_connect failed: %s", uv_strerror(status));
        ax_running = false;
        return;
    }
    LOGI("AX tcp connected");

    uv_read_start((uv_stream_t *)req->handle, on_require_alloc_buf, on_readed_data);
}

static void stop_async_cb(uv_async_t* handle)
{
    (void)handle;

    if (ax_running) {
        LOGI("AX stop async callback called");

        uv_stop(&axUVLoop);
    }
}

static int ax_thread_cb(void *data) 
{
    (void)data;
    LOGI("AX thread running");

    sc_vecdeque_init(&ax_pending_touch_queue);

    uv_loop_init(&axUVLoop);

    uv_async_init(&axUVLoop, &stop_async, stop_async_cb);

    uv_timer_t repeatTimer;
    uv_timer_init(&axUVLoop, &repeatTimer);
    uv_timer_start(&repeatTimer, onAXRepeatTimerExpired, 1000, AX_REPEAT_TIMER_REPEAT_VAL);

    
    uv_tcp_init(&axUVLoop, &tcpClientSocket);

    struct sockaddr_in serverAddr;
    uv_ip4_addr(AX_SERVER_ADDR, AX_SERVER_PORT, &serverAddr);

    uv_connect_t tcpConnector;

    int retVal = uv_tcp_connect(&tcpConnector, &tcpClientSocket, (const struct sockaddr*)&serverAddr, on_tcp_connected);
    if (retVal) {
        LOGI("AX uv_tcp_connect failed: %s", uv_strerror(retVal));
        return SCRCPY_EXIT_FAILURE;
    }

    LOGI("AX uv_run running");
    ax_running = true;

    retVal = uv_run(&axUVLoop, UV_RUN_DEFAULT);
    if (retVal) {
        LOGI("AX uv_run broken");
    }

    uv_loop_close(&axUVLoop);

    ax_running = false;
    sc_vecdeque_destroy(&ax_pending_touch_queue);

    return SCRCPY_EXIT_SUCCESS;
}

int ax_start_action(const char *serial, struct sc_input_manager *sc_im)
{
    LOGI("AX starting action: %s", serial);

    sc_mutex_init(&ax_mutex);

    size_t serialLen = strlen(serial);
    if (serialLen > AX_SERIAL_MAX_LEN) {
        LOGE("AX serial len too large: %d", (int)serialLen);
        return SCRCPY_EXIT_FAILURE;
    }
    strcpy(android_serial, serial);

    ax_sc_im = sc_im;

    bool ok = sc_thread_create(&ax_thread, ax_thread_cb, "ax_thread_name", "");
    if (!ok) {
        LOGE("AX thread create failed");
        return SCRCPY_EXIT_FAILURE;
    }
    ax_thread_started = true;

    return SCRCPY_EXIT_SUCCESS;
}

int ax_stop_action()
{
    LOGI("AX stopping action");

    if (ax_thread_started && ax_running) {
        uv_async_send(&stop_async);
        
        sc_thread_join(&ax_thread, NULL);
    }

    sc_mutex_destroy(&ax_mutex);
    
    return SCRCPY_EXIT_SUCCESS;
}

void ax_update_client_info(int screen_width, int screen_height, int video_codec_id)
{
    if (ax_thread_started && screen_width > 0 && screen_height > 0) {
        sc_mutex_lock(&ax_mutex);

        if (sc_screen_width != screen_width || sc_screen_height != screen_height) {
            sc_screen_width = screen_width;
            sc_screen_height = screen_height;
            av_video_codec_id = video_codec_id;
        }

        sc_mutex_unlock(&ax_mutex);
    }
}

