/* Shared frontend logic runs in the game's HID sampling thread. */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "../../d2r-mobile/dm/host/dm_input.h"
#include "../../d2r-mobile/dm/host/dm_hud.h"
static _Atomic int g_fe_name_open,g_fe_name_tap,g_fe_name_has,g_fe_opt_open,g_fe_confirm_open,g_fe_back,g_fe_opt_side;
static _Atomic int g_fe_del_open,g_del_q,g_del_hold,g_cc_open;
static _Atomic int g_fe_ribbon,g_fe_list,g_fe_name_key,g_fe_skip_tut,g_fe_menu,g_fe_mode_modal,g_fe_options,g_fe_name,g_fe_name_done;
static _Atomic uint64_t g_fe_key_until,g_fe_key_mask,g_fe_name_ms;
static char g_fe_name_text[64],g_sel_name[17];static uint64_t g_sel_fr;
_Atomic int g_fe_name_no_accept;
static uint64_t dm_guest_frame(void){return atomic_load(&input_frame);}
static uint64_t ui_hash(const char *s){return mouse_hash(s);}
static module_t *main_module(void){for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main"))return &g_mod[i];return NULL;}
static void dm_cursor_set(int x,int y){module_t *m=main_module();if(m){*(int32_t *)(m->base+0x2d128f8)=x;*(int32_t *)(m->base+0x2d128fc)=y;}}
static const char *frontend_save_dir(void) {
    const char *override=getenv("DM_SAVE");if(override)return override;
    static char path[1024];snprintf(path,sizeof path,"%s/save",getenv("DM_ROOT")?getenv("DM_ROOT"):".");return path;
}
static void fe_send(const char *,const char *,const char *);
#include "../generated/frontend_flow.inc"
#include "../generated/ui_state_readout.inc"
/* Polled by the Android UI thread: never call Java on the guest stack. */
int dm_android_restart_cover(void) { int st=atomic_load(&g_rs);return st==RS_IDLE?0:st==RS_SHOW?2:1; }
static uint64_t frontend_buttons(void) {return dm_guest_frame()<atomic_load(&g_fe_key_until)?atomic_load(&g_fe_key_mask):0;}
/* Same mode-page tap interpretation as dm_ui_touch: accept only the tapped
 * Standard/Hardcore card, and let the upstream pump reconcile its checked bit. */
static int frontend_tap_prepare(module_t *m,uint64_t root,float nx,float ny,int x,int y) {
    if(atomic_load(&g_fe_opt_open) && !atomic_load(&g_fe_confirm_open)) {
        if((ny<899/2160.f || ny>1259/2160.f) && !(ny>1290/2160.f && ny<1470/2160.f)) {
            atomic_store(&g_fe_back,1);return 1;
        }
        atomic_store(&g_fe_opt_side,nx>=.5f);
    }
    if(atomic_load(&g_fe_name_open)) {
        uint64_t panel=ui_find(root,"CharacterCreateNamePanel",0);
        static const char *names[]={"InputBackplate","InputBackground","InputText"};
        uint64_t xy=(uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y<<32);
        for(unsigned i=0;panel && i<3;i++) {
            uint64_t w=ui_find(panel,names[i],0);
            if(w && (((int (*)(uint64_t,uint64_t))*(uint64_t *)(*(uint64_t *)w+0x48))(w,xy)&1)) {
                atomic_store(&g_fe_name_tap,1);return 1;
            }
        }
    }
    return 0;
}
