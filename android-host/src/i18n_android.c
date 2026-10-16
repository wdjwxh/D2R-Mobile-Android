// Port of upstream dm_i18n.c / diag.c setlang_obs (944e199).
// Keep the same language policy and generated strings; replace CoreFoundation
// with the Activity locale and Android's system-property fallback.
#define _GNU_SOURCE
#include "i18n_android.h"
#include "vk_backend.h"
#include "../../d2r-mobile/dm/host/dm_i18n_tab.h"
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <unistd.h>

_Atomic int g_dm_lang = -1;
static _Atomic int android_language = -1;

int dm_lang_from_tag(const char *tag) {
    if (!tag || !*tag) return 8;
    char language[4] = {0};
    for (int i = 0; i < 3 && tag[i] && tag[i] != '-' && tag[i] != '_'; ++i)
        language[i] = (char)(tag[i] | 0x20);
    static const char *const chinese[] = {"zh", "yue", "wuu", "hak", "nan", "cmn", "gan", "hsn", "lzh"};
    for (unsigned i = 0; i < sizeof chinese / sizeof *chinese; ++i)
        if (!strcmp(language, chinese[i])) return 8;
    return 0;
}

int dm_sys_lang(void) {
    const char *override = getenv("DM_LANG");
    if (override && *override) return dm_lang_from_tag(override);
    int language = atomic_load(&android_language);
    if (language >= 0) return language;
    char tag[PROP_VALUE_MAX] = {0};
    if (!__system_property_get("persist.sys.locale", tag))
        __system_property_get("ro.product.locale", tag);
    return dm_lang_from_tag(tag);
}

uint64_t dm_android_desired_language(void) {
    const char *override = getenv("DM_LANG");
    const char *tag = override && *override ? override : dm_sys_lang() == 8 ? "zh-Hans" : "en-US";
    char code[8] = {0};
    strncpy(code, tag, sizeof code - 1);
    uint64_t result;
    memcpy(&result, code, sizeof result);
    return result;
}

static int settings_language(void) {
    const char *save = getenv("DM_SAVE"), *root = getenv("DM_ROOT");
    char path[1200];
    if (save && *save) snprintf(path, sizeof path, "%s/Settings/Settings.json", save);
    else if (root && *root) snprintf(path, sizeof path, "%s/save/Settings/Settings.json", root);
    else return -2; // UI may ask before the bridge has supplied its private root.
    FILE *file = fopen(path, "rb");
    if (!file) return dm_sys_lang();
    char buffer[8192];
    size_t count = fread(buffer, 1, sizeof buffer - 1, file);
    fclose(file);
    buffer[count] = 0;
    const char *key = strstr(buffer, "\"Language\"");
    // Upstream preserves the original default for an old settings file which
    // has no Language entry. Only an absent settings file follows the system.
    if (!key || !(key = strchr(key + 10, ':'))) return 8;
    char *end;
    long language = strtol(key + 1, &end, 10);
    return end != key + 1 && language >= 0 && language < 13 ? (int)language : 8;
}

int dm_lang_index(void) {
    int language = atomic_load(&g_dm_lang);
    if (language >= 0) return language;
    static _Atomic int from_file = -2;
    language = atomic_load(&from_file);
    if (language == -2) {
        language = settings_language();
        if (language != -2) atomic_store(&from_file, language);
    }
    return language < 0 ? dm_sys_lang() : language;
}

int dm_lang_zh(void) { int language = dm_lang_index(); return language == 8 || language == 9; }

const char *dm_ui(const char *key) {
    if (!key) return "";
    for (unsigned i = 0; i < sizeof k_dm_i18n / sizeof *k_dm_i18n; ++i)
        if (!strcmp(k_dm_i18n[i].k, key)) return dm_lang_zh() ? k_dm_i18n[i].zh : k_dm_i18n[i].en;
    return key;
}

const char *dm_str(uint8_t *main_base, const char *key) {
    if (!main_base) return dm_ui(key);
    struct { const char *key; uint64_t size; } string = {key, strlen(key)};
    const char *text = ((const char *(*)(void *))(main_base + 0x7f5630))(&string);
    return text && *text && strcmp(text, key) ? text : dm_ui(key);
}

void dm_android_i18n_observe(const uint64_t *regs) {
    // Upstream OBS 5 passes a pointer to the new language number in x1.
    // Like vm_read_overwrite on iOS, a bad guest address must not fault here.
    uint32_t language;
    struct iovec local = {&language, sizeof language};
    struct iovec remote = {(void *)(uintptr_t)regs[1], sizeof language};
    if (regs[1] && (dm_vk_guest_heap_read(regs[1], &language, sizeof language) ||
        process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof language) && language < 13)
        atomic_store(&g_dm_lang, (int)language);
}

JNIEXPORT void JNICALL Java_local_d2rm_MainActivity_nativeSetLanguageTag(JNIEnv *env, jclass cls, jstring tag) {
    (void)cls;
    if (!tag) return;
    const char *text = (*env)->GetStringUTFChars(env, tag, NULL);
    if (!text) return;
    atomic_store(&android_language, dm_lang_from_tag(text));
    (*env)->ReleaseStringUTFChars(env, tag, text);
}

JNIEXPORT jstring JNICALL Java_local_d2rm_MainActivity_nativeUiText(JNIEnv *env, jclass cls, jstring key) {
    (void)cls;
    if (!key) return NULL;
    const char *name = (*env)->GetStringUTFChars(env, key, NULL);
    if (!name) return NULL;
    const char *text = dm_ui(name);
    // The TSV is standard UTF-8, not JNI modified UTF-8 (some strings use emoji).
    jsize size = (jsize)strlen(text);
    jbyteArray bytes = (*env)->NewByteArray(env, size);
    if (bytes) (*env)->SetByteArrayRegion(env, bytes, 0, size, (const jbyte *)text);
    (*env)->ReleaseStringUTFChars(env, key, name);
    if (!bytes || (*env)->ExceptionCheck(env)) return NULL;
    jclass strings = (*env)->FindClass(env, "java/lang/String");
    if (!strings) return NULL;
    jmethodID constructor = (*env)->GetMethodID(env, strings, "<init>", "([BLjava/lang/String;)V");
    if (!constructor) return NULL;
    jstring charset = (*env)->NewStringUTF(env, "UTF-8");
    if (!charset) return NULL;
    return (jstring)(*env)->NewObject(env, strings, constructor, bytes, charset);
}
