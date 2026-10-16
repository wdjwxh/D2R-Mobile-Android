/* Nativebridge-only equality alternative. Real ARM devices keep bionic's
 * optimized memcmp; dm_no_fp_state identifies the libndk_translation path.
 * Every load stays inside the requested range, including unaligned tails. */
__attribute__((noinline,target("no-neon,no-sve")))
static size_t dm_vk_scalar_equal_prefix(const void *a,const void *b,size_t size) {
    const unsigned char *left=a,*right=b;
    size_t original=size;
    // Large resident bindings dominate translated comparison time. Amortize
    // loop/address bookkeeping over four 32-byte groups, using only GPRs.
    // A differing block returns a conservative proven prefix, as below.
#pragma clang loop vectorize(disable) interleave(disable)
    while(size>=128) {
        uint64_t different=0;
#pragma clang loop unroll(full)
        for(unsigned group=0;group<4;group++) {
            const unsigned char *l=left+group*32,*r=right+group*32;
            uint64_t a0,a1,a2,a3,b0,b1,b2,b3;
            __builtin_memcpy(&a0,l,8);__builtin_memcpy(&a1,l+8,8);
            __builtin_memcpy(&a2,l+16,8);__builtin_memcpy(&a3,l+24,8);
            __builtin_memcpy(&b0,r,8);__builtin_memcpy(&b1,r+8,8);
            __builtin_memcpy(&b2,r+16,8);__builtin_memcpy(&b3,r+24,8);
            different|=(a0^b0)|(a1^b1)|(a2^b2)|(a3^b3);
            __asm__("" : "+r"(different));
        }
        if(different)return original-size;
        left+=128;right+=128;size-=128;
    }
#pragma clang loop vectorize(disable) interleave(disable)
    while(size>=32) {
        uint64_t a0,a1,a2,a3,b0,b1,b2,b3;
        __builtin_memcpy(&a0,left,8);
        __builtin_memcpy(&a1,left+8,8);
        __builtin_memcpy(&a2,left+16,8);
        __builtin_memcpy(&a3,left+24,8);
        __builtin_memcpy(&b0,right,8);
        __builtin_memcpy(&b1,right+8,8);
        __builtin_memcpy(&b2,right+16,8);
        __builtin_memcpy(&b3,right+24,8);
        uint64_t different=(a0^b0)|(a1^b1)|(a2^b2)|(a3^b3);
        // Materialize the combined difference in a GPR. Without this empty
        // constraint LLVM rewrites the reduction into four early branches.
        // This emits no instruction and is not a memory/synchronization fence.
        __asm__("" : "+r"(different));
        if(different)return original-size;
        left+=32;right+=32;size-=32;
    }
#pragma clang loop vectorize(disable) interleave(disable)
    while(size>=8) {
        uint64_t av,bv;
        __builtin_memcpy(&av,left,8);__builtin_memcpy(&bv,right,8);
        if(av!=bv)return original-size;
        left+=8;right+=8;size-=8;
    }
#pragma clang loop vectorize(disable) interleave(disable)
    while(size) {
        if(*left!=*right)return original-size;
        left++;right++;size--;
    }
    return original;
}
// Returns a proven equal prefix; a differing 128/32/8-byte block is deliberately
// left for the caller's existing exact page path. Native ARM keeps bionic.
static size_t dm_vk_equal_prefix(const void *a,const void *b,size_t size) {
    if(!dm_no_fp_state)return memcmp(a,b,size)==0?size:0;
    return dm_vk_scalar_equal_prefix(a,b,size);
}
static int dm_vk_bytes_equal(const void *a,const void *b,size_t size) {
    return dm_vk_equal_prefix(a,b,size)==size;
}
