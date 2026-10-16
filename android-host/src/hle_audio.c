// Adapted from upstream 7ff4e36 audio queues, lifecycle and 5.1 downmix. Android uses AAudio
// and POSIX semaphores; CoreAudio/dispatch code is replaced.
#include "dm_host.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>
#include <aaudio/AAudio.h>
typedef uint32_t Result;
#define RES_OK 0u
static uint64_t now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static struct { const void *key;void *value; } audio_objects[8];
static pthread_mutex_t audio_object_lock=PTHREAD_MUTEX_INITIALIZER;
static void *objmap_get(const void *key) {
    void *result=NULL;pthread_mutex_lock(&audio_object_lock);
    for(int i=0;i<8;i++) if(audio_objects[i].key==key) result=audio_objects[i].value;
    pthread_mutex_unlock(&audio_object_lock);return result;
}
static void objmap_put(const void *key,void *value) {
    pthread_mutex_lock(&audio_object_lock);
    for(int i=0;i<8;i++) if(!audio_objects[i].key) {audio_objects[i].key=key;audio_objects[i].value=value;pthread_mutex_unlock(&audio_object_lock);return;}
    die("Audio object registry full");
}
static void *objmap_del(const void *key) {
    void *result=NULL;pthread_mutex_lock(&audio_object_lock);
    for(int i=0;i<8;i++) if(audio_objects[i].key==key) {result=audio_objects[i].value;audio_objects[i].key=NULL;break;}
    pthread_mutex_unlock(&audio_object_lock);return result;
}
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 写进客体 AudioOutBuffer 的前 24 字节 | The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // 已释放最多 = 在途缓冲数（游戏收回缓冲才能再追加），与待播同上限 | Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客体 SystemEvent：有缓冲被释放就触发 | Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：游戏侧写，出声端读 | Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已释放：出声端写，游戏侧读 | Released: written by the output side, read by the game side
    uint64_t pos;                      // 当前块已播字节 | Bytes already played of the current block
    AAudioStream *au;
    int mix;
    int16_t *mixbuf; // Preallocated 5.1 staging; the output callback never allocates.
    pthread_t null_th; _Atomic int null_run;
    sem_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
} aout;

// 打开中的输出登记表：前端生命周期（切后台）一次性暂停 / 恢复
// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background)
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        sem_post(&a->notify_sem);
}

static void *event_notify(void *p) {
    aout *a = p;
    for (;;) {
        while(sem_wait(&a->notify_sem) && errno==EINTR) {}
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            sem_post(&a->notify_sem);
    }
    return NULL;
}

// 从待播队列取 n 字节交给出声端；整块播完就挪到已释放并通知普通线程。
// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut 已释放队列溢出");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    if (released) notify_release(a);
}

