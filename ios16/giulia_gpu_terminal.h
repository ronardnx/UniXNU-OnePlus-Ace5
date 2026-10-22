/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_TERMINAL_H
#define GIULIA_GPU_TERMINAL_H
/* The USB command path only touches this static mailbox, never an IOService
 * pointer or an MMIO mapping. Repeated pending requests coalesce. Closing
 * excludes new requests until the old owner has joined its retained worker. */
enum { GGT_STATUS=1, GGT_OBSERVE=2, GGT_ACTIVE=4, GGT_CLOSING=8 };
enum { GGT_UNAVAILABLE, GGT_PREPARING, GGT_READY, GGT_STOPPING, GGT_STOPPED, GGT_FAILED };
struct gg_terminal_mailbox { unsigned requests, stage, mode; };
static inline int gg_terminal_equal(const char *a,const char *b)
{
    if(!a)return 0;
    while(*a && *a==*b){++a;++b;}
    return *a==*b;
}
static inline unsigned gg_terminal_command(const char *command)
{
    return gg_terminal_equal(command,"gpu.status")?GGT_STATUS:
        gg_terminal_equal(command,"gpu.observe")?GGT_OBSERVE:0;
}
static inline int gg_terminal_open(struct gg_terminal_mailbox *m)
{
    unsigned expected=0;
    return __atomic_compare_exchange_n(&m->requests,&expected,GGT_ACTIVE,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline int gg_terminal_request(struct gg_terminal_mailbox *m,unsigned request)
{
    unsigned expected=__atomic_load_n(&m->requests,__ATOMIC_ACQUIRE);
    if(request!=GGT_STATUS && request!=GGT_OBSERVE)return 0;
    do {
        if(!(expected&GGT_ACTIVE))return 0;
    } while(!__atomic_compare_exchange_n(&m->requests,&expected,expected|request,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return 1;
}
static inline unsigned gg_terminal_take(struct gg_terminal_mailbox *m)
{
    return __atomic_fetch_and(&m->requests,~(GGT_STATUS|GGT_OBSERVE),__ATOMIC_ACQ_REL)&(GGT_STATUS|GGT_OBSERVE);
}
static inline unsigned gg_terminal_close(struct gg_terminal_mailbox *m)
{
    return __atomic_exchange_n(&m->requests,GGT_CLOSING,__ATOMIC_ACQ_REL)&(GGT_STATUS|GGT_OBSERVE);
}
static inline void gg_terminal_finish(struct gg_terminal_mailbox *m)
{
    __atomic_store_n(&m->requests,0u,__ATOMIC_RELEASE);
}
static inline const char *gg_terminal_stage_name(unsigned stage)
{
    const char *const names[]={"unavailable","preparing","host-memory-prepared","stopping","stopped","failed"};
    return stage<sizeof(names)/sizeof(names[0])?names[stage]:"invalid";
}
#endif
