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

#include "ap_action.h"
#include "ap_defines.h"
#include "ap_packet.h"
#include "cJSON.h"

static sc_mutex ax_mutex;
static struct sc_input_manager *ax_sc_im = NULL;

static bool ax_thread_started = false;
static sc_thread ax_thread;

static uv_loop_t axUVLoop;
static uv_tcp_t tcpClientSocket;
static uv_timer_t repeatTimer;

static bool ax_running = false;

static uv_async_t stop_async;
static uv_async_t send_video_async;

static char android_serial[AX_SERIAL_MAX_LEN];

static int last_send_screen_width = 0;
static int last_send_screen_height = 0;
static int sc_screen_width = 0;
static int sc_screen_height = 0;
static uint32_t av_video_codec_id = 0;

static char ax_print_log_buf[AX_BUF_SIZE];

#define AX_READED_BUF_SIZE 8192
static char ax_readed_buf[AX_READED_BUF_SIZE];
static size_t readed_buf_used_size = 0;

static bool client_should_send_video = false;

static uint8_t ax_sending_video_packet_buf[AX_SEND_RAW_VIDEO_BUFFER_MAX_LEN];
static int ax_sending_video_packet_length = 0;

#define ax_delayed_type_touch_down 0
#define ax_delayed_type_touch_up 1
#define ax_delayed_type_touch_move 2
#define ax_delayed_type_set_video_mode 3

struct ax_delayed_action
{
    int delayed_type; // like ax_delayed_type_touch_down

    int touch_x;
    int touch_y;

    int expire_count; // if > 0, valid
};

struct ax_delayed_action_queue SC_VECDEQUE(struct ax_delayed_action);
struct ax_delayed_action_queue ax_pending_delayed_queue;
struct ax_delayed_action handling_delayed_action = {0, 0, 0, -1};

static AVPacket *last_missed_key_packet = NULL;

// touch utility
static void add_ax_delayed_action(struct ax_delayed_action touchAction)
{
    sc_vecdeque_push(&ax_pending_delayed_queue, touchAction);
}

// packets utility

static int add_to_readed_buf(const uv_buf_t buf)
{
    if (readed_buf_used_size + buf.len > AX_READED_BUF_SIZE)
    {
        LOGE("AX add_to_readed_buf failed: %d %d", (int)readed_buf_used_size, (int)buf.len);
        return SCRCPY_EXIT_FAILURE;
    }

    memcpy(ax_readed_buf + readed_buf_used_size, buf.base, buf.len);
    readed_buf_used_size += buf.len;

    return SCRCPY_EXIT_SUCCESS;
}

static int remove_readed_buf_head(size_t count)
{
    if (0 == count)
    {
        return SCRCPY_EXIT_SUCCESS;
    }
    if (count > readed_buf_used_size)
    {
        LOGE("AX remove_readed_buf_head failed: %d", (int)count);

        return SCRCPY_EXIT_FAILURE;
    }

    if (count == readed_buf_used_size)
    {
        // all data removed, do not touch innerBuffer
        readed_buf_used_size = 0;
    }
    else
    {
        readed_buf_used_size -= count;

        memmove(ax_readed_buf, ax_readed_buf + count, readed_buf_used_size);
    }

    return SCRCPY_EXIT_SUCCESS;
}

