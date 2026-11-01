#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <initializer_list>
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

#define targetLibName OBFUSCATE("libil2cpp.so")

std::atomic<unsigned int> cheatMask(0);
static bool hooked = false;

bool (*old_IsSet)(void *self, int key, void *mi);
bool hk_IsSet(void *self, int key, void *mi) {
    if (key >= 0 && key < 32 && (cheatMask.load() & (1u << key))) return true;
    return old_IsSet(self, key, mi);
}

static void setKeys(std::initializer_list<int> keys, bool on) {
    if (on && !hooked) {
        hooked = true;
        HOOK(targetLibName, "0x265C5C4", hk_IsSet, old_IsSet);
    }
    for (int k : keys) {
        if (on) cheatMask |= (1u << k);
        else cheatMask &= ~(1u << k);
    }
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;
    const char *features[] = {
            OBFUSCATE("Toggle_Craft semua gratis"),
            OBFUSCATE("Toggle_Unlock tanpa waktu"),
            OBFUSCATE("Toggle_Tanpa damage"),
            OBFUSCATE("Toggle_Tanpa lapar"),
            OBFUSCATE("Toggle_Tanpa haus"),
            OBFUSCATE("Toggle_Skill gratis"),
            OBFUSCATE("Toggle_Gerak cepat"),
            OBFUSCATE("Toggle_Memancing tidak gagal"),
            OBFUSCATE("Toggle_Berenang bebas")
    };
    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray) env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return (ret);
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0: setKeys({0}, boolean); break;
        case 1: setKeys({5, 6}, boolean); break;
        case 2: setKeys({7}, boolean); break;
        case 3: setKeys({8}, boolean); break;
        case 4: setKeys({9}, boolean); break;
        case 5: setKeys({10}, boolean); break;
        case 6: setKeys({12}, boolean); break;
        case 7: setKeys({3}, boolean); break;
        case 8: setKeys({11}, boolean); break;
        default: break;
    }
}

void hack_thread() {
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }
    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
