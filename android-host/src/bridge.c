// JNI bridge for the Android game frontend and guest startup.
#include "dm_host.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include "display.h"
#include <sys/mman.h>
#include <unistd.h>
void dm_log_str(const char *s);
#include <string.h>
#include <android/log.h>
#include <pthread.h>

void *guest_sym(const char *name);
extern uint8_t *g_boot_stack;
extern size_t g_boot_stack_size;
uint32_t host_thread_main_init(void);
void run_guest_inits(void);
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
void dm_log_kv(const char *, uint64_t);
uint64_t dm_gate_stack(void);
static void boot_run_on_new_stack(void);
static JavaVM *ui_vm;
static jclass ui_class;
static pthread_mutex_t keyboard_lock=PTHREAD_MUTEX_INITIALIZER;
void dm_hid_buttons(uint64_t buttons,int reset);
static void dm_android_keys(uint64_t,int);
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeButtons(JNIEnv *env,jobject activity,jlong buttons,jboolean reset) {
    (void)env;(void)activity;dm_android_keys((uint64_t)buttons,reset);
}
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved) {
    (void)reserved;ui_vm=vm;return JNI_VERSION_1_6;
}
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeRegisterUi(JNIEnv *env,jobject activity) {
    if(!ui_class) { jclass cls=(*env)->GetObjectClass(env,activity);ui_class=(*env)->NewGlobalRef(env,cls);(*env)->DeleteLocalRef(env,cls); }
}
static jbyteArray utf8_bytes(JNIEnv *env,const char *text) {
    size_t n=text?strlen(text):0;jbyteArray bytes=(*env)->NewByteArray(env,(jsize)n);
    if(bytes && n) (*env)->SetByteArrayRegion(env,bytes,0,(jsize)n,(const jbyte *)text);
    return bytes;
}
typedef struct {
    const char *title,*sub,*guide,*initial;
    int max_chars;char *out;int out_cap,result;
} text_input_request;
// ART attaches and runs Java on a normal pthread stack. Guest threads switch
// SP to Horizon stacks which are neither the pthread stack registered with ART
// nor sized for Java execution; never attach them directly to the VM.
static void *text_input_worker(void *arg) {
    text_input_request *request=arg;JNIEnv *env=NULL;int attached=0;
    jint status=(*ui_vm)->GetEnv(ui_vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*ui_vm)->AttachCurrentThread(ui_vm,&env,NULL)!=JNI_OK)return NULL;
        attached=1;
    } else if(status!=JNI_OK)return NULL;
    if(!env)return NULL;
    LOG("Android keyboard: host worker attached");
    jbyteArray args[4]={NULL,NULL,NULL,NULL},answer=NULL;
    const char *strings[]={request->title,request->sub,request->guide,request->initial};
    for(int i=0;i<4;i++) {
        args[i]=utf8_bytes(env,strings[i]);
        if(!args[i] || (*env)->ExceptionCheck(env))goto cleanup;
    }
    jmethodID method=(*env)->GetStaticMethodID(env,ui_class,"showTextInput","([B[B[B[BI)[B");
    if(!method || (*env)->ExceptionCheck(env))goto cleanup;
    LOG("Android keyboard: requesting UI dialog");
    answer=(*env)->CallStaticObjectMethod(env,ui_class,method,args[0],args[1],args[2],args[3],request->max_chars);
    if((*env)->ExceptionCheck(env))goto cleanup;
    request->result=0;
    if(answer) {
        jsize size=(*env)->GetArrayLength(env,answer);
        if(size<request->out_cap) {
            (*env)->GetByteArrayRegion(env,answer,0,size,(jbyte *)request->out);
            if(!(*env)->ExceptionCheck(env)){request->out[size]=0;request->result=1;}
        }
    }
