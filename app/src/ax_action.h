//
//
//

#ifndef AX_ACTION_H
#define AX_ACTION_H

int ax_start_action(const char *serial);
int ax_stop_action();

void ax_update_client_info(int screen_width, int screen_height);

#endif // end AX_ACTION_H