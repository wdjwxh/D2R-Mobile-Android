/* Android adapter for upstream's shared iOS touch geometry and gestures.
 * Keep the core source unchanged; render coordinates come from its layout. */
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_controls.c"
#include "vk_backend.h"
#include <sys/uio.h>
#include <sys/syscall.h>
#include <time.h>
#include "../../d2r-mobile/dm/host/dm_hud.h"
#include "../../d2r-mobile/dm/ios/Sources/dm_touch_native_art.h"
static pthread_mutex_t touch_lock=PTHREAD_MUTEX_INITIALIZER;
static dm_tc_state touch_state;
static dm_tc_scene touch_scene;
static int touch_ready;
static int touch_hud_blocked;
#include "../generated/touch_hud_stack.inc"
static uint64_t touch_scene_epoch=1,touch_world_epoch=1;
static uint64_t touch_keys;
static int frontend_slot=-1;
static float frontend_x,frontend_y;
void dm_android_ui_tap(float,float);
void dm_android_ui_cancel(void);
void dm_hid_stick(int32_t,int32_t);
void dm_hid_right(int32_t,int32_t);
uint64_t dm_hid_samples(void);
static double touch_now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
int dm_restart_busy(void);
int dm_android_restart_cover(void);
int dm_ui_panel_state(void);
int dm_ui_targeting(void);
static void touch_sync_game_state(void) {
    dm_hud_info hi;dm_hud_get(&hi);
    dm_tc_set_holding(&touch_state,hi.valid && hi.holding && !dm_ui_targeting());
    dm_tc_set_panel_state(dm_ui_panel_state());
}
static void touch_publish(void) {
    if(dm_restart_busy()){dm_tc_cancel_all(&touch_state);touch_keys=0;frontend_slot=-1;dm_android_ui_cancel();dm_hid_buttons(0,1);dm_hid_stick(0,0);dm_hid_right(0,0);return;}
    if(touch_state.diff_request){dm_hud_request_players(touch_state.diff_request);touch_state.diff_request=0;}
    dm_hid_buttons(touch_keys|touch_state.buttons,0);
    dm_hid_stick(touch_state.lx,touch_state.ly);dm_hid_right(touch_state.rx,touch_state.ry);
}
static int touch_navigation_mode(void) {
    int nav=!touch_scene.game || touch_scene.panels || (touch_scene.game && touch_hud_blocked>0);
    return dm_tc_panel_mode(nav,&touch_scene,touch_hud_blocked);
}
static void touch_apply_navigation(void) {
    int nav=touch_navigation_mode();
    dm_tc_set_navigation_mode(&touch_state,nav);
    dm_tc_set_joy_block(&touch_state,touch_hud_blocked<0);
    // Keep Android's current controller navigation until the complete pointer
    // flow is linked. Match upstream's dead/dialog masks and HUD cross entry.
    uint32_t mask=nav?(dm_tc_nav_mask(nav!=2)|(nav==3?1u<<DM_TC_MENU:0)):0xfff;
    dm_tc_enable_targets(&touch_state,touch_scene.game?mask:0);
}
static uint64_t touch_hash(const char *s) { uint64_t h=0xcbf29ce484222325ull;while(*s)h=(h^(uint8_t)*s++)*0x100000001b3ull;return h; }
static int touch_read(uint64_t address,void *out,size_t n) {
    if(dm_vk_guest_heap_read(address,out,n))return 1;
    struct iovec local={out,n},remote={(void *)(uintptr_t)address,n};
    return syscall(__NR_process_vm_readv,getpid(),&local,1,&remote,1,0)==(ssize_t)n;
}
void dm_android_frontend_message(const char *,const char *,const char *);
void dm_android_touch_observe(const uint64_t *regs) {
    uint64_t msg[3];if(!touch_read(regs[1],msg,sizeof msg))return;
    const char *categories[]={"PanelManager","FrontEndNavigation","ConsoleCharacterCreate","CharacterSelect","MainMenuHD"};
    const char *cat=NULL;for(unsigned i=0;i<sizeof categories/sizeof categories[0];i++)if(msg[0]==touch_hash(categories[i])){cat=categories[i];break;}
    if(!cat)return;
    const char *actions[]={"OpenPanel","OpenExclusivePanel","ClosePanel","UnloadPanel","UnloadAll","TogglePanel","ToFrontEnd","ToGame","ToCharacterCreate","ToggleCharacterOptions","SelectCharacter","UpdateCharacterInfo"};
    const char *action=NULL;for(unsigned i=0;i<sizeof actions/sizeof actions[0];i++)if(msg[1]==touch_hash(actions[i])){action=actions[i];break;}
    if(!action)return;
    char arg[65]={0};if(msg[2] && !touch_read(msg[2],arg,64))arg[0]=0;
    for(int i=0;i<64 && arg[i];i++)if((unsigned char)arg[i]<32 || (unsigned char)arg[i]>=127){arg[0]=0;break;}
    dm_android_frontend_message(cat,action,arg);
    pthread_mutex_lock(&touch_lock);
    int was_game=touch_scene.game,was_nav=touch_navigation_mode()!=0;
    if(!strcmp(cat,"PanelManager"))touch_hud_blocked=dm_android_hud_stack_msg(action,arg);
    dm_tc_set_panel_state(dm_ui_panel_state());
    (void)dm_tc_navigation(&touch_scene,cat,action,arg);
    int nav=touch_navigation_mode()!=0;
    if(was_game!=touch_scene.game) { touch_world_epoch++;frontend_slot=-1;dm_android_ui_cancel(); }
    if(was_nav!=nav)touch_scene_epoch++;
    dm_touch_game_set_scene(!nav,touch_scene_epoch,touch_world_epoch);
    if(touch_ready) { touch_apply_navigation();touch_publish(); }
    pthread_mutex_unlock(&touch_lock);
    LOG("Android touch scene: %s:%s:%s game=%d navigation=%d",cat,action,arg,touch_scene.game,nav);
}
static void dm_android_keys(uint64_t keys,int reset) {
    pthread_mutex_lock(&touch_lock);touch_keys=keys;
    if(reset)dm_tc_cancel_all(&touch_state);touch_publish();if(reset)dm_hid_buttons(keys,1);
    pthread_mutex_unlock(&touch_lock);
}
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeTouchSize(JNIEnv *env,jobject self,jfloat w,jfloat h) {
    (void)env;(void)self;pthread_mutex_lock(&touch_lock);
    dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(0,0);
    if(touch_ready)dm_tc_resize(&touch_state,w,h);else {dm_tc_init(&touch_state,w,h);touch_ready=1;}
    touch_apply_navigation();
    touch_publish();pthread_mutex_unlock(&touch_lock);
}
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeTouch(JNIEnv *env,jobject self,jint action,jint slot,jfloat x,jfloat y) {
    (void)env;(void)self;pthread_mutex_lock(&touch_lock);touch_sync_game_state();double now=touch_now();
    if(action!=3 && dm_restart_busy()){pthread_mutex_unlock(&touch_lock);return;}
    if(action==3) { dm_tc_cancel_all(&touch_state);frontend_slot=-1;dm_android_ui_cancel(); }
    else if(touch_ready && !touch_scene.game) {
        // front_ios.m uiTouch: first finger only, click at lift coordinates.
        if(action==0 && frontend_slot<0)frontend_slot=slot;
        if(slot==frontend_slot) {
            if(action==0 || action==1){frontend_x=x;frontend_y=y;}
            if(action==2) { dm_android_ui_tap(frontend_x/touch_state.layout.width,frontend_y/touch_state.layout.height);frontend_slot=-1; }
        }
    }
    else if(touch_ready && touch_scene.game) {
        if(action==0)dm_tc_begin_at(&touch_state,slot,x,y,now);
        if(action==1)dm_tc_move_at(&touch_state,slot,x,y,now);
        if(action==2)dm_tc_end_at(&touch_state,slot,now);
    }
    touch_publish();pthread_mutex_unlock(&touch_lock);
}
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeTouchFrame(JNIEnv *env,jobject self,jfloatArray output) {
    (void)self;if((*env)->GetArrayLength(env,output)<128)return;
    pthread_mutex_lock(&touch_lock);touch_sync_game_state();double now=touch_now();dm_tc_tick(&touch_state,now);
    static uint64_t epoch,sample0;static double began,seen;
    uint64_t samples=dm_hid_samples();
    if(touch_state.pulse_button) {
        if(epoch!=touch_state.pulse_epoch){epoch=touch_state.pulse_epoch;sample0=samples;began=now;seen=0;}
        if(!seen && samples>sample0)seen=now;
        if((seen && samples-sample0>=3 && now-seen>=fmax(.12,touch_state.pulse_hold_s)) || now-began>2+touch_state.pulse_hold_s)dm_tc_pulse_end(&touch_state,epoch);
    }
    touch_publish();
    dm_tc_layout *l=&touch_state.layout;
    float a[128]={touch_scene.game,touch_state.navigation_mode,touch_state.skill_set,dm_tc_scale_for_size(l->width,l->height),
        touch_state.joy_slot>=0?touch_state.joy_origin_x:l->joy_x,touch_state.joy_slot>=0?touch_state.joy_origin_y:l->joy_y,
        l->joy_radius,touch_state.lx/32767.f,touch_state.ly/32767.f,touch_state.joy_slot>=0,l->swap.x,l->swap.y,l->swap.r,touch_state.enabled_targets};
    for(int i=0;i<12;i++){a[16+i*4]=l->target[i].x;a[17+i*4]=l->target[i].y;a[18+i*4]=l->target[i].r;a[19+i*4]=l->target[i].button && (touch_state.buttons&l->target[i].button)==l->target[i].button;}
    dm_tg_snapshot snapshot={0};dm_touch_game_snapshot(&snapshot);
    // DmTouchOverlay shown_glyph keeps a stale complete snapshot for painting;
    // it must not turn stale observations into new gameplay input.
    uint64_t ns=(uint64_t)(now*1e9);
    if(snapshot.captured_ns && ns>snapshot.captured_ns && ns-snapshot.captured_ns>DM_TG_MAX_AGE_NS)ns=snapshot.captured_ns;
    for(int i=0;i<5;i++) {
        dm_ta_glyph g=dm_ta_current_set(&snapshot,l->target[i].button,ns,touch_state.skill_set);
        int at=64+i*8;a[at]=g.status;a[at+1]=g.atlas;
        dm_ta_frame frame;
        int held=l->target[i].button && (touch_state.buttons&l->target[i].button)==l->target[i].button;
        if(g.status==DM_TA_GLYPH && dm_ta_frame_at(g.atlas,held?g.pressed_frame:g.normal_frame,&frame)) {
            a[at+2]=frame.x;a[at+3]=frame.y;a[at+4]=frame.width;a[at+5]=frame.height;
        }
        a[at+6]=g.raw_skill_id;a[at+7]=g.slot;
    }
    dm_hud_info hud;dm_hud_get(&hud);
    const float hv[]={hud.valid,hud.level,hud.exp_permille,hud.belt[0],hud.belt[1],hud.belt[2],hud.belt[3],hud.portal,hud.players,
        hud.hp,hud.hp_max,hud.mp,hud.mp_max,hud.area,hud.x,hud.y,hud.points,hud.holding,hud.quest_unread,hud.dead,touch_state.diff_open};
    memcpy(a+104,hv,sizeof hv);
    a[125]=dm_android_restart_cover();
    a[126]=dm_hud_bar_visible(&hud,touch_state.navigation_mode && touch_hud_blocked>=0,touch_hud_blocked>0);
    pthread_mutex_unlock(&touch_lock);(*env)->SetFloatArrayRegion(env,output,0,128,a);
}

/* Save failures originate on a guest stack. Publish only; Android presents
 * the upstream localized warning on its own UI thread at the next frame. */
static _Atomic int android_save_failure;
void front_save_failed(int err) {
    int empty=0;atomic_compare_exchange_strong(&android_save_failure,&empty,err?err:5);
}
JNIEXPORT jint JNICALL Java_local_d2rm_MainActivity_nativeTakeSaveFailure(JNIEnv *env,jobject self) {
    (void)env;(void)self;return atomic_exchange(&android_save_failure,0);
}
