/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Input sink contract. The GUI adapter implements it; physical drivers do
 * not access display objects, HID classes, or each other's private state.
 * Returns are acceptance/readiness, not hardware readiness guarantees.
 */
#ifndef GIULIA_INPUT_PROVIDER_H
#define GIULIA_INPUT_PROVIDER_H
#ifdef __cplusplus
extern "C" {
#endif
int giulia_physical_button(unsigned usage, unsigned down);
int giulia_slider_ring(unsigned enabled);
unsigned giulia_remote_ready(void);
void sweet_hid_touch(int finger, int down, int x, int y);
#ifdef __cplusplus
}
#endif
#endif
