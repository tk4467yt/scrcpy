//
//
//

#ifndef AX_ACTION_H
#define AX_ACTION_H

int start_ax_action(const char *serial);
int stop_ax_action();

void update_ax_device_info(int screen_width, int screen_height);

#endif // end AX_ACTION_H