// Linux/Android plumbing for the shared upstream dirty-page state machine.
#include <signal.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include <link.h>
static uint64_t dirty_clock_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static int (*dirty_fault_hook)(void *,int,void *);
static _Atomic(void (*)(void *,size_t)) dirty_write_hook;
static struct sigaction dirty_previous;
static int dirty_translation;
static uintptr_t dirty_page_mask;
typedef struct { uintptr_t lo,hi; } dirty_code_range;
static dirty_code_range dirty_guest_code[512];
static unsigned dirty_guest_code_count;
static void dirty_code_add(uintptr_t lo,uintptr_t hi) {
    if(!lo || hi<=lo)return;
    for(unsigned i=0;i<dirty_guest_code_count;i++)
        if(dirty_guest_code[i].lo==lo && dirty_guest_code[i].hi==hi)return;
    if(dirty_guest_code_count<sizeof dirty_guest_code/sizeof *dirty_guest_code)
        dirty_guest_code[dirty_guest_code_count++]=(dirty_code_range){lo,hi};
}
static int dirty_loaded_arm_code(struct dl_phdr_info *info,size_t size,void *arg) {
    (void)size;(void)arg;
    const ElfW(Ehdr) *header=NULL;
    // Validate architecture from a readable, mapped ELF header: the native
    // bridge may enumerate x86 helpers too, which must never be ARM-decoded.
    for(unsigned i=0;i<info->dlpi_phnum;i++) {
        const ElfW(Phdr) *p=&info->dlpi_phdr[i];
        if(p->p_type==PT_LOAD && p->p_offset==0 && (p->p_flags&PF_R) && p->p_filesz>=sizeof *header) {
            header=(const ElfW(Ehdr) *)(info->dlpi_addr+p->p_vaddr);break;
        }
    }
    if(!header || memcmp(header->e_ident,ELFMAG,SELFMAG) ||
       header->e_ident[EI_CLASS]!=ELFCLASS64 || header->e_ident[EI_DATA]!=ELFDATA2LSB ||
       header->e_machine!=EM_AARCH64)return 0;
    for(unsigned i=0;i<info->dlpi_phnum;i++) {
        const ElfW(Phdr) *p=&info->dlpi_phdr[i];
        if(p->p_type!=PT_LOAD || (p->p_flags&(PF_R|PF_X))!=(PF_R|PF_X))continue;
        uintptr_t lo=info->dlpi_addr+p->p_vaddr;
        if(p->p_memsz<=UINTPTR_MAX-lo)dirty_code_add(lo,lo+p->p_memsz);
    }
    return 0;
}
static int dirty_context_fd=-1;
static _Atomic unsigned dirty_context_logged;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"signal diagnostic requires lock-free flag");
static char *dirty_text(char *out,const char *text) {
    while(*text)*out++=*text++;
    return out;
}
static char *dirty_hex(char *out,uint64_t value) {
    static const char digits[]="0123456789abcdef";
    *out++='0';*out++='x';
    for(int shift=60;shift>=0;shift-=4)*out++=digits[(value>>shift)&15];
    return out;
}
static void dirty_context_record(siginfo_t *info,void *context) {
    if(!dirty_translation || atomic_exchange_explicit(&dirty_context_logged,1,memory_order_relaxed))return;
    int saved_errno=errno;char line[2048],*out=line;
    out=dirty_text(out,"dirty unclassified ARM ACCERR si_addr=");out=dirty_hex(out,(uintptr_t)info->si_addr);
#if defined(__aarch64__)
    if(context) {
        const ucontext_t *uc=context;
        out=dirty_text(out," pc=");out=dirty_hex(out,uc->uc_mcontext.pc);
        out=dirty_text(out," sp=");out=dirty_hex(out,uc->uc_mcontext.sp);
        out=dirty_text(out," lr=");out=dirty_hex(out,uc->uc_mcontext.regs[30]);
        out=dirty_text(out," fault_address=");out=dirty_hex(out,uc->uc_mcontext.fault_address);
        for(unsigned i=0;i<31;i++) {
            out=dirty_text(out," x");*out++='0'+i/10;*out++='0'+i%10;*out++='=';
            out=dirty_hex(out,uc->uc_mcontext.regs[i]);
        }
        const unsigned char *p=(const unsigned char *)uc->uc_mcontext.__reserved;
        size_t left=sizeof uc->uc_mcontext.__reserved;
        for(unsigned i=0;i<16 && left>=8;i++) {
            uint32_t magic,size;memcpy(&magic,p,4);memcpy(&size,p+4,4);
            out=dirty_text(out," ctx=");out=dirty_hex(out,magic);*out++='/';out=dirty_hex(out,size);
            if(size<8 || size>left)break;
            if(magic==0x45535201 && size>=16) {
                uint64_t esr;memcpy(&esr,p+8,8);out=dirty_text(out," esr=");out=dirty_hex(out,esr);
            }
            p+=size;left-=size;
        }
    }
#endif
    *out++='\n';
    if(dirty_context_fd>=0)(void)write(dirty_context_fd,line,(size_t)(out-line));
    (void)write(STDERR_FILENO,line,(size_t)(out-line));errno=saved_errno;
}
// Decode only ordinary scalar/SIMD stores and store pairs. Unknown encodings,
// atomic/structure stores and PCs outside verified ARM code are not guessed.
static int dirty_guest_store(void *context,uintptr_t fault) {
#if defined(__aarch64__)
    if(!dirty_translation || !context)return 0;
    const ucontext_t *uc=context;uintptr_t pc=uc->uc_mcontext.pc;
    if(!pc || (pc&3) || (pc&dirty_page_mask)==(fault&dirty_page_mask))return 0;
    unsigned i;
    for(i=0;i<dirty_guest_code_count;i++)if(pc>=dirty_guest_code[i].lo &&
        pc<dirty_guest_code[i].hi && dirty_guest_code[i].hi-pc>=4)break;
    if(i==dirty_guest_code_count)return 0;
    uint32_t ins;memcpy(&ins,(const void *)pc,4);
    unsigned rn=(ins>>5)&31,opc=(ins>>22)&3,scale=ins>>30,vector=(ins>>26)&1;
    uint64_t address=rn==31?uc->uc_mcontext.sp:uc->uc_mcontext.regs[rn];
    size_t bytes;int64_t offset=0;
    if((ins&0x3b000000)==0x39000000 || (ins&0x3b200000)==0x38000000) {
        // LDR/LDRSW/PRFM share this class; only opc=0 (or SIMD Q opc=2)
        // actually stores. Post-index faults use the original base register.
        if(opc==0)bytes=(size_t)1<<scale;
        else if(vector && opc==2 && scale==0)bytes=16;
        else return 0;
        if((ins&0x3b000000)==0x39000000)offset=((ins>>10)&4095)*bytes;
        else {
            unsigned mode=(ins>>10)&3,imm=(ins>>12)&511;
            if(vector && mode==2)return 0; // reserved SIMD unprivileged form
            if(mode!=1)offset=(int32_t)(imm^256)-256;
        }
    } else if((ins&0x3b200c00)==0x38200800) {
        // Register-offset STR only. Observed guest instruction b823491f is
        // STR WZR,[X8,W3,UXTW]; retain exact store width/address validation.
        if(opc==0)bytes=(size_t)1<<scale;
        else if(vector && opc==2 && scale==0)bytes=16;
        else return 0;
        unsigned rm=(ins>>16)&31,option=(ins>>13)&7;
        uint64_t index=rm==31?0:uc->uc_mcontext.regs[rm];
        if(option==2)index=(uint32_t)index;             // UXTW
        else if(option==6)index=(uint64_t)(int64_t)(int32_t)(uint32_t)index; // SXTW
        else if(option!=3 && option!=7)return 0;       // LSL / SXTX
        // Unsigned arithmetic implements architectural modulo-64-bit address
        // calculation, including negative extended offsets, without C overflow.
        if(ins&(1u<<12))index*=bytes;
        address+=index;
    } else if((ins&0x3a000000)==0x28000000 && !(ins&(1u<<22))) {
        // Pair opc is bits31:30 (not the scalar opc above).
        if(vector) { if(scale==3)return 0;bytes=(size_t)4<<scale; }
        else { if(scale!=0 && scale!=2)return 0;bytes=scale==2?8:4; }
        unsigned mode=(ins>>23)&3,imm=(ins>>15)&127;
        if(mode!=1)offset=((int32_t)(imm^64)-64)*(int64_t)bytes;
        bytes*=2;
    } else return 0;
    address+=(uint64_t)offset;
    return fault>=address && fault-address<bytes;
#else
    return 0;
#endif
}
static int dirty_is_write(void *context,uintptr_t fault) {
    if(!context)return 0;
#if defined(__aarch64__)
    // Linux exports ESR in the extensible AArch64 signal context records.
    ucontext_t *uc=context;
    const unsigned char *p=(const unsigned char *)uc->uc_mcontext.__reserved;
    size_t remaining=sizeof uc->uc_mcontext.__reserved;
    while(remaining>=8) {
        uint32_t magic,size;memcpy(&magic,p,4);memcpy(&size,p+4,4);
        if(size<8 || size>remaining)break;
        if(magic==0x45535201 && size>=16) {
            uint64_t esr;memcpy(&esr,p+8,8);
            unsigned ec=(unsigned)(esr>>26)&63;
            return (ec==0x24 || ec==0x25) && (esr&(1u<<6));
        }
        p+=size;remaining-=size;
    }
#endif
    return dirty_guest_store(context,fault);
}
static void dirty_signal(int sig,siginfo_t *info,void *context) {
    if(info->si_code==SEGV_ACCERR) {
        int is_write=dirty_is_write(context,(uintptr_t)info->si_addr);
        if(dirty_fault_hook && dirty_fault_hook(info->si_addr,is_write,context))return;
        if(!is_write)dirty_context_record(info,context);
    }
    if(dirty_previous.sa_handler==SIG_DFL || dirty_previous.sa_handler==SIG_IGN) {
        // Retry the fault with the original disposition, preserving Android's
        // crash reporting instead of hiding an unrelated memory error.
        sigaction(sig,&dirty_previous,NULL);return;
    }
    if(dirty_previous.sa_flags&SA_SIGINFO)dirty_previous.sa_sigaction(sig,info,context);
    else dirty_previous.sa_handler(sig);
}
static void dm_fault_hook_register(int (*hook)(void *,int,void *)) {
    extern unsigned char dm_no_fp_state;
    dirty_translation=dm_no_fp_state!=0;
    if(dirty_translation) {
        const char *root=getenv("DM_ROOT");char path[1200];
        if(root) {
            int n=snprintf(path,sizeof path,"%s/dirty-context.log",root);
            if(n>0 && (size_t)n<sizeof path)
                dirty_context_fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
        }
        // Module metadata is immutable before NVN pools are registered. Copy
        // bounds here so the signal path never consults loader state or maps.
        dirty_page_mask=~((uintptr_t)getpagesize()-1);
        dirty_guest_code_count=0;
        for(int i=0;i<g_nmod && i<MAX_MOD;i++) {
            const module_t *m=&g_mod[i];uintptr_t base=(uintptr_t)m->base;
            if(m->text_end && m->text_end<=m->size)
                dirty_code_add(base,base+m->text_end);
            if(m->patch_off<m->size)
                dirty_code_add(base+m->patch_off,base+m->size);
        }
        // ARM libc's translated code is r-- in /proc/maps. ELF PF_X conveys
        // guest execute permission; never whitelist arbitrary readable maps.
        // These startup libraries remain loaded for this game's lifetime.
        dl_iterate_phdr(dirty_loaded_arm_code,NULL);
        LOG("Translation dirty tracking: %u verified ARM code segments",dirty_guest_code_count);
    }
    dirty_fault_hook=hook;
    struct sigaction action={0};sigemptyset(&action.sa_mask);
    action.sa_sigaction=dirty_signal;action.sa_flags=SA_SIGINFO|SA_ONSTACK;
    if(sigaction(SIGSEGV,&action,&dirty_previous))die("Cannot install texture dirty-page handler");
}
static void dm_guest_write_prep_register(void (*hook)(void *,size_t)) {
    atomic_store(&dirty_write_hook,hook);
}
void dm_guest_write_prep(void *addr,size_t len) {
    void (*hook)(void *,size_t)=atomic_load(&dirty_write_hook);
    if(hook)hook(addr,len);
}
