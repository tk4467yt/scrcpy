//
//
//


#include "scrcpy.h"
#include "util/log.h"
#include "util/thread.h"

// MIN, MAX macro redefined in "common.h" and <sys/param.h>
// so declare uv.h after scrcpy's header
#include <uv.h>

#include "ax_action.h"
#include "cJSON.h"

static sc_thread ax_thread;
static char android_serial[128];

static int ax_thread_cb(void *data) 
{
    (void)data;
    LOGI("ax thread running");

    return SCRCPY_EXIT_SUCCESS;
}

int start_ax_action(const char *serial)
{
    LOGI("starting ax action: %s", serial);
    strcpy(android_serial, serial);

    bool ok = sc_thread_create(&ax_thread, ax_thread_cb, "ax_thread_name", "");
    if (!ok) {
        LOGE("Could not start controller thread");
        return SCRCPY_EXIT_FAILURE;
    }

    return SCRCPY_EXIT_SUCCESS;
}

int stop_ax_action()
{
    LOGI("stopping ax action");

    sc_thread_join(&ax_thread, NULL);

    return SCRCPY_EXIT_SUCCESS;
}