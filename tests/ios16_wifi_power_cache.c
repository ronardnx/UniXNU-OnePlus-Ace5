#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPowerCache.h"
int main(void)
{
  _Alignas(8) unsigned char rc[sizeof(GW_COMMAND)+8],node[sizeof(GW_VOTER)+8];
  GW_COMMAND command={0}; GW_VOTER voter={0}; unsigned data; int valid;
  _Static_assert(sizeof(GW_COMMAND)==24,"resource layout");
  _Static_assert(sizeof(GW_VOTER)==72,"voter layout");
  command.Address=0x43000;command.Voters=(GW_VOTER *)(void *)(node+4);
  voter.Driver=2;voter.Local[0].Data=1200;voter.Local[0].Valid=1;
  memcpy(rc+4,&command,sizeof(command));memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_OK && data==1200 && valid);
  assert(gw_cached_vote(rc+4,0x44000,&data,&valid)==GW_CACHE_BAD);
  assert(gw_cached_vote(rc+1,0x43000,&data,&valid)==GW_CACHE_BAD);
  assert(gw_cached_vote(NULL,0x43000,&data,&valid)==GW_CACHE_OK && !valid);
  voter.Local[0].Dirty=2;memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_BUSY);
  voter.Local[0].Dirty=0;voter.Local[0].Valid=2;memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_BAD);
  voter.Local[0].Valid=0;memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_OK && !valid);
  voter.Driver=1;voter.Next=NULL;memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_OK && !valid);
  voter.Next=(GW_VOTER *)(void *)(node+4);memcpy(node+4,&voter,sizeof(voter));
  assert(gw_cached_vote(rc+4,0x43000,&data,&valid)==GW_CACHE_BAD);
  puts("RPMh cache: 9 unaligned-copy, unknown-vote, state and cycle cases passed");
  return 0;
}
