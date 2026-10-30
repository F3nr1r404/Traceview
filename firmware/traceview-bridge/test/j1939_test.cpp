// Host unit test for the J1939 decoder:  g++ -std=c++17 j1939_test.cpp -o jt && ./jt
#include "../src/j1939.h"
#include <assert.h>
using namespace j1939;
uint32_t mk(uint8_t prio,uint32_t pgn,uint8_t sa,uint8_t da=0xFF){uint8_t pf=(pgn>>8)&0xFF;uint32_t id=((uint32_t)prio<<26)|((pgn>>16&3)<<24)|((uint32_t)pf<<16)|((pf<240?da:(pgn&0xFF))<<8)|sa;return id;}
void dtcBytes(uint8_t*o,uint32_t spn,uint8_t fmi,uint8_t oc){o[0]=spn&0xFF;o[1]=(spn>>8)&0xFF;o[2]=((spn>>11)&0xE0)|(fmi&0x1F);o[3]=oc&0x7F;}
int main(){Decoder D;char buf[600];
 // single-frame DM1 from engine: amber lamp on, SPN 168 FMI 1 OC 3
 uint8_t f[8]={0x04,0xFF,0,0,0,0,0xFF,0xFF};dtcBytes(f+2,168,1,3);
 D.onFrame(mk(6,PGN_DM1,0),true,f,8,1000);
 assert(D.nDtc==1&&D.dtc[0].spn==168&&D.dtc[0].fmi==1&&D.dtc[0].oc==3);
 assert(D.lamps(1000).awl==1&&D.lamps(1000).mil==0);
 // BAM DM1 from aftertreatment (SA 61) with 3 codes incl. SPN > 65535 (520192? use 520348)
 uint8_t pl[14];pl[0]=0x40;pl[1]=0xFF;dtcBytes(pl+2,4364,18,1);dtcBytes(pl+6,3226,2,5);dtcBytes(pl+10,520348,31,1);
 uint8_t cm[8]={32,14,0,2,0xFF,(uint8_t)(PGN_DM1&0xFF),(uint8_t)((PGN_DM1>>8)&0xFF),0};
 D.onFrame(mk(7,PGN_TP_CM,61,0xFF),true,cm,8,1100);
 uint8_t dt1[8]={1};memcpy(dt1+1,pl,7);uint8_t dt2[8]={2};memset(dt2+1,0xFF,7);memcpy(dt2+1,pl+7,7);
 D.onFrame(mk(7,PGN_TP_DT,61,0xFF),true,dt1,8,1150);D.onFrame(mk(7,PGN_TP_DT,61,0xFF),true,dt2,8,1200);
 assert(D.nDtc==4);bool big=false;for(int i=0;i<D.nDtc;i++)if(D.dtc[i].spn==520348&&D.dtc[i].fmi==31)big=true;assert(big);
 assert(D.lamps(1200).mil==1);
 // engine clears codes (SPN0 FMI0)
 uint8_t c[8]={0,0xFF,0,0,0,0,0xFF,0xFF};D.onFrame(mk(6,PGN_DM1,0),true,c,8,2000);assert(D.nDtc==3);
 // expiry of SA61 after 5 s
 D.expire(6300);assert(D.nDtc==0);
 // live
 uint8_t e[8]={0,0,0,(uint8_t)(5200&0xFF),(uint8_t)(5200>>8),0,0,0};D.onFrame(mk(3,PGN_EEC1,0),true,e,8,7000);assert((int)D.live.rpm==650);
 uint8_t v[8]={0,0,0,0,(uint8_t)(278&0xFF),(uint8_t)(278>>8),0xFF,0xFF};D.onFrame(mk(6,PGN_VEP1,0),true,v,8,7000);assert(D.live.batV>13.89&&D.live.batV<13.91);
 uint8_t t1[8]={125,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};D.onFrame(mk(6,PGN_ET1,0),true,t1,8,7000);assert((int)D.live.coolF==185);
 uint8_t s[8]={0,0,0,0,0,0,0,0};uint16_t sp=(uint16_t)(88.5*256);s[1]=sp&0xFF;s[2]=sp>>8;D.onFrame(mk(6,PGN_CCVS1,17),true,s,8,7000);
 // 11-bit frames ignored
 uint32_t fr=D.frames;D.onFrame(0x123,false,s,8,7000);assert(D.frames==fr);
 D.jsonLive(buf,600,7100);printf("%s",buf);D.jsonCodes(buf,600,7100);printf("%s",buf);
 // VIN via BAM
 const char*vin="1FUJHHDR2NLAB1234*";uint8_t cm2[8]={32,18,0,3,0xFF,(uint8_t)(PGN_VI&0xFF),(uint8_t)((PGN_VI>>8)&0xFF),0};
 D.onFrame(mk(6,PGN_TP_CM,0,0xFF),true,cm2,8,8000);for(int k=1;k<=3;k++){uint8_t p[8];p[0]=k;memset(p+1,0xFF,7);for(int j=0;j<7;j++){int ix=(k-1)*7+j;if(ix<18)p[1+j]=vin[ix];}D.onFrame(mk(7,PGN_TP_DT,0,0xFF),true,p,8,8000+k);}
 printf("VIN=%s\n",D.live.vin);assert(!strcmp(D.live.vin,"1FUJHHDR2NLAB1234"));
 // parseId PDU1
 Id x=parseId(mk(6,PGN_TP_CM,0x21,0x00));assert(x.pgn==PGN_TP_CM&&x.da==0&&x.sa==0x21);
 puts("ALL J1939 TESTS PASS");}
