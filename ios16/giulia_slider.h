/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * AK09970 protocol evidence: hardware/sm8650-keys.json and the stock source
 * manifest in out/buttons-slider-session-20261023/reference/evidence.json.
 * This portable core does not acquire GPIO, clocks or a regulator.
 */
#ifndef GIULIA_SLIDER_H
#define GIULIA_SLIDER_H
#include <stdint.h>
#include <stddef.h>
enum { GSL_OK, GSL_ARGUMENT, GSL_BUS_BUSY, GSL_STRETCH, GSL_NACK,
       GSL_IO, GSL_IDENTITY, GSL_NOT_READY, GSL_READBACK, GSL_CALIBRATION };
enum { GSL_SDA, GSL_SCL };
struct gsl_bus {
    void *cookie;
    /* low=1 drives zero; low=0 DISABLES output. It must never drive high. */
    void (*low)(void *,unsigned,unsigned);
    int (*level)(void *,unsigned);
    void (*delay_us)(void *,unsigned);
};
static inline int gsl_clock(const struct gsl_bus *b)
{
    b->low(b->cookie,GSL_SCL,0);
    for(unsigned i=0;i<200;i++) {
        int level=b->level(b->cookie,GSL_SCL);
        if(level<0)return GSL_IO;
        if(level)return GSL_OK;
        b->delay_us(b->cookie,5);
    }
    return GSL_STRETCH;
}
static inline int gsl_start(const struct gsl_bus *b)
{
    b->low(b->cookie,GSL_SDA,0);
    /* In a repeated START this is also the SCL-low/data-setup interval
     * following the previous ACK clock; keep it bounded but nonzero. */
    b->delay_us(b->cookie,5);
    int e=gsl_clock(b);if(e)return e;
    int level=b->level(b->cookie,GSL_SDA);
    if(level<0)return GSL_IO;
    if(!level)return GSL_BUS_BUSY;
    b->delay_us(b->cookie,5);b->low(b->cookie,GSL_SDA,1);
    b->delay_us(b->cookie,5);b->low(b->cookie,GSL_SCL,1);
    return GSL_OK;
}
static inline int gsl_stop(const struct gsl_bus *b)
{
    b->low(b->cookie,GSL_SCL,1);b->low(b->cookie,GSL_SDA,1);
    b->delay_us(b->cookie,5);int e=gsl_clock(b);
    b->delay_us(b->cookie,5);b->low(b->cookie,GSL_SDA,0);
    /* Release both lines even when SCL never rises. */
    b->low(b->cookie,GSL_SCL,0);b->delay_us(b->cookie,5);
    return e;
}
static inline int gsl_bit(const struct gsl_bus *b,unsigned output,unsigned *input)
{
    b->low(b->cookie,GSL_SDA,!output);b->delay_us(b->cookie,5);
    int e=gsl_clock(b);if(e)return e;
    b->delay_us(b->cookie,5);
    if(input) {
        int level=b->level(b->cookie,GSL_SDA);
        if(level<0)return GSL_IO;
        *input=!!level;
    }
    b->low(b->cookie,GSL_SCL,1);return GSL_OK;
}
static inline int gsl_send(const struct gsl_bus *b,unsigned byte)
{
    for(unsigned bit=0;bit<8;bit++) {
        int e=gsl_bit(b,(byte>>(7-bit))&1,0);if(e)return e;
    }
    unsigned ack;int e=gsl_bit(b,1,&ack);
    return e?e:ack?GSL_NACK:GSL_OK;
}
static inline int gsl_receive(const struct gsl_bus *b,uint8_t *byte,unsigned last)
{
    unsigned data=0;
    for(unsigned bit=0;bit<8;bit++) {
        unsigned value;int e=gsl_bit(b,1,&value);if(e)return e;
        data=(data<<1)|value;
    }
    int e=gsl_bit(b,!!last,0);if(e)return e;
    *byte=(uint8_t)data;return GSL_OK;
}
static inline int gsl_transfer(const struct gsl_bus *b,unsigned reg,uint8_t *data,
    unsigned length,unsigned read)
{
    if(!b || !b->low || !b->level || !b->delay_us || !data || !length ||
       length>16 || reg>255 || read>1)return GSL_ARGUMENT;
    int e=gsl_start(b);
    /* START did not acquire the bus: do not manufacture a STOP on a bus
     * potentially held by another owner. Only release our outputs. */
    if(e) {
        b->low(b->cookie,GSL_SDA,0);b->low(b->cookie,GSL_SCL,0);
        return e;
    }
    if(!e)e=gsl_send(b,0x20); /* Fixed AK09970 address 0x10, write. */
    if(!e)e=gsl_send(b,reg);
    if(read) {
        if(!e)e=gsl_start(b);
        if(!e)e=gsl_send(b,0x21);
        for(unsigned i=0;!e && i<length;i++)e=gsl_receive(b,data+i,i+1==length);
    } else {
        for(unsigned i=0;!e && i<length;i++)e=gsl_send(b,data[i]);
    }
    int cleanup=gsl_stop(b);
    return e?e:cleanup;
}
struct gsl_sensor_io {
    void *cookie;
    int (*read)(void *,unsigned,uint8_t *,unsigned);
    int (*write)(void *,unsigned,const uint8_t *,unsigned);
    void (*delay_us)(void *,unsigned);
};
struct gsl_sensor { unsigned saved,changed,active,cleanup_error;uint8_t previous; };
struct gsl_vector { int32_t x,y,z; };
static inline int gsl_sensor_restore(const struct gsl_sensor_io *io,struct gsl_sensor *s)
{
    s->active=0;if(!s->changed)return GSL_OK;
    uint8_t verify=0;
    int e=io->write(io->cookie,0x21,&s->previous,1);
    if(!e)e=io->read(io->cookie,0x21,&verify,1);
    if(!e && verify!=s->previous)e=GSL_READBACK;
    s->cleanup_error=(unsigned)e;
    if(!e)s->changed=0;
    return e;
}
static inline int gsl_sensor_begin(const struct gsl_sensor_io *io,struct gsl_sensor *s)
{
    if(!io || !s || !io->read || !io->write || !io->delay_us || s->changed)return GSL_ARGUMENT;
    uint8_t id[2],control=0x2a,verify=0;
    int e=io->read(io->cookie,0,id,2);if(e)return e;
    if(id[0]!=0x48 || id[1]!=0xc1)return GSL_IDENTITY;
    e=io->read(io->cookie,0x21,&s->previous,1);if(e)return e;
    s->saved=1;s->changed=1;
    e=io->write(io->cookie,0x21,&control,1);
    if(!e)e=io->read(io->cookie,0x21,&verify,1);
    if(!e && verify!=control)e=GSL_READBACK;
    if(e){(void)gsl_sensor_restore(io,s);return e;}
    io->delay_us(io->cookie,35000);s->active=1;return GSL_OK;
}
static inline int gsl_sensor_sample(const struct gsl_sensor_io *io,
    const struct gsl_sensor *s,struct gsl_vector *v)
{
    if(!io || !s || !v || !s->active)return GSL_NOT_READY;
    uint8_t data[7];int e=io->read(io->cookie,0x17,data,7);if(e)return e;
    if(!(data[0]&1))return GSL_NOT_READY;
    v->z=(int16_t)((data[1]<<8)|data[2]);
    v->y=(int16_t)((data[3]<<8)|data[4]);
    v->x=(int16_t)((data[5]<<8)|data[6]);return GSL_OK;
}
/* Positions 1/2/3 are operator-labelled UP/MIDDLE/DOWN, never inferred from
 * magnetic polarity. Radius is a measured calibration input, not a fallback. */
