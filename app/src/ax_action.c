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

#define AX_SERIAL_MAX_LEN 128
#define AX_BUF_SIZE 2048

#define AX_SERVER_ADDR "127.0.0.1"
#define AX_SERVER_PORT 10748

static sc_thread ax_thread;
static char android_serial[AX_SERIAL_MAX_LEN];
static uv_loop_t axUVLoop;
static bool ax_running = false;

static uv_async_t stop_async;

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
            LOGE("on_readed_data handle failed");
            uv_close((uv_handle_t*) stream, NULL);

            uv_async_send(&stop_async);
        }
    } else {
        if (nread < 0) {
            if (nread != UV_EOF) {
                LOGE("uv_tcp_connect failed: %s", uv_strerror(nread));
            }
            
            uv_close((uv_handle_t*) stream, NULL);
        }
    }

    ax_release_uv_buf(buf);
}

static void on_tcp_connected(uv_connect_t* req, int status)
{
    if (status) {
        LOGE("uv_tcp_connect failed: %s", uv_strerror(status));
        return;
    }
    LOGI("AX tcp connected");

    uv_read_start((uv_stream_t *)req->handle, on_require_alloc_buf, on_readed_data);
}

static void stop_async_cb(uv_async_t* handle)
{
    (void)handle;

    LOGI("AX stop async callback called");

    uv_stop(&axUVLoop);
}

static int ax_thread_cb(void *data) 
{
    (void)data;
    LOGI("AX thread running");

    uv_loop_init(&axUVLoop);

    uv_async_init(&axUVLoop, &stop_async, stop_async_cb);

    uv_tcp_t tcpSocket;
    uv_tcp_init(&axUVLoop, &tcpSocket);

    struct sockaddr_in serverAddr;
    uv_ip4_addr(AX_SERVER_ADDR, AX_SERVER_PORT, &serverAddr);

    uv_connect_t tcpConnector;

    int retVal = uv_tcp_connect(&tcpConnector, &tcpSocket, (const struct sockaddr*)&serverAddr, on_tcp_connected);
    if (retVal) {
        LOGI("uv_tcp_connect failed: %s", uv_strerror(retVal));
        return SCRCPY_EXIT_FAILURE;
    }

    retVal = uv_run(&axUVLoop, UV_RUN_DEFAULT);
    if (retVal) {
        LOGI("uv_run broken");
    }

    uv_loop_close(&axUVLoop);

    ax_running = false;

    return SCRCPY_EXIT_SUCCESS;
}

int start_ax_action(const char *serial)
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
    
    ax_running = true;

    return SCRCPY_EXIT_SUCCESS;
}

int stop_ax_action()
{
    LOGI("AX stopping action");

    if (ax_running) {
        uv_async_send(&stop_async);
        
        sc_thread_join(&ax_thread, NULL);
    }

    return SCRCPY_EXIT_SUCCESS;
}