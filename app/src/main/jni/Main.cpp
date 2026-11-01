#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <string>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"
#include "dobby.h"

#define ENABLE_HOOKS 0

bool fCraft = false, fUnlock = false, fProtect = false, fSkill = false;

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Toggle_Free Craft"),
            OBFUSCATE("Toggle_Free Unlock"),
            OBFUSCATE("Toggle_Stat Protected"),
            OBFUSCATE("Toggle_Free Skill")
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

#define targetLibName OBFUSCATE("libil2cpp.so")

bool IsCraftFree(void*, void*, void*);
bool IsLongUnlockFree(void*, void*, void*);
bool IsCharProtected(void*, void*, void*);
bool IsSkillFree(void*, void*, void*);
extern bool (*old_IsCraftFree)(void*, void*, void*);
extern bool (*old_IsLongUnlockFree)(void*, void*, void*);
extern bool (*old_IsCharProtected)(void*, void*, void*);
extern bool (*old_IsSkillFree)(void*, void*, void*);
static bool h0 = false, h1 = false, h2 = false, h3 = false;

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0: fCraft = boolean; if (boolean && !h0) { h0 = true; HOOK(targetLibName, "0x265CEFC", IsCraftFree, old_IsCraftFree); } break;
        case 1: fUnlock = boolean; if (boolean && !h1) { h1 = true; HOOK(targetLibName, "0x265CF80", IsLongUnlockFree, old_IsLongUnlockFree); } break;
        case 2: fProtect = boolean; if (boolean && !h2) { h2 = true; HOOK(targetLibName, "0x2659004", IsCharProtected, old_IsCharProtected); } break;
        case 3: fSkill = boolean; if (boolean && !h3) { h3 = true; HOOK(targetLibName, "0x265D054", IsSkillFree, old_IsSkillFree); } break;
        default: break;
    }
}

bool (*old_IsCraftFree)(void*, void*, void*);
bool IsCraftFree(void *cm, void *uid, void *mi) {
    if (fCraft) return true;
    return old_IsCraftFree(cm, uid, mi);
}
bool (*old_IsLongUnlockFree)(void*, void*, void*);
bool IsLongUnlockFree(void *cm, void *uid, void *mi) {
    if (fUnlock) return true;
    return old_IsLongUnlockFree(cm, uid, mi);
}
bool (*old_IsCharProtected)(void*, void*, void*);
bool IsCharProtected(void *cm, void *ch, void *mi) {
    if (fProtect) return true;
    return old_IsCharProtected(cm, ch, mi);
}
bool (*old_IsSkillFree)(void*, void*, void*);
bool IsSkillFree(void *cm, void *sk, void *mi) {
    if (fSkill) return true;
    return old_IsSkillFree(cm, sk, mi);
}

void hack_thread() {
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }
#if defined(__aarch64__) && ENABLE_HOOKS
    HOOK(targetLibName, "0x265CEFC", IsCraftFree, old_IsCraftFree);
    HOOK(targetLibName, "0x265CF80", IsLongUnlockFree, old_IsLongUnlockFree);
    HOOK(targetLibName, "0x2659004", IsCharProtected, old_IsCharProtected);
    HOOK(targetLibName, "0x265D054", IsSkillFree, old_IsSkillFree);
#endif
    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