cleanup:
    if((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);request->result=-1;LOG("Android keyboard exception");
    }
    if(answer)(*env)->DeleteLocalRef(env,answer);
    for(int i=0;i<4;i++)if(args[i])(*env)->DeleteLocalRef(env,args[i]);
    if(attached)(*ui_vm)->DetachCurrentThread(ui_vm);
    LOG("Android keyboard: dialog completed result=%d",request->result);
    return NULL;
}
int front_text_input(const char *title,const char *sub,const char *guide,const char *initial,int max_chars,char *out,int out_cap) {
    if(!ui_vm || !ui_class || !out || out_cap<1)return -1;
    pthread_mutex_lock(&keyboard_lock);
    text_input_request request={title,sub,guide,initial,max_chars,out,out_cap,-1};
    pthread_t worker;
    LOG("Android keyboard: dispatching to host worker");
    int error=pthread_create(&worker,NULL,text_input_worker,&request);
    if(!error) {
        // Keep request strings and output storage alive until the UI completes.
        error=pthread_join(worker,NULL);
        if(error)die("Android keyboard worker join failed: %d",error);
    } else LOG("Android keyboard worker creation failed: %d",error);
    pthread_mutex_unlock(&keyboard_lock);return request.result;
}
static void boot_on_big_stack(void) {
    // 栈 32MB + 顶上 64MB 余量：Horizon 主线程栈上方有参数区，SDK 会往 SP 之上写（实测 +0x160 越顶页）
    enum { STK = 32u << 20, SLACK = 64u << 20 };
    uint8_t *stk = mmap(NULL, STK + SLACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stk == MAP_FAILED) die("主线程大栈 mmap");
    uint64_t sp = ((uint64_t)stk + STK) & ~0xFull;
    g_boot_stack = stk; g_boot_stack_size = STK;   // 回报栈区 = [stk, 初始SP)：SDK 会把 SP 设到「回报顶+0x260」，落进上方 RW 余量
    // 注：不再 MAP_FIXED 顶端补洞——曾在 ASLR 巧合下炸掉已装载的模块（真凶之一），64MB 余量已足
    {
        extern void svc_mem_preinit(void);
        svc_mem_preinit();   // Initialize guest address-space state before entering guest code.
    }
    dm_thread_enter((uint64_t)boot_run_on_new_stack, 0, sp);
    die("引导序列返回（不应发生）");
}

extern uint8_t *g_boot_stack;
extern size_t g_boot_stack_size;
static char *g_argv[8] = {"D2R"};
static int g_argc = 1;

static void boot_run_on_new_stack(void) {
    LOG("引导序列开始（新栈）");
    typedef void (*fn0)(void);
    typedef void (*fn2)(uint64_t, uint64_t);
    typedef void (*fnp)(void *);
    uint32_t hmain;
    void *p;
    void dm_log_kv(const char *tag, uint64_t v);
    void dm_log_str(const char *s);
    hmain = host_thread_main_init();                       // 上游顺序：先拿主线程句柄
    dm_log_kv("主线程句柄", hmain);
    { extern void dm_log_gate_done(void); dm_log_gate_done(); }
    p = guest_sym("_init_libc0"); dm_log_kv("查 _init_libc0", (uint64_t)p); if (p) { dm_log_kv("调 _init_libc0 入", (uint64_t)p); ((fn0)p)(); dm_log_kv("调 _init_libc0 出", 0); }
    p = guest_sym("nnosInitialize"); dm_log_kv("查 nnosInitialize", (uint64_t)p); if (p) { dm_log_kv("调 nnosInitialize 入", (uint64_t)p); ((fn2)p)(hmain, 0); dm_log_kv("调 nnosInitialize 出", 0); }
    p = guest_sym("_init_libc1"); dm_log_kv("查 _init_libc1", (uint64_t)p); if (p) { dm_log_kv("调 _init_libc1 入", (uint64_t)p); ((fn0)p)(); dm_log_kv("调 _init_libc1 出", 0); }
    p = guest_sym("nninitInitializeSdkModule"); dm_log_kv("查 nninitInitializeSdkModule", (uint64_t)p); if (p) { dm_log_kv("调 nninitInitializeSdkModule 入", (uint64_t)p); ((fn0)p)(); dm_log_kv("调 nninitInitializeSdkModule 出", 0); }
    if ((p = guest_sym("nninitStartup"))) { LOG(">>> nninitStartup"); ((fn0)p)(); }
    if ((p = guest_sym("_init_libc2"))) { LOG(">>> _init_libc2"); ((fn0)p)(); }
    dm_log_str(">>> init_array");
    run_guest_inits();
    dm_log_str("<<< init_array");
    p = guest_sym("_ZN2nn2os11SetHostArgcEi"); dm_log_kv("查 SetHostArgc", (uint64_t)p); if (p) { dm_log_str("调 SetHostArgc"); ((fnp)p)((void *)(uint64_t)g_argc); }
    p = guest_sym("_ZN2nn2os11SetHostArgvEPPc"); dm_log_kv("查 SetHostArgv", (uint64_t)p); if (p) { dm_log_str("调 SetHostArgv"); ((fnp)p)((void *)g_argv); }
    extern void dm_android_touch_layout_boot(void);
    dm_android_touch_layout_boot();
    extern void dm_touch_game_live_check(int);
    extern int dm_touch_game_attach(void);
    dm_touch_game_live_check(1);
    if(!dm_touch_game_attach())LOG("Touch game bridge could not attach; live skill state unavailable");
    extern int dm_android_hud_attach(void);
    if(!dm_android_hud_attach())LOG("HUD server tick observation unavailable");
    p = guest_sym("nnMain"); dm_log_kv("查 nnMain", (uint64_t)p); if (p) { dm_log_str(">>> nnMain 调用"); ((fn0)p)(); dm_log_str("<<< nnMain 返回"); }
    _exit(0);
}

