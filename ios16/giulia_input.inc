/* CDC worker owns remote input state. HID providers dispatch real digitizer /
 * keyboard events; physical contact slots 0..9 remain separate from remote 10.
 * A short lease and explicit release prevent stuck contacts after client loss.
 */
/* GIULIA_INPUT_PROVIDER is embedded here by the profile generator. */
static struct {
    uint8_t keys[32];
    unsigned touch,x,y,buttons,modifiers;
    uint64_t deadline;
} ginput;

static int ginput_uint(const char **cursor,unsigned max,unsigned *value)
{
    const char *p=*cursor;unsigned n=0;
    while(*p==' ')p++;
    if(*p<'0' || *p>'9')return 0;
    do {unsigned digit=(unsigned)(*p++-'0');if(n>(max-digit)/10u || digit>max)return 0;n=n*10u+digit;}
    while(*p>='0' && *p<='9');
    if(*p && *p!=' ')return 0;
    *cursor=p;*value=n;return 1;
}
static int ginput_end(const char *p)
{while(*p==' ')p++;return !*p;}
static int ginput_held(void)
{
    if(ginput.touch || ginput.buttons || ginput.modifiers)return 1;
    for(unsigned i=0;i<sizeof(ginput.keys);i++)if(ginput.keys[i])return 1;
    return 0;
}
static void ginput_release(void)
{
    if(ginput.touch)giulia_remote_touch(0,ginput.x,ginput.y);
    for(unsigned usage=4;usage<232;usage++)if(ginput.keys[usage/8]&(1u<<(usage%8)))
        giulia_remote_hid(7,usage,0);
    for(unsigned bit=0;bit<8;bit++)if(ginput.modifiers&(1u<<bit))
        giulia_remote_hid(7,224+bit,0);
    static const unsigned usages[]={0x30,0xe9,0xea,0x40};
    for(unsigned bit=0;bit<4;bit++)if(ginput.buttons&(1u<<bit))
        giulia_remote_hid(12,usages[bit],0);
    memset(&ginput,0,sizeof(ginput));
}
static void ginput_tick(uint64_t now)
{if(ginput_held() && now>=ginput.deadline)ginput_release();}
static void ginput_status(void)
{
    unsigned ready=gs_enabled()?giulia_remote_ready():0;
    printf("GIULIA INPUT: available=%u touch=%u keyboard=%u buttons=%u lease_ms=3000\n",
        ready!=0,(ready&1)!=0,(ready&2)!=0,(ready&4)!=0);
}
static int ginput_command(const char *cmd,uint64_t now)
{
    if(strncmp(cmd,"input.",6))return 0;
    if(!strcmp(cmd,"input.status")) {ginput_status();return 1;}
    if(!strcmp(cmd,"input.release")) {ginput_release();return 1;}
    if(!gs_enabled()) {printf("GIULIA INPUT: available=0 reason=requires-giulia_debug\n");return 1;}
    if(!strcmp(cmd,"input.keepalive")) {ginput.deadline=now+3000;return 1;}
    unsigned ready=giulia_remote_ready();
    if(!strncmp(cmd,"input.touch ",12)) {
        const char *p=cmd+12;unsigned down=0,move=0,x=0,y=0;
        if(!strncmp(p,"down ",5)) {down=1;p+=5;}
        else if(!strncmp(p,"move ",5)) {down=move=1;p+=5;}
        else if(!strncmp(p,"up ",3))p+=3;
        else if(!strncmp(p,"cancel ",7))p+=7;
        else goto invalid;
        if(!ginput_uint(&p,65535,&x) || !ginput_uint(&p,65535,&y) || !ginput_end(p))goto invalid;
        if(!(ready&1))goto unavailable;
        if(move && !ginput.touch)goto invalid;
        if(down && !move && ginput.touch)giulia_remote_touch(0,ginput.x,ginput.y);
        if(!giulia_remote_touch(down,x,y))goto unavailable;
        ginput.touch=down;ginput.x=x;ginput.y=y;ginput.deadline=now+3000;return 1;
    }
    if(!strncmp(cmd,"input.key ",10)) {
        const char *p=cmd+10;unsigned usage,down,modifiers;
        if(!ginput_uint(&p,231,&usage) || usage<4 || !ginput_uint(&p,1,&down) ||
           !ginput_uint(&p,255,&modifiers) || !ginput_end(p))goto invalid;
        if(!(ready&2))goto unavailable;
        /* Modifier usages are represented by the modifier mask, avoiding a
         * second owner for the same HID key during release or cancellation. */
        if(usage>=224) {
            unsigned mask=1u<<(usage-224);
            modifiers=down?(modifiers|mask):(modifiers&~mask);
        }
        for(unsigned bit=0;bit<8;bit++)if((ginput.modifiers^modifiers)&(1u<<bit)) {
            if(!giulia_remote_hid(7,224+bit,(modifiers>>bit)&1u))goto unavailable;
            ginput.modifiers^=1u<<bit;
        }
        if(usage<224) {
            if(!giulia_remote_hid(7,usage,down))goto unavailable;
            if(down)ginput.keys[usage/8]|=(uint8_t)(1u<<(usage%8));
            else ginput.keys[usage/8]&=(uint8_t)~(1u<<(usage%8));
        }
        ginput.deadline=now+3000;return 1;
    }
    if(!strncmp(cmd,"input.button ",13)) {
        const char *p=cmd+13;unsigned key,down;
        if(!strncmp(p,"power ",6)) {key=0;p+=6;}
        else if(!strncmp(p,"volumeup ",9)) {key=1;p+=9;}
        else if(!strncmp(p,"volumedown ",11)) {key=2;p+=11;}
        else if(!strncmp(p,"home ",5)) {key=3;p+=5;}
        else if(!strncmp(p,"back ",5)) {key=4;p+=5;}
        else goto invalid;
        if(!ginput_uint(&p,1,&down) || !ginput_end(p))goto invalid;
        if(key==4) {
            if(!(ready&2) || !giulia_remote_hid(7,41,down))goto unavailable;
            if(down)ginput.keys[41/8]|=1u<<(41%8);
            else ginput.keys[41/8]&=(uint8_t)~(1u<<(41%8));
        } else {
            static const unsigned usages[]={0x30,0xe9,0xea,0x40};
            if(!(ready&4) || !giulia_remote_hid(12,usages[key],down))goto unavailable;
            if(down)ginput.buttons|=1u<<key;else ginput.buttons&=~(1u<<key);
        }
        ginput.deadline=now+3000;return 1;
    }
invalid:
    printf("GIULIA INPUT: rejected=invalid-command\n");return 1;
unavailable:
    ginput_release();printf("GIULIA INPUT: rejected=service-unavailable\n");ginput_status();return 1;
}