// libuv utility
static int make_ax_jitter(int limit)
{
    int retJitter = 0;

    int jitter_val = rand() % limit;
    int jitter_direction = rand() % 2;

    if (jitter_direction > 0)
    {
        retJitter = jitter_val;
    }
    else
    {
        retJitter = 0 - jitter_val;
    }

    return retJitter;
}
static void auto_scroll_handle_4_up()
{
    LOGD("auto scroll up");

    int jitter_limit_small = 50;
    int jitter_limit_large = 100;

    int start_x = last_send_screen_width / 2 + make_ax_jitter(jitter_limit_large);
    int start_y = last_send_screen_height * 3 / 4 + make_ax_jitter(jitter_limit_small);

    int end_x = start_x + make_ax_jitter(jitter_limit_small);
    int end_y = start_y - last_send_screen_height / 4 + make_ax_jitter(jitter_limit_small);

    // start touch
    struct ax_delayed_action beginTouch;
    beginTouch.delayed_type = ax_delayed_type_touch_down;
    beginTouch.touch_x = start_x;
    beginTouch.touch_y = start_y;
    beginTouch.expire_count = 0;

    add_ax_delayed_action(beginTouch);

    // moving
    int jitter_step = 2;
    int steps = 10 + make_ax_jitter(jitter_step);
    int step_x = (end_x - start_x) / steps;
    int step_y = (end_y - start_y) / steps;
    int expire_count = 0;

    int moving_x = start_x;
    int moving_y = start_y;
    while (moving_y > end_y)
    {
        struct ax_delayed_action moveTouch;
        moveTouch.delayed_type = ax_delayed_type_touch_move;
        moveTouch.touch_x = moving_x;
        moveTouch.touch_y = moving_y;
        moveTouch.expire_count = expire_count;

        add_ax_delayed_action(moveTouch);

        moving_x += step_x;
        moving_y += step_y;
    }

    // end touch
    struct ax_delayed_action endTouch;
    endTouch.delayed_type = ax_delayed_type_touch_up;
    endTouch.touch_x = end_x;
    endTouch.touch_y = end_y;
    endTouch.expire_count = 0;

    add_ax_delayed_action(endTouch);
}

void auto_scroll_handle_4_left()
{
    LOGD("auto scroll left");

    int jitter_limit_small = 50;
    int jitter_limit_large = 100;

    int start_x = last_send_screen_width * 3 / 4 + make_ax_jitter(jitter_limit_small);
    int start_y = last_send_screen_height / 2 + make_ax_jitter(jitter_limit_large);

    int end_x = start_x - last_send_screen_height / 4 + make_ax_jitter(jitter_limit_small);
    int end_y = start_y + make_ax_jitter(jitter_limit_small);

    // start touch
    struct ax_delayed_action beginTouch;
    beginTouch.delayed_type = ax_delayed_type_touch_down;
    beginTouch.touch_x = start_x;
    beginTouch.touch_y = start_y;
    beginTouch.expire_count = 0;

    add_ax_delayed_action(beginTouch);

    // moving
    int jitter_step = 2;
    int steps = 10 + make_ax_jitter(jitter_step);
    int step_x = (end_x - start_x) / steps;
    int step_y = (end_y - start_y) / steps;
    int expire_count = 0;

    int moving_x = start_x;
    int moving_y = start_y;
    while (moving_x > end_x)
    {
        struct ax_delayed_action moveTouch;
        moveTouch.delayed_type = ax_delayed_type_touch_move;
        moveTouch.touch_x = moving_x;
        moveTouch.touch_y = moving_y;
        moveTouch.expire_count = expire_count;

        add_ax_delayed_action(moveTouch);

        moving_x += step_x;
        moving_y += step_y;
    }

    // end touch
    struct ax_delayed_action endTouch;
    endTouch.delayed_type = ax_delayed_type_touch_up;
    endTouch.touch_x = end_x;
    endTouch.touch_y = end_y;
    endTouch.expire_count = 0;

    add_ax_delayed_action(endTouch);
}

