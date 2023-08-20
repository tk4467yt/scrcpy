//
//
//


#include "scrcpy.h"
#include "util/log.h"
#include "util/thread.h"
#include "util/vecdeque.h"

// MIN, MAX macro redefined in "common.h" and <sys/param.h>
// so declare uv.h after scrcpy's header
#include <uv.h>

#include "ax_action.h"
#include "cJSON.h"

// packets define
#define AX_JSON_COMMAND_SET_CLIENT_INFO "set_client_info" // set client info (client ==>> server)

#define AX_JSON_CONTENT_KEY_COMMAND "command"
#define AX_JSON_CONTENT_KEY_UNIQUE_ID "unique_id"
#define AX_JSON_CONTENT_KEY_CONTENT "content"
#define AX_JSON_CONTENT_KEY_ERR_CODE "err_code"

#define AX_ERR_CODE_SUCCESS 0
#define AX_ERR_CODE_FAILED 1

#define AX_JSON_KEY_CLIENT_ID "client_id"
#define AX_JSON_KEY_SCREEN_WIDTH "screen_width"
#define AX_JSON_KEY_SCREEN_HEIGHT "screen_height"

#define AX_PACKET_HEADER_LEN 4

#define AX_STREAM_CONTENT_TYPE_RAW_VIDEO 1
#define AX_STREAM_CONTENT_TYPE_JSON 2

// libuv relate
#define AX_SERIAL_MAX_LEN 128

#define AX_SERVER_ADDR "127.0.0.1"
#define AX_SERVER_PORT 10748

static bool ax_thread_started = false;
static sc_thread ax_thread;

static uv_loop_t axUVLoop;
static uv_tcp_t tcpClientSocket;
static bool ax_running = false;

static uv_async_t stop_async;
static uv_async_t update_client_info_async;

static char android_serial[AX_SERIAL_MAX_LEN];
static int last_send_screen_width;
static int last_send_screen_height;
static int tmp_screen_width;
static int tmp_screen_height;

#define AX_BUF_SIZE 4096
#define AX_JSON_BUF_SIZE 4096
static char ax_content_buf[AX_JSON_BUF_SIZE];
static char ax_cmd_buf[AX_JSON_BUF_SIZE];

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

static char * makeAXCommand(char *cmd, char *content)
{
    cJSON *cmdJson = cJSON_CreateObject();
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_COMMAND, cmd);
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_UNIQUE_ID, "1234567890");
    cJSON_AddStringToObject(cmdJson, AX_JSON_CONTENT_KEY_CONTENT, content);

    cJSON_PrintPreallocated(cmdJson, ax_cmd_buf, AX_JSON_BUF_SIZE, false);

    cJSON_Delete(cmdJson);

    return ax_cmd_buf;
}

static char * makeSetClientInfoJson(char *clientID, int screen_width, int screen_height)
{
    cJSON *contentJson = cJSON_CreateObject();
    cJSON_AddStringToObject(contentJson, AX_JSON_KEY_CLIENT_ID, clientID);
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_SCREEN_WIDTH, screen_width);
    cJSON_AddNumberToObject(contentJson, AX_JSON_KEY_SCREEN_HEIGHT, screen_height);

    cJSON_PrintPreallocated(contentJson, ax_content_buf, AX_JSON_BUF_SIZE, false);

    cJSON_Delete(contentJson);

    char *cmd_str = makeAXCommand(AX_JSON_COMMAND_SET_CLIENT_INFO, ax_content_buf);

    return cmd_str;
}


// libuv utility
static int handle_received_data(const uv_buf_t* buf, ssize_t nread)
{
    (void)buf;
    (void)nread;

    return SCRCPY_EXIT_SUCCESS;
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

static void on_readed_data(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    if (nread > 0) {
        int retVal = handle_received_data(buf, nread);
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
    uv_write_t *ax_writer = (uv_write_t *)malloc(sizeof(uv_write_t));
    uv_buf_t writer_buf;
    on_require_alloc_buf(NULL, 0, &writer_buf);

    makeAXPacket(writer_buf.base, cmd_str);
    writer_buf.len = strlen(cmd_str) + AX_PACKET_HEADER_LEN;

    ax_writer->data = (void *)writer_buf.base;

    uv_write(ax_writer, (uv_stream_t *)&tcpClientSocket, &writer_buf, 1, on_tcp_wrote_data);
}

static void update_client_info_async_cb(uv_async_t* handle)
{
    (void)handle;

    if (ax_running) {
        if (tmp_screen_width != last_send_screen_width || tmp_screen_height != last_send_screen_height) {
            LOGI("AX update client info: %d --- %d", tmp_screen_width, tmp_screen_height);

            last_send_screen_width = tmp_screen_width;
            last_send_screen_height = tmp_screen_height;

            char *cmd_str = makeSetClientInfoJson(android_serial, last_send_screen_width, last_send_screen_height);
            sendAXCommand(cmd_str);
        }
    }
}

static int ax_thread_cb(void *data) 
{
    (void)data;
    LOGI("AX thread running");

    uv_loop_init(&axUVLoop);

    uv_async_init(&axUVLoop, &stop_async, stop_async_cb);
    uv_async_init(&axUVLoop, &update_client_info_async, update_client_info_async_cb);

    
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

    return SCRCPY_EXIT_SUCCESS;
}

int ax_start_action(const char *serial)
{
    LOGI("AX starting action: %s", serial);
    size_t serialLen = strlen(serial);
    if (serialLen > AX_SERIAL_MAX_LEN) {
        LOGE("AX serial len too large: %d", (int)serialLen);
        return SCRCPY_EXIT_FAILURE;
    }
    strcpy(android_serial, serial);

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

    if (ax_thread_started) {
        uv_async_send(&stop_async);
        
        sc_thread_join(&ax_thread, NULL);
    }
    
    return SCRCPY_EXIT_SUCCESS;
}

void ax_update_client_info(int screen_width, int screen_height)
{
    // TODO: tmp_screen_width, tmp_screen_height are multi thread access
    // 应该不会有问题，宽高第一次设置后，应该就不会再改变
    if (ax_thread_started && screen_width > 0 && screen_height > 0) {
        if (tmp_screen_width != screen_width || tmp_screen_height != screen_height) {
            tmp_screen_width = screen_width;
            tmp_screen_height = screen_height;

            uv_async_send(&update_client_info_async);
        }
    }
}
