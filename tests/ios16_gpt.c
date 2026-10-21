#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define VERMEER_HOST_TEST 1
#include "vermeer_gpt.h"
int main(int argc,char **argv) {
 assert(argc==2);
 uint8_t data[24576]; FILE *f=fopen(argv[1],"rb"); assert(f);
 assert(fread(data,1,sizeof(data),f)==sizeof(data)); fclose(f);
 struct vermeer_gpt g;
 assert(vg_header(data+4096,61321215,&g)==0);
 assert(g.entries_bytes==4096 && g.entries_lba==2);
 assert(vg_userdata(data+8192,&g)==0);
 assert(g.first==3920350 && g.blocks==57400861);
 data[8192+4095]^=1;
 assert(vg_userdata(data+8192,&g)!=0);
 return 0;
}