static _Atomic uint64_t g_render_calls;   // 声卡回调累计（生命周期验证：恢复后须继续增长） | Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
// 5.1 → 双声道（公开 的：原先把游戏的 6 声道原样交给 AudioUnit、不设声道布局，系统隐式转换只取前两路，中置——NPC 对白——整路丢掉）。
// 游戏的声道顺序（Mac App 走到阿卡拉跟前、对白开口那半秒按声道量能量实测：只有第 3 路跳起来）：左前、右前、中置、低音、左后、右后。
// 系数照 ITU-R BS.775 的 Lo/Ro：L = 左前 + 0.707·中置 + 0.707·左后，R 对称；低音不并入（标准下混不含 LFE，并进去会让低频发闷）；
// 整体再乘 0.707 留 3 dB 余量，仍超出 16 位的饱和截断。Q15 定点：0.707 = 23170，0.707 × 0.707 = 16384
// 5.1 -> stereo (previously: the game's 6 channels were handed to the AudioUnit as is without a channel layout; the system's implicit conversion took only the first two, and the centre - NPC dialogue - was dropped entirely).
// The game's channel order (measured in the Mac App by per-channel energy during the half second Akara starts speaking: only channel 3 jumps): front left, front right, centre, LFE, rear left, rear right.
// Coefficients from the ITU-R BS.775 Lo/Ro downmix: L = front left + 0.707*centre + 0.707*rear left, R symmetric; LFE is not mixed in (the standard downmix excludes LFE, and mixing it in muddies the low end);
// the whole is multiplied by 0.707 again for 3 dB of headroom, and anything still beyond 16 bits is saturated. Q15 fixed point: 0.707 = 23170, 0.707 x 0.707 = 16384
#define MIXMAX 4096
static void downmix51(const int16_t *in, int16_t *out, unsigned frames) {
    for (unsigned i = 0; i < frames; i++, in += 6, out += 2) {
        int32_t l = (23170 * in[0] + 16384 * in[2] + 16384 * in[4]) >> 15, r = (23170 * in[1] + 16384 * in[2] + 16384 * in[5]) >> 15;
        out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r); }
}
static aaudio_data_callback_result_t au_render(AAudioStream *stream,void *ctx,void *data,int32_t frames) {
    (void)stream;aout *a=ctx;atomic_fetch_add_explicit(&g_render_calls,1,memory_order_relaxed);
    if(!a->mix)pull(a,data,(uint64_t)frames*2*a->ch);
    else {
        int16_t *out=data;unsigned left=frames;
        while(left) {
            unsigned count=left<MIXMAX?left:MIXMAX;
            pull(a,(uint8_t *)a->mixbuf,(uint64_t)count*12);
            downmix51(a->mixbuf,out,count);out+=2*count;left-=count;
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static aaudio_result_t aa_state(AAudioStream *stream,aaudio_stream_state_t wanted) {
    uint64_t deadline=now_ns()+2000000000ull;
    while(now_ns()<deadline) {
        aaudio_stream_state_t state=AAudioStream_getState(stream),next;
        if(state==wanted) return AAUDIO_OK;
        if(state==AAUDIO_STREAM_STATE_DISCONNECTED || state==AAUDIO_STREAM_STATE_CLOSED) return AAUDIO_ERROR_DISCONNECTED;
        aaudio_result_t r=AAudioStream_waitForStateChange(stream,state,&next,100000000);
        if(r!=AAUDIO_OK && r!=AAUDIO_ERROR_TIMEOUT) return r;
    }
    return AAUDIO_ERROR_TIMEOUT;
}
static aaudio_result_t aa_start(AAudioStream *stream) {
    aaudio_result_t r=AAudioStream_requestStart(stream);return r?r:aa_state(stream,AAUDIO_STREAM_STATE_STARTED);
}
static void aa_stop(AAudioStream *stream) {
    aaudio_result_t r=AAudioStream_requestPause(stream);
    if(r || aa_state(stream,AAUDIO_STREAM_STATE_PAUSED)) die("AAudio pause failed; refusing concurrent consumers");
}
static int au_open(aout *a) {
    AAudioStreamBuilder *builder=NULL;
    a->mix=a->ch==6;
    if(a->mix && !a->mixbuf)a->mixbuf=malloc(MIXMAX*12);
    if(a->mix && !a->mixbuf) { LOG("AAudio downmix allocation failed; using silent sink");return -1; }
    int channels=a->mix?2:a->ch;
    if(AAudio_createStreamBuilder(&builder)) { LOG("AAudio builder failed; using silent sink");return -1; }
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder,RATE);
    AAudioStreamBuilder_setChannelCount(builder,channels);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder,au_render,a);
    aaudio_result_t r=AAudioStreamBuilder_openStream(builder,&a->au);
    AAudioStreamBuilder_delete(builder);
    if(r) { a->au=NULL;LOG("AAudio open failed: %s; using silent sink",AAudio_convertResultToText(r));return -1; }
    if(AAudioStream_getSampleRate(a->au)!=RATE || AAudioStream_getChannelCount(a->au)!=channels ||
       AAudioStream_getFormat(a->au)!=AAUDIO_FORMAT_PCM_I16) {
        AAudioStream_close(a->au);a->au=NULL;LOG("AAudio incompatible PCM format; using silent sink");return -1;
    }
    if(a->mix)LOG("Audio 5.1 -> stereo: centre and surrounds mixed explicitly; LFE excluded, -3 dB headroom");
    return 0;
}
// 静音接收端：每 5 ms 按 48 kHz 实时消耗
// Silent sink: consumes in real time at 48 kHz every 5 ms
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns(); if (t > now) usleep((useconds_t)((t - now) / 1000));
        pull(a, scratch, bytes);
    }
    return NULL;
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : 1; }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系统默认（48 kHz / 2 声道）
// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
static void h_au_InitParam(uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
static int h_au_List(char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
static Result h_au_Open(void *g, void *ev, const char *name, const uint8_t *param) {
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if(ch<1 || ch>8 || objmap_get(g)) die("Invalid/duplicate AudioOut");
    if (rate && rate != RATE) die("AudioOut 采样率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);
    if(!a) die("AudioOut allocation failed");
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    if(sem_init(&a->notify_sem,0,0)) die("Audio notify semaphore");
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // 缓冲释放事件：客体线程上绑内核事件（自动清除），之后由音频线程触发 | Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut 同时打开超过 %d 个", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    LOG("audio::OpenAudioOut %s 声道 %d %s", name ? name : "(默认)", ch, real_audio() ? "真出声" : "静音接收端");
    if (real_audio()) au_open(a);
    return RES_OK;
}
static Result h_au_Start(void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was == STATE_STARTED) return RES_OK;
    if(a->au && !g_suspended && aa_start(a->au)) {
        // Closing stops any callback before the silent sink takes ownership.
        LOG("AAudio start failed; using silent sink");AAudioStream_close(a->au);a->au=NULL;
    }
    if(!a->au || g_suspended) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); }
    return RES_OK;
}
static void h_au_Stop(void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was != STATE_STARTED) return;
    pthread_mutex_lock(&g_aos_lock); int susp = g_suspended; pthread_mutex_unlock(&g_aos_lock);
    if (a->au && !susp) aa_stop(a->au);
    else { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); }   // 静音接收端（含暂停期间接手的） | Silent sink (including one that took over during suspension)
}
static void h_au_Close(void *g) {
    h_au_Stop(g);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    sem_post(&a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { AAudioStream_close(a->au); }
    sem_destroy(&a->notify_sem);
    free(a->mixbuf);pthread_mutex_destroy(&a->m); free(a);
}
static int h_au_State(const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
static int h_au_Rate(const void *g) { (void)ao(g); return RATE; }
static int h_au_Ch(const void *g) { return ao(g)->ch; }
static int h_au_Fmt(const void *g) { (void)ao(g); return FMT_PCM16; }

static void h_au_SetBuf(gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
static void * h_au_BufPtr(const gbuf *b) { return b->ptr; }
static uint64_t h_au_BufSize(const gbuf *b) { return b->size; }
static unsigned h_au_Append(void *g, gbuf *b) {
    aout *a = ao(g);
    // 诊断开关 DM_AUDIO_DUMP=<文件>：游戏追加的每块 PCM 原样续写（16 位、48 kHz、声道数见 OpenAudioOut 日志，常见 6）。判「某个音播没播」用：
    // 拿原版音频对它做互相关（一档掉落音就这么验的）。只在设了变量时打开文件，没设时这里只多一次已缓存的判断
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_DUMP 打不开 %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    uint32_t rr=atomic_load_explicit(&a->rel_head,memory_order_acquire), rw=atomic_load_explicit(&a->rel_tail,memory_order_acquire);
    int ok = b && b->ptr && b->size && b->size<=b->cap && b->size%(2*a->ch)==0 && qt-qh+rw-rr < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：队列满时 false（真机同样有上限） | bool: false when the queue is full (the real device has a limit too)
}
static gbuf * h_au_Released(void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// 前端生命周期 / 音频会话打断：暂停真出声，期间由静音接收端接手按实时消耗（节奏不断）；恢复时切回 AudioUnit。
// 前端在 切后台 / 打断开始 时调 1，回前台 / 打断结束（shouldResume）时调 0。
// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) continue;
            // 暂停期间改由静音接收端按实时消耗：否则缓冲不再释放，FMOD 输出线程永久等待、游戏音频节奏卡死（来电打断同理）
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) { aa_stop(a->au); atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); }
            else { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL);
                   aaudio_result_t st = aa_start(a->au);
                   if (st) {   // 会话还被别人占着（通话没挂就切回游戏、打断结束时人还在别的 App）：不致命，继续由静音接收端顶着，等下一次恢复再试 | The session is still held by someone else (switched back to the game with a call not yet hung up, or the user is still in another app when the interruption ends): not fatal, the silent sink keeps covering, and we retry at the next resume
                       LOG("【红】AudioUnit 恢复失败（状态 %d）：保持静音接收端，等下次恢复重试", (int)st);
                       atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); failed = 1; } }
        }
        if (failed) g_suspended = 1;   // 没恢复成：状态留在「暂停」，下次调 0 会再试 | Resume did not succeed: the state stays "suspended", and the next call with 0 will retry
        LOG("audio %s", on ? "暂停（切后台 / 被打断）" : failed ? "恢复未成" : "恢复（回前台 / 打断结束）");
    }
    pthread_mutex_unlock(&g_aos_lock);
}

typedef struct { const char *name;void *fn; } hle_ent;
const hle_ent *audio_force_lookup(const char *name) {
    static const hle_ent entries[]={
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", (void *)h_au_InitParam},
        {"_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", (void *)h_au_List},
        {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", (void *)h_au_Open},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", (void *)h_au_Start},
        {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", (void *)h_au_Stop},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", (void *)h_au_Close},
        {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", (void *)h_au_State},
        {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", (void *)h_au_Rate},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", (void *)h_au_Ch},
        {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", (void *)h_au_Fmt},
        {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", (void *)h_au_SetBuf},
        {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", (void *)h_au_BufPtr},
        {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", (void *)h_au_BufSize},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", (void *)h_au_Append},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", (void *)h_au_Released},
    };
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++) if(!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