static void handle_ax_json_cmd(const uv_buf_t buf)
{
    cJSON *cmdJson = cJSON_ParseWithLength(buf.base, buf.len);

    if (sc_get_log_level() == SC_LOG_LEVEL_VERBOSE || sc_get_log_level() == SC_LOG_LEVEL_DEBUG)
    {
        cJSON_PrintPreallocated(cmdJson, ax_print_log_buf, AX_BUF_SIZE, false);

        LOGD("ax received: %s", ax_print_log_buf);
    }

    char *innerCmd = cJSON_GetStringValue(cJSON_GetObjectItem(cmdJson, AX_JSON_KEY_COMMAND));
    int innerErrCode = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cmdJson, AX_JSON_KEY_ERR_CODE));

    if (AX_ERR_CODE_SUCCESS == innerErrCode)
    {
        // LOGD("%s success", innerCmd);
        if (strcmp(innerCmd, AX_JSON_COMMAND_RESPONSE) == 0)
        {
            // no handle response
            char *innerResponse2Cmd = cJSON_GetStringValue(cJSON_GetObjectItem(cmdJson, AX_JSON_KEY_RESPONSE_2_COMMAND));
            if (strcmp(innerResponse2Cmd, AX_JSON_COMMAND_CLIENT_BEGIN_VIDEO) == 0)
            {
                struct ax_delayed_action tmpAction;
                tmpAction.delayed_type = ax_delayed_type_set_video_mode;
                tmpAction.expire_count = 10;

                add_ax_delayed_action(tmpAction);
            }
        }
        else if (strcmp(innerCmd, AX_JSON_COMMAND_AUTO_SCROLL) == 0)
        {
            // origin at left-top

            char *innerContent = cJSON_GetStringValue(cJSON_GetObjectItem(cmdJson, AX_JSON_KEY_CONTENT));

            if (strcmp(innerContent, AX_SCROLL_DIRECTION_UP) == 0)
            {
                auto_scroll_handle_4_up();
            }
            else if (strcmp(innerContent, AX_SCROLL_DIRECTION_LEFT) == 0)
            {
                auto_scroll_handle_4_left();
            }
        }
        else if (strcmp(innerCmd, AX_JSON_COMMAND_SWITCH_TO_VIDEO_MODE) == 0)
        {
            sendBeginVideoPacket();
        }
    }
    else
    {
        LOGE("AX cmd: %s failed", innerCmd);
    }

    cJSON_Delete(cmdJson);
}

static void on_require_alloc_buf(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;

    buf->base = (char *)malloc(AX_BUF_SIZE);
    buf->len = AX_BUF_SIZE;
}

static void ax_release_uv_buf(const uv_buf_t *buf)
{
    free(buf->base);
}

static void on_tcp_wrote_data(uv_write_t *req, int status)
{
    // LOGD("AX onTcpWroteData status: %d", status);

    if (status)
    {
        LOGE("AX onTcpWroteData failed: %s", uv_strerror(status));
    }

    // free
    if (NULL != req->data)
    {
        uv_buf_t tmpBuf;
        tmpBuf.base = (char *)req->data;
        tmpBuf.len = AX_BUF_SIZE;

        ax_release_uv_buf(&tmpBuf);
    }

    free(req);
}

static void sendAXCommand(char *cmd_str)
{
    LOGI("AX send: %s", cmd_str);

    uv_write_t *ax_writer = (uv_write_t *)malloc(sizeof(uv_write_t));
    uv_buf_t writer_buf;
    on_require_alloc_buf(NULL, 0, &writer_buf);

    makeAPPacket(writer_buf.base, cmd_str);
    writer_buf.len = strlen(cmd_str) + apPacketHeaderLength(AP_STREAM_CONTENT_TYPE_JSON);

    ax_writer->data = (void *)writer_buf.base;

    uv_write(ax_writer, (uv_stream_t *)&tcpClientSocket, &writer_buf, 1, on_tcp_wrote_data);
}

static void check_report_client_info()
{
    sc_mutex_lock(&ax_mutex);

    bool screen_size_updated = false;

    if (sc_screen_width != last_send_screen_width || sc_screen_height != last_send_screen_height)
    {
        last_send_screen_width = sc_screen_width;
        last_send_screen_height = sc_screen_height;

        screen_size_updated = true;
    }

    sc_mutex_unlock(&ax_mutex);

    if (screen_size_updated)
    {
        char *cmd_str = makeSetClientInfoJson(android_serial, last_send_screen_width, last_send_screen_height);

        sendAXCommand(cmd_str);
    }
}

void sendBeginVideoPacket()
{
    char *cmd_str = makeBeginVideoModeJson(av_video_codec_id);

    sendAXCommand(cmd_str);
}

static bool isAxDelayedTypeIsTouch(int delayed_type)
{
    if (ax_delayed_type_touch_down == delayed_type ||
        ax_delayed_type_touch_up == delayed_type ||
        ax_delayed_type_touch_move == delayed_type)
    {
        return true;
    }

    return false;
}

static void onAXRepeatTimerExpired(uv_timer_t *handle)
{
    (void)handle;

    if (!ax_running)
    {
        LOGE("ax not running, but onAXRepeatTimerExpired");
        return;
    }

    check_report_client_info();

    if (handling_delayed_action.expire_count < 0)
    {
        // handling touch_action invalid
        if (sc_vecdeque_is_empty(&ax_pending_delayed_queue))
        {
            // queue empty, nothing todo
        }
        else
        {
            handling_delayed_action = sc_vecdeque_pop(&ax_pending_delayed_queue);
        }
    }

    if (handling_delayed_action.expire_count >= 0)
    {
        handling_delayed_action.expire_count -= 1;

        if (handling_delayed_action.expire_count < 0)
        {
            // LOGD("consumed touch_action: type(%d) --- x(%d) --- y(%d)",
            //     handling_delayed_action.delayed_type,
            //     handling_delayed_action.touch_x,
            //     handling_delayed_action.touch_y);
            int delayed_type = handling_delayed_action.delayed_type;
            if (isAxDelayedTypeIsTouch(delayed_type))
            {
                struct sc_point touchPoint = {.x = handling_delayed_action.touch_x, .y = handling_delayed_action.touch_y};

                if (ax_delayed_type_touch_down == delayed_type || ax_delayed_type_touch_up == delayed_type)
                {
                    struct sc_mouse_click_event clickEvt = {
                        .position = {
                            .screen_size = ax_sc_im->screen->frame_size,
                            .point = touchPoint,
                        },
                        .action = handling_delayed_action.delayed_type == ax_delayed_type_touch_down ? SC_ACTION_DOWN : SC_ACTION_UP,
                        .button = SC_MOUSE_BUTTON_LEFT,
                        .pointer_id = ax_sc_im->vfinger_down ? SC_POINTER_ID_GENERIC_FINGER : SC_POINTER_ID_MOUSE,
                        .buttons_state = handling_delayed_action.delayed_type == ax_delayed_type_touch_down ? SC_MOUSE_BUTTON_LEFT : 0,
                    };
                    ax_sc_im->mp->ops->process_mouse_click(ax_sc_im->mp, &clickEvt);
                }
                else if (ax_delayed_type_touch_move == delayed_type)
                {
                    struct sc_mouse_motion_event motionEvt = {
                        .position = {
                            .screen_size = ax_sc_im->screen->frame_size,
                            .point = touchPoint,
                        },
                        .pointer_id = ax_sc_im->vfinger_down ? SC_POINTER_ID_GENERIC_FINGER : SC_POINTER_ID_MOUSE,
                        .xrel = 0,
                        .yrel = 0,
                        .buttons_state = SC_MOUSE_BUTTON_LEFT,
                    };

                    ax_sc_im->mp->ops->process_mouse_motion(ax_sc_im->mp, &motionEvt);
                }
            }
            else if (ax_delayed_type_set_video_mode == delayed_type)
            {
                client_should_send_video = true;
                LOGI("AX video state set");
            }
        }
    }
}

