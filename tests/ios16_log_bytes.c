#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/common/giulia_log_bytes.h"
int main(void) {
    unsigned char bytes[65]; char text[129];
    for (unsigned page=0;page<4;page++) {
        for (unsigned i=0;i<64;i++) bytes[i]=(unsigned char)(page*64+i);
        assert(glb_hex(bytes,64,text,sizeof(text)));
        assert(strlen(text)==128);
        for (unsigned i=0;i<64;i++) { unsigned n=999; assert(sscanf(text+2*i,"%2x",&n)==1);assert(n==bytes[i]); }
    }
    assert(!glb_hex(bytes,65,text,sizeof(text)));
    assert(!glb_hex(bytes,64,text,128));
    assert(!glb_hex(NULL,1,text,sizeof(text)));
    assert(glb_hex(NULL,0,text,sizeof(text)) && !text[0]);
    puts("All byte values, NUL, chunk boundaries and capacity bounds passed");
    return 0;
}
