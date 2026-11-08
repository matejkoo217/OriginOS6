/* SPDX-License-Identifier: MIT */
#ifndef WM_CLIENT_H
#define WM_CLIENT_H
#ifdef __cplusplus
extern "C" {
#endif
int  wm_keyguard_state(int *locked, int *showing_and_not_occluded);
void wm_keyguard_state_reset(void);
#ifdef __cplusplus
}
#endif
#endif  /* WM_CLIENT_H End */