static int handle_received_data(const uv_buf_t buf)
{
    int retVal = add_to_readed_buf(buf);

    if (SCRCPY_EXIT_SUCCESS == retVal)
    {
        size_t headerLen = apPacketHeaderLength(AP_STREAM_CONTENT_TYPE_JSON);

        while (readed_buf_used_size >= headerLen)
        {
            size_t packet_len = getAPPacketLength(ax_readed_buf);
            int content_type = getAPPacketContentType(ax_readed_buf);

            if (packet_len < headerLen ||
                content_type != AP_STREAM_CONTENT_TYPE_JSON)
            {
                retVal = SCRCPY_EXIT_FAILURE;
                LOGE("AX invalid packet header");
                break;
            }

            if (packet_len > readed_buf_used_size)
            {
                LOGI("AX no enough packet data");
                break;
            }
            else
            {
                handle_ax_json_cmd(uv_buf_init(ax_readed_buf + headerLen, packet_len - headerLen));

                retVal = remove_readed_buf_head(packet_len);
            }
        }
    }

    return retVal;
}

static void on_readed_data(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    LOGD("on_readed_data: %d", (int)nread);

    if (nread > 0)
    {
        int retVal = handle_received_data(uv_buf_init(buf->base, nread));
        if (SCRCPY_EXIT_SUCCESS != retVal)
        {
            LOGE("AX on_readed_data handle failed");
            uv_close((uv_handle_t *)stream, NULL);

            uv_async_send(&stop_async);
        }
    }
    else
    {
        if (nread < 0)
        {
            if (nread != UV_EOF)
            {
                LOGE("AX on_readed_data failed: %s", uv_strerror(nread));
            }

            uv_close((uv_handle_t *)stream, NULL);
            uv_async_send(&stop_async);
        }
    }

    ax_release_uv_buf(buf);
}

static void ax_close_handles_and_stop()
{
    uv_close((uv_handle_t *)&stop_async, NULL);
    uv_close((uv_handle_t *)&send_video_async, NULL);

    uv_timer_stop(&repeatTimer);
    uv_close((uv_handle_t *)&repeatTimer, NULL);

    uv_close((uv_handle_t *)&tcpClientSocket, NULL);

    uv_stop(&axUVLoop);
}

static void on_tcp_connected(uv_connect_t *req, int status)
{
    if (status)
    {
        LOGE("AX on_tcp_connected failed: %s", uv_strerror(status));
        ax_running = false;

        ax_close_handles_and_stop();

        return;
    }
    LOGI("AX tcp connected");

    uv_read_start((uv_stream_t *)req->handle, on_require_alloc_buf, on_readed_data);
}

static void stop_async_cb(uv_async_t *handle)
{
    (void)handle;

    if (ax_running)
    {
        LOGI("AX stop async callback called");

        ax_close_handles_and_stop();
    }
}

static void send_video_async_cb(uv_async_t *handle)
{
    (void)handle;

    if (ax_running)
    {
        sc_mutex_lock(&ax_mutex);

        if (ax_sending_video_packet_length > 0)
        {
            // LOGD("AX send video packet: %d", ax_sending_video_packet_length);

            uv_buf_t tmp_buf = uv_buf_init((char *)ax_sending_video_packet_buf, ax_sending_video_packet_length);

            uv_write_t *ax_writer = (uv_write_t *)malloc(sizeof(uv_write_t));
            ax_writer->data = NULL;
            uv_write(ax_writer, (uv_stream_t *)&tcpClientSocket, &tmp_buf, 1, on_tcp_wrote_data);

            ax_sending_video_packet_length = 0;
        }

        sc_mutex_unlock(&ax_mutex);
    }
}