struct gsl_calibration { struct gsl_vector centre[3];unsigned radius[3],valid; };
static inline unsigned gsl_distance(int32_t a,int32_t b)
{int64_t d=(int64_t)a-b;return (unsigned)(d<0?-d:d);}
static inline int gsl_calibration_valid(const struct gsl_calibration *c)
{
    if(!c || c->valid!=7)return 0;
    for(unsigned i=0;i<3;i++) {
        if(!c->radius[i] || c->radius[i]>32767)return 0;
        if(c->centre[i].x<-32768 || c->centre[i].x>32767 ||
           c->centre[i].y<-32768 || c->centre[i].y>32767 ||
           c->centre[i].z<-32768 || c->centre[i].z>32767)return 0;
        for(unsigned j=0;j<i;j++) {
            unsigned r=c->radius[i]+c->radius[j];
            if(gsl_distance(c->centre[i].x,c->centre[j].x)<=r &&
               gsl_distance(c->centre[i].y,c->centre[j].y)<=r &&
               gsl_distance(c->centre[i].z,c->centre[j].z)<=r)return 0;
        }
    }
    return 1;
}
static inline unsigned gsl_position(const struct gsl_calibration *c,const struct gsl_vector *v)
{
    if(!v || !gsl_calibration_valid(c))return 0;
    for(unsigned i=0;i<3;i++)if(gsl_distance(v->x,c->centre[i].x)<=c->radius[i] &&
        gsl_distance(v->y,c->centre[i].y)<=c->radius[i] &&
        gsl_distance(v->z,c->centre[i].z)<=c->radius[i])return i+1;
    return 0;
}
struct gsl_filter { unsigned candidate,stable;uint64_t since; };
static inline unsigned gsl_filter_sample(struct gsl_filter *f,unsigned position,uint64_t ms)
{
    if(position>3)position=0;
    if(position!=f->candidate){f->candidate=position;f->since=ms;}
    if(!position)f->stable=0;
    else if(ms-f->since>=100)f->stable=position;
    return f->stable;
}
#endif
