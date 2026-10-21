// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include "../scripts/ufs-read/GiuliaSmmuTakeover.h"
int main() {
  std::array<uint32_t,0x100000/4> r{};
  auto reset=[&] {
    r.fill(0);r[0x20/4]=0x4c017e6b;r[0x24/4]=0x60000047;r[0x28/4]=0x5111;
    r[0x808/4]=0x80000060;r[0xc08/4]=2;
    r[(128+2)*4096/4]=1; // Existing firmware translation remains untouched.
  };
  auto read=[&](unsigned at){return r.at(at/4);};
  auto plan=[&]{return planGiuliaSmmu(0x60,0x882f6a000ULL,0x22000,read);};
  reset();auto p=plan();
  assert(!p.target.error && p.retarget && p.target.slot==2 && p.target.bank==70);
  std::vector<std::pair<unsigned,uint32_t>> writes;
  auto write=[&](unsigned at,uint32_t value){writes.emplace_back(at,value);r.at(at/4)=value;};
  assert(configureGiuliaSmmu(p,0x60,false,read,write) && writes.empty());
  assert(!configureGiuliaSmmu(p,0x60,true,read,write));
  assert(r[0xc08/4]==70 && r[0x808/4]==0x80000060);
  for(auto [at,value]:writes) {
    (void)value;
    assert(at!=(128+2)*4096 && at!=0x1000+2*4 && at!=0x1800+2*4);
  }
  assert(r[(128+2)*4096/4]==1);
  p=plan();writes.clear();assert(!p.target.error && p.target.reuse && !p.retarget);
  assert(!configureGiuliaSmmu(p,0x60,true,read,write) && writes.empty());
  reset();p=plan();r[0xc08/4]=3;writes.clear();
  assert(configureGiuliaSmmu(p,0x60,true,read,write) && writes.empty());
  reset();r[0x80c/4]=0x80000060;r[0xc0c/4]=3;assert(plan().target.error);
  reset();r[0x808/4]=0x80010060;assert(plan().target.error);
  reset();r[0x80c/4]=0x80000061;r[0xc0c/4]=70;
  p=plan();assert(!p.target.error && p.target.bank==69);
  reset();r[(128+70)*4096/4]=1;p=plan();assert(!p.target.error && p.target.bank==69);
  reset();assert(planGiuliaSmmu(0x60,0x1000000000ULL,4096,read).target.error);
  reset();p=plan();writes.clear();
  auto dropped=[&](unsigned at,uint32_t value){
    writes.emplace_back(at,value);
    if(at!=0x1800+4*70)r.at(at/4)=value;
  };
  assert(configureGiuliaSmmu(p,0x60,true,read,dropped));
  assert(r[0xc08/4]==2 && r[(128+2)*4096/4]==1);
  puts("PASS: captured SM8650 geometry, exclusive SID retarget, old-bank preservation, quiescence, races, conflicts and rejected writes");
}