void run_guest_inits(void);
uint32_t svc_thread_main_init(void);
uint32_t host_thread_main_init(void) { return svc_thread_main_init(); }


// Load the guest modules, then enter the upstream SDK/game startup sequence.
void dm_log_open(const char *path);
void dm_log_str(const char *s);
static int dmhost_load_and_run(const char *tables, const char *mainbin, const char *sdkbin) {
    {
        char lp[1024];
        snprintf(lp, sizeof lp, "%s/dmhost.log", tables);
        dm_log_open(lp);
    }
    LOG("dmhost 库模式：装载 %s + %s（表 %s）", mainbin, sdkbin, tables);
    setenv("DM_ROOT", tables, 1); // temp/save 使用应用私有目录，不能依赖进程 cwd
    char overlay[1200];snprintf(overlay,sizeof overlay,"%s/romfs_overlay",tables);
    setenv("DM_ROMFS_OVERLAY",overlay,1);
    LOG("资源根目录：%s", getenv("DM_ROMFS") ? getenv("DM_ROMFS") : "未配置");
    tsd_init();
    tsd_guest_thread_init();
    load_module(mainbin, tables, "main");
    load_module(sdkbin, tables, "sdk");
    resolve_and_relocate();
    install_key_pages();
    protect_modules();
    LOG("Guest modules loaded; booting on the dedicated 32 MB stack");
    boot_on_big_stack();
    return 0;
}

JNIEXPORT jint JNICALL
Java_local_d2rm_MainActivity_nativeRun(JNIEnv *env, jobject thiz, jstring jTables, jstring jMain, jstring jSdk, jstring jRomfs) {
    const char *tables = (*env)->GetStringUTFChars(env, jTables, NULL);
    const char *mainb = (*env)->GetStringUTFChars(env, jMain, NULL);
    const char *sdkb = (*env)->GetStringUTFChars(env, jSdk, NULL);
    const char *romfs = (*env)->GetStringUTFChars(env, jRomfs, NULL);
    setenv("DM_ROMFS", romfs, 1);
    (*env)->ReleaseStringUTFChars(env, jRomfs, romfs);
    int r = dmhost_load_and_run(tables, mainb, sdkb);
    (*env)->ReleaseStringUTFChars(env, jTables, tables);
    (*env)->ReleaseStringUTFChars(env, jMain, mainb);
    (*env)->ReleaseStringUTFChars(env, jSdk, sdkb);
    return r;
}

JNIEXPORT void JNICALL
Java_local_d2rm_MainActivity_nativeSetSurface(JNIEnv *env, jobject thiz, jobject surface) {
    (void)thiz;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    dm_window_set(window);
    if (window) ANativeWindow_release(window);
}

JNIEXPORT void JNICALL
Java_local_d2rm_MainActivity_nativeFocus(JNIEnv *env, jobject thiz, jint state) {
    (void)env; (void)thiz;
    extern void dm_focus_set(int);
    dm_focus_set(state);
    extern void dm_audio_suspend(int);
    dm_audio_suspend(state!=1);
}

void dm_hid_stick(int32_t x,int32_t y);
JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeStick(JNIEnv *env,jobject activity,jint x,jint y) {
    (void)env;(void)activity;dm_hid_stick(x,y);
}

#include "touch_android.inc"
