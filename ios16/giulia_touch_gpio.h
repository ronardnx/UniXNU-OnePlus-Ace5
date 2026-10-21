/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SPI mode 0, MSB first, active-low CS. No DMA or controller reprogramming.
 * Pin indices are MISO=0, MOSI=1, clock=2, CS=3. The caller owns the pins.
 */
#ifndef GIULIA_TOUCH_GPIO_H
#define GIULIA_TOUCH_GPIO_H
#include <stdint.h>
struct gg_bus {
    void (*set)(unsigned,unsigned);
    unsigned (*sample)(void);
    void (*pause)(void);
};
static inline int gg_transfer(const struct gg_bus *b,const uint8_t *tx,uint8_t *rx,unsigned n)
{
    if(!b || !b->set || !b->sample || !b->pause || !n || n>519 || (!tx && !rx))return 1;
    b->set(2,0);b->set(3,0);b->pause();
    for(unsigned i=0;i<n;i++) {
        uint8_t send=tx?tx[i]:0xff,receive=0;
        for(unsigned bit=0;bit<8;bit++) {
            b->set(1,(send>>(7-bit))&1);b->pause();
            b->set(2,1);b->pause();
            receive=(uint8_t)((receive<<1)|(b->sample()&1));
            b->set(2,0);
        }
        if(rx)rx[i]=receive;
    }
    b->pause();b->set(3,1);b->pause();return 0;
}
#endif
