#include <list>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <initializer_list>
#include <set>
#include <unordered_set>
#include <string>
#include <sys/stat.h>
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
#define OUT_DIR "/data/data/ocean.nomad.survival.simulator/files"

// ---------- toggle cheat (ECheatKey) ----------
std::atomic<unsigned int> cheatMask(0);
static bool hookedSet = false;

bool (*old_IsSet)(void *self, int key, void *mi);
bool hk_IsSet(void *self, int key, void *mi) {
    if (key >= 0 && key < 32 && (cheatMask.load() & (1u << key))) return true;
    return old_IsSet(self, key, mi);
}

static void setKeys(std::initializer_list<int> keys, bool on) {
    if (on && !hookedSet) {
        hookedSet = true;
        HOOK(targetLibName, "0x265C5C4", hk_IsSet, old_IsSet);
    }
    for (int k : keys) {
        if (on) cheatMask |= (1u << k);
        else cheatMask &= ~(1u << k);
    }
}

// ---------- mode debug ----------
static void *g_pbm = nullptr, *g_db = nullptr;
static std::atomic<int> pending(0);
static std::set<std::string> seenUids;
static std::unordered_set<void *> seenPtr;

static std::string readStr(void *s) {
    std::string r;
    if (!s) return r;
    int len = *(int *) ((char *) s + 0x10);
    if (len <= 0 || len > 128) return r;
    const uint16_t *c = (const uint16_t *) ((char *) s + 0x14);
    for (int i = 0; i < len; i++) r += (c[i] < 128) ? (char) c[i] : '?';
    return r;
}

static void noteUid(void *s) {
    if (!s || seenPtr.size() > 20000 || seenPtr.count(s)) return;
    seenPtr.insert(s);
    std::string u = readStr(s);
    if (!u.empty()) seenUids.insert(u);
}

static void *containerStorage(void *sc) {
    return sc ? *(void **) ((char *) sc + 0x10) : nullptr;
}

static void writeItem(FILE *f, const char *prefix, char *item, int extra) {
    std::string uid = readStr(*(void **) (item + 0x10));
    std::string icon = readStr(*(void **) (item + 0x28));
    int kind = *(int *) (item + 0x18);
    fprintf(f, "%s%s|%s|%d|%d\n", prefix, uid.c_str(), icon.c_str(), kind, extra);
}

static void dumpStorage(FILE *f, void *storage, const char *label) {
    if (!storage) { fprintf(f, "# %s: kosong\n", label); fflush(f); return; }
    auto getClass = (void *(*)(void *)) getAbsoluteAddress(targetLibName, "0x2223054");
    auto getMethod = (void *(*)(void *, const char *, int)) getAbsoluteAddress(targetLibName, "0x22227e8");
    void *klass = getClass(storage);
    void *mi = klass ? getMethod(klass, "get_Cells", 0) : nullptr;
    if (!mi) { fprintf(f, "# %s: get_Cells tidak ditemukan\n", label); fflush(f); return; }
    auto fn = *(void *(**)(void *, void *)) mi;
    char *arr = (char *) fn(storage, mi);
    if (!arr) { fprintf(f, "# %s: array kosong\n", label); fflush(f); return; }
    long n = *(long *) (arr + 0x18);
    fprintf(f, "# %s: %ld slot\n", label, n);
    char prefix[32];
    snprintf(prefix, sizeof prefix, "%s|", label);
    for (long i = 0; i < n && i < 200; i++) {
        char *cell = *(char **) (arr + 0x20 + i * 8);
        if (!cell) continue;
        char *item = *(char **) (cell + 0x10);
        if (!item) continue;
        writeItem(f, prefix, item, *(int *) (cell + 0x20));
    }
    fflush(f);
}

