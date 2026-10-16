#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* SHA-256 (FIPS 180-4), used for upstream's control-block shader identity. */
static uint32_t sk_rotr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void sk_block(uint32_t h[8],const uint8_t b[64]) {
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];for(int i=0;i<16;i++)w[i]=(uint32_t)b[i*4]<<24|(uint32_t)b[i*4+1]<<16|(uint32_t)b[i*4+2]<<8|b[i*4+3];
    for(int i=16;i<64;i++){uint32_t x=w[i-15],y=w[i-2];w[i]=w[i-16]+(sk_rotr(x,7)^sk_rotr(x,18)^(x>>3))+w[i-7]+(sk_rotr(y,17)^sk_rotr(y,19)^(y>>10));}
    uint32_t a=h[0],c=h[2],b0=h[1],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
    for(int i=0;i<64;i++) {
        uint32_t t=z+(sk_rotr(e,6)^sk_rotr(e,11)^sk_rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(sk_rotr(a,2)^sk_rotr(a,13)^sk_rotr(a,22))+((a&b0)^(a&c)^(b0&c));
        z=g;g=f;f=e;e=d+t;d=c;c=b0;b0=a;a=t+u;
    }
    h[0]+=a;h[1]+=b0;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
}
static void shader_key_bytes(const void *data,size_t size,char key[17]) {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *p=data;size_t left=size;
    while(left>=64){sk_block(h,p);p+=64;left-=64;}
    uint8_t tail[128]={0};memcpy(tail,p,left);tail[left]=0x80;
    size_t n=left<56?64:128;uint64_t bits=(uint64_t)size*8;
    for(int i=0;i<8;i++)tail[n-1-i]=(uint8_t)(bits>>(8*i));
    sk_block(h,tail);if(n==128)sk_block(h,tail+64);
    static const char hex[]="0123456789abcdef";
    for(int i=0;i<8;i++){unsigned b=(h[i/4]>>(24-8*(i%4)))&255;key[i*2]=hex[b>>4];key[i*2+1]=hex[b&15];}key[16]=0;
}
