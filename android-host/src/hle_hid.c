// Android button input, with upstream's press-edge latch for slow game frames.
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx,ly,rx,ry; uint32_t attributes,pad; } NpadState;
typedef struct { int64_t sampling; int32_t count,pad; uint8_t touches[16][0x28]; } TouchState16;
_Static_assert(sizeof(NpadState)==0x28, "Npad ABI");
_Static_assert(sizeof(TouchState16)==0x290, "Touch ABI");
static _Atomic int64_t pad_sample, touch_sample;
_Atomic int g_dm_l_presses;
static _Atomic uint64_t pad_buttons, pad_pressed, pad_stick, pad_right;
static _Atomic uint64_t sampled_right;
void dm_input_sampled_right(int32_t *x,int32_t *y) {
    uint64_t axes=atomic_load(&sampled_right);*x=(int32_t)axes;*y=(int32_t)(axes>>32);
}
void dm_aim_cone_pump(void);
uint64_t dm_hid_samples(void) { return atomic_load(&pad_sample); }
void dm_hid_right(int32_t x,int32_t y) { atomic_store(&pad_right,(uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y<<32)); }
void dm_hid_stick(int32_t x,int32_t y) { atomic_store(&pad_stick,(uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y<<32)); }
void dm_hid_buttons(uint64_t buttons,int reset) {
    uint64_t old=atomic_exchange(&pad_buttons,buttons);
    if(reset)atomic_store(&pad_pressed,0);
    else atomic_fetch_or(&pad_pressed,buttons&~old);
}
/* Front-end mouse path from upstream dm_cursor.c (fixed guest 94012).
 * JNI only queues normalized taps. Guest UI calls run at GetNpadState on the
 * game thread, with two presented frames of hover before down, then up.
 * This is the basic frontend path; item hit testing/dragging stays separate. */
#include "dm_host.h"
#include <pthread.h>
static _Atomic uint64_t input_frame;
void dm_android_input_frame_tick(void) { atomic_fetch_add(&input_frame,1); }
static pthread_mutex_t mouse_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { float x,y; } mouse_queue[16];
static unsigned mouse_head,mouse_count;
int dm_restart_busy(void);
void dm_android_ui_tap(float x,float y) {
    if(dm_restart_busy())return;
    pthread_mutex_lock(&mouse_lock);
    if(mouse_count<16) {
        unsigned at=(mouse_head+mouse_count++)%16;
        mouse_queue[at].x=x<0?0:x>1?1:x;
        mouse_queue[at].y=y<0?0:y>1?1:y;
    }
    pthread_mutex_unlock(&mouse_lock);
}
void dm_android_ui_cancel(void) {
    pthread_mutex_lock(&mouse_lock);mouse_count=0;pthread_mutex_unlock(&mouse_lock);
}
static uint64_t mouse_hash(const char *s) {
    uint64_t h=0xcbf29ce484222325ull;while(*s)h=(h^(uint8_t)*s++)*0x100000001b3ull;return h;
}
static void mouse_send(module_t *m,uint64_t root,const char *action,int x,int y) {
    uint64_t payload[16]={0},msg[8]={0};
    payload[0]=(uint64_t)(uint32_t)x<<32;payload[1]=(uint32_t)y;
    msg[0]=mouse_hash("InputMessage");msg[1]=mouse_hash(action);msg[2]=(uint64_t)payload;
    uint64_t vt=0,fn=0;memcpy(&vt,(void *)root,8);memcpy(&fn,(void *)(vt+0x28),8);
    *(int32_t *)(m->base+0x2d128f8)=x;*(int32_t *)(m->base+0x2d128fc)=y;
    ((void (*)(uint64_t,void *))fn)(root,msg);
}
#include "frontend_android.inc"
static void mouse_pump(void) {
    static uint64_t last_frame=UINT64_MAX;
    static int phase;static float nx,ny;
    if(dm_restart_busy()){phase=0;dm_android_ui_cancel();return;}
    uint64_t frame=atomic_load(&input_frame);if(frame==last_frame)return;
    module_t *m=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
    if(!m || m->size<0x2f05460)return;
    uint64_t root=0;memcpy(&root,m->base+0x2f05458,8);
    int w=*(int32_t *)(m->base+0x211da38),h=*(int32_t *)(m->base+0x211da3c);
    if(!root || w<=0 || h<=0)return;
    if(!phase) {
        pthread_mutex_lock(&mouse_lock);
        if(mouse_count){nx=mouse_queue[mouse_head].x;ny=mouse_queue[mouse_head].y;mouse_head=(mouse_head+1)%16;mouse_count--;phase=1;}
        pthread_mutex_unlock(&mouse_lock);
        if(!phase)return;
    }
    last_frame=frame;
    int x=(int)(nx*w),y=(int)(ny*h);if(x>=w)x=w-1;if(y>=h)y=h-1;
    if(phase==1 && frontend_tap_prepare(m,root,nx,ny,x,y)){phase=0;return;}
    mouse_send(m,root,"MouseMove",x,y);
    if(phase==3)mouse_send(m,root,"LeftMouseButtonDown",x,y);
    if(phase==4){mouse_send(m,root,"LeftMouseButtonUp",x,y);LOG("Android UI mouse click (%d,%d)",x,y);phase=0;}
    else phase++;
}
// Fixed docked bootstrap profile: controller 1P, full-key style.
static uint32_t h_hid_GetStyle(const uint32_t *id) { return *id==0 ? 1 : 0; }
static void fill(NpadState *s,const uint32_t *id,uint32_t style) {
    dm_android_panel_state_pump();
    dm_ui_frontend_pump();
    mouse_pump();
    memset(s,0,sizeof *s);
    s->sampling=atomic_fetch_add(&pad_sample,1)+1;
    s->attributes=(h_hid_GetStyle(id)&style) ? 1 : 0;
    if(s->attributes) {
        s->buttons=atomic_load(&pad_buttons)|atomic_exchange(&pad_pressed,0)|frontend_buttons();
        uint64_t axes=atomic_load(&pad_stick); s->lx=(int32_t)axes; s->ly=(int32_t)(axes>>32);
        axes=atomic_load(&pad_right);s->rx=(int32_t)axes;s->ry=(int32_t)(axes>>32);
    }
    atomic_store(&sampled_right,(uint32_t)s->rx|((uint64_t)(uint32_t)s->ry<<32));
    dm_aim_cone_pump();
    { static uint64_t prev; if ((s->buttons & DM_BTN_L) && !(prev & DM_BTN_L)) { int n = atomic_fetch_add(&g_dm_l_presses, 1) + 1; if (n <= 3) LOG("取手柄处：「显示物品」键（L）交给了游戏，本进程第 %d 次", n); } prev = s->buttons; }   // 「显示物品」键（L）交给游戏的按下次数（首领掉落读数用，dm_bossrush.c） | number of presses of the 「显示物品」(Show Items) key (L) delivered to the game (used by the boss drop readout, dm_bossrush.c)
}
static void h_hid_InitNpad(void) {}
static void h_hid_InitTouch(void) {}
static void h_hid_SetIds(const uint32_t *ids,uint64_t n) { (void)ids; (void)n; }
static void h_hid_SetStyles(uint32_t styles) { (void)styles; }
static void h_hid_SetHhMode(int m) { (void)m; }
static void h_hid_GetFullKey(NpadState *s,const uint32_t *id) { fill(s,id,1); }
static void h_hid_GetHandheld(NpadState *s,const uint32_t *id) { fill(s,id,2); }
static unsigned h_hid_VibPermit(void) { return 0; }
static int h_hid_GetVibHandles(void *out,int max,const uint32_t *id,uint32_t style) { (void)out; (void)max; (void)id; (void)style; return 0; }
static void h_hid_InitVib(const void *h) { (void)h; }
static void h_hid_SendVib(const void *h,const void *v) { (void)h; (void)v; }
static int h_hid_GetTouch(TouchState16 *out,int n) {
    if(n<1) return 0;
    memset(out,0,sizeof *out);
    out->sampling=atomic_fetch_add(&touch_sample,1)+1;
    return 1;
}
typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *hid_force_lookup(const char *name) {
    static const hle_ent F[]={
        {"_ZN2nn3hid14InitializeNpadEv", (void *)h_hid_InitNpad},
        {"_ZN2nn3hid21InitializeTouchScreenEv", (void *)h_hid_InitTouch},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", (void *)h_hid_SetIds},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", (void *)h_hid_SetStyles},
        {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", (void *)h_hid_SetHhMode},
        {"_ZN2nn3hid15GetNpadStyleSetERKj", (void *)h_hid_GetStyle},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", (void *)h_hid_GetFullKey},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", (void *)h_hid_GetHandheld},
        {"_ZN2nn3hid20IsVibrationPermittedEv", (void *)h_hid_VibPermit},
        {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", (void *)h_hid_GetVibHandles},
        {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", (void *)h_hid_InitVib},
        {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", (void *)h_hid_SendVib},
        {"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", (void *)h_hid_GetTouch},
    };
    for(unsigned i=0;i<sizeof F/sizeof F[0];i++)
        if(!strcmp(name,F[i].name)) return &F[i];
    return 0;
}