static void doDumpInventory() {
    mkdir(OUT_DIR, 0777);
    FILE *f = fopen(OUT_DIR "/inv_dump.txt", "w");
    if (!f) { LOGI(OBFUSCATE("gagal membuka inv_dump.txt")); return; }
    fprintf(f, "# label|uid|icon|kind|jumlah\n");
    if (!g_pbm) {
        fprintf(f, "# PlayerBackpacksManager belum tertangkap\n");
        fclose(f);
        return;
    }
    dumpStorage(f, containerStorage(*(void **) ((char *) g_pbm + 0x48)), "backpack");
    dumpStorage(f, containerStorage(*(void **) ((char *) g_pbm + 0x50)), "belt");
    dumpStorage(f, containerStorage(*(void **) ((char *) g_pbm + 0x58)), "equipment");
    auto getTab = (void *(*)(void *, int, void *)) getAbsoluteAddress(targetLibName, "0x2648AD0");
    for (int i = 0; i < 3; i++) {
        char lb[16];
        snprintf(lb, sizeof lb, "tab%d", i);
        dumpStorage(f, getTab(g_pbm, i, nullptr), lb);
    }
    fclose(f);
    LOGI(OBFUSCATE("inv_dump selesai"));
}

static void doDumpDb() {
    mkdir(OUT_DIR, 0777);
    FILE *f = fopen(OUT_DIR "/db_dump.txt", "w");
    if (!f) { LOGI(OBFUSCATE("gagal membuka db_dump.txt")); return; }
    fprintf(f, "# uid|icon|kind|stack\n");
    if (g_db) {
        auto getClass = (void *(*)(void *)) getAbsoluteAddress(targetLibName, "0x2223054");
        auto getMethod = (void *(*)(void *, const char *, int)) getAbsoluteAddress(targetLibName, "0x22227e8");
        void *klass = getClass(g_db);
        void *mi = klass ? getMethod(klass, "GetData", 0) : nullptr;
        if (mi) {
            fflush(f);
            auto fn = *(void *(**)(void *, void *)) mi;
            char *arr = (char *) fn(g_db, mi);
            if (arr) {
                long n = *(long *) (arr + 0x18);
                for (long i = 0; i < n && i < 5000; i++) {
                    char *item = *(char **) (arr + 0x20 + i * 8);
                    if (item) writeItem(f, "", item, *(int *) (item + 0x3C));
                }
            }
        } else fprintf(f, "# GetData tidak ditemukan\n");
    } else fprintf(f, "# database belum tertangkap\n");
    fprintf(f, "# --- uid yang pernah dicari game (%zu) ---\n", seenUids.size());
    for (auto &u : seenUids) fprintf(f, "%s\n", u.c_str());
    fclose(f);
    LOGI(OBFUSCATE("db_dump selesai"));
}

static void runPending() {
    int p = pending.exchange(0);
    if (p & 1) doDumpInventory();
    if (p & 2) doDumpDb();
}

int (*old_FreeCells)(void *, void *);
int hk_FreeCells(void *self, void *mi) {
    if (self) g_pbm = self;
    runPending();
    return old_FreeCells(self, mi);
}
int (*old_AmountThing)(void *, void *, void *);
int hk_AmountThing(void *self, void *thing, void *mi) {
    if (self) g_pbm = self;
    runPending();
    return old_AmountThing(self, thing, mi);
}
void *(*old_Get)(void *, void *, void *);
void *hk_Get(void *db, void *t, void *mi) {
    if (db) g_db = db;
    noteUid(t);
    runPending();
    return old_Get(db, t, mi);
}
bool (*old_TryGet)(void *, void *, void *, void *);
bool hk_TryGet(void *db, void *t, void *out, void *mi) {
    if (db) g_db = db;
    noteUid(t);
    runPending();
    return old_TryGet(db, t, out, mi);
}

static bool dbgHooked = false;
static void installDebug() {
    if (dbgHooked) return;
    dbgHooked = true;
    HOOK(targetLibName, "0x2648D00", hk_FreeCells, old_FreeCells);
    HOOK(targetLibName, "0x2647348", hk_AmountThing, old_AmountThing);
    HOOK(targetLibName, "0x23DE354", hk_Get, old_Get);
    HOOK(targetLibName, "0x23DE3FC", hk_TryGet, old_TryGet);
}

// ---------- menu ----------
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
            OBFUSCATE("Toggle_Berenang bebas"),
            OBFUSCATE("Category_Debug"),
            OBFUSCATE("Button_Siapkan (pasang hook)"),
            OBFUSCATE("Button_Dump inventory"),
            OBFUSCATE("Button_Dump database item")
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
        case 9: installDebug(); break;
        case 10: pending |= 1; break;
        case 11: pending |= 2; break;
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