static int ax_thread_cb(void *data)
{
    (void)data;
    LOGI("AX thread running");

    sc_vecdeque_init(&ax_pending_delayed_queue);

    uv_loop_init(&axUVLoop);

    uv_async_init(&axUVLoop, &stop_async, stop_async_cb);
    uv_async_init(&axUVLoop, &send_video_async, send_video_async_cb);

    uv_timer_init(&axUVLoop, &repeatTimer);
    uv_timer_start(&repeatTimer, onAXRepeatTimerExpired, 1000, AX_REPEAT_TIMER_REPEAT_VAL);

    uv_tcp_init(&axUVLoop, &tcpClientSocket);

    struct sockaddr_in serverAddr;
    uv_ip4_addr(AX_SERVER_ADDR, AX_SERVER_PORT, &serverAddr);

    uv_connect_t tcpConnector;

    int retVal = uv_tcp_connect(&tcpConnector, &tcpClientSocket, (const struct sockaddr *)&serverAddr, on_tcp_connected);
    if (retVal)
    {
        LOGI("AX uv_tcp_connect failed: %s", uv_strerror(retVal));
        return SCRCPY_EXIT_FAILURE;
    }

    LOGI("AX uv_run running");
    ax_running = true;

    retVal = uv_run(&axUVLoop, UV_RUN_DEFAULT);
    if (retVal)
    {
        LOGI("AX uv_run failed");
    }

    retVal = uv_loop_close(&axUVLoop);
    if (retVal)
    {
        LOGI("AX uv_loop_close failed: %s", uv_strerror(retVal));
    }

    ax_running = false;
    sc_vecdeque_destroy(&ax_pending_delayed_queue);

    return SCRCPY_EXIT_SUCCESS;
}

int ax_start_action(const char *serial, struct sc_input_manager *sc_im)
{
    LOGI("AX starting action: %s", serial);

    sc_mutex_init(&ax_mutex);

    size_t serialLen = strlen(serial);
    if (serialLen > AX_SERIAL_MAX_LEN)
    {
        LOGE("AX serial len too large: %d", (int)serialLen);
        return SCRCPY_EXIT_FAILURE;
    }
    strcpy(android_serial, serial);

    ax_sc_im = sc_im;

    bool ok = sc_thread_create(&ax_thread, ax_thread_cb, "ax_thread_name", "");
    if (!ok)
    {
        LOGE("AX thread create failed");
        return SCRCPY_EXIT_FAILURE;
    }
    ax_thread_started = true;

    return SCRCPY_EXIT_SUCCESS;
}

int ax_stop_action()
{
    LOGI("AX stopping action");

    if (ax_thread_started && ax_running)
    {
        uv_async_send(&stop_async);
    }
    sc_thread_join(&ax_thread, NULL);

    sc_mutex_destroy(&ax_mutex);

    return SCRCPY_EXIT_SUCCESS;
}

void ax_update_client_info(int screen_width, int screen_height, int video_codec_id)
{
    if (ax_thread_started && screen_width > 0 && screen_height > 0)
    {
        sc_mutex_lock(&ax_mutex);

        if (sc_screen_width != screen_width || sc_screen_height != screen_height)
        {
            sc_screen_width = screen_width;
            sc_screen_height = screen_height;
            av_video_codec_id = video_codec_id;
        }

        sc_mutex_unlock(&ax_mutex);
    }
}

bool ax_should_send_video()
{
    return client_should_send_video;
}

static void ax_inner_send_avPacket(AVPacket *packet)
{
    sc_mutex_lock(&ax_mutex);

    bool can_send = true;

    if (ax_sending_video_packet_length > 0)
    {
        LOGW("AX last video packet not sent");
        can_send = false;
    }

    if (can_send)
    {
        int makedSize = makeAPVideoPacket(packet, ax_sending_video_packet_buf);
        if (makedSize > 0)
        {
            ax_sending_video_packet_length = makedSize;

            uv_async_send(&send_video_async);
        }
    }

    sc_mutex_unlock(&ax_mutex);
}

void ax_send_videoPacket(AVPacket *packet)
{
    if (ax_thread_started && ax_running)
    {
        if (NULL != last_missed_key_packet)
        {
            ax_inner_send_avPacket(last_missed_key_packet);

            av_packet_unref(last_missed_key_packet);
            last_missed_key_packet = NULL;
        }
        else
        {
            ax_inner_send_avPacket(packet);
        }
    }
}

void ax_set_missed_key_packet(AVPacket *keyPacket)
{
    if (NULL != last_missed_key_packet)
    {
        av_packet_unref(last_missed_key_packet);
    }

    last_missed_key_packet = av_packet_clone(keyPacket);
}
