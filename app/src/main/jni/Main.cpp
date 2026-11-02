#include <list>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <initializer_list>
#include <set>
#include <unordered_set>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
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
// fallback kalau getExternalFilesDir gagal (folder ini TIDAK bisa dibaca tanpa root)
#define OUT_DIR_FALLBACK "/data/data/ocean.nomad.survival.simulator/files"
#define ONCE(msg) { static bool _o = false; if (!_o) { _o = true; LOGI(OBFUSCATE(msg)); } }

// ---------- toggle cheat (ECheatKey) ----------
std::atomic<unsigned int> cheatMask(0);
static bool hookedSet = false;

bool (*old_IsSet)(void *self, int key, void *mi) = nullptr;
bool hk_IsSet(void *self, int key, void *mi) {
    if (key >= 0 && key < 32 && (cheatMask.load() & (1u << key))) return true;
    return old_IsSet ? old_IsSet(self, key, mi) : false;
}

static void setKeys(std::initializer_list<int> keys, bool on) {
    if (on && !hookedSet) {
        if (!isLibraryLoaded(targetLibName)) {
            LOGI(OBFUSCATE("libil2cpp belum termuat, toggle diabaikan"));
            return;
        }
        HOOK(targetLibName, "0x265C5C4", hk_IsSet, old_IsSet);
        // hanya dianggap terpasang kalau Dobby benar-benar mengisi pointer aslinya
        hookedSet = (old_IsSet != nullptr);
        if (!hookedSet) {
            LOGI(OBFUSCATE("hook IsSet gagal (cek offset 0x265C5C4)"));
            return;
        }
    }
    for (int k : keys) {
        if (on) cheatMask |= (1u << k);
        else cheatMask &= ~(1u << k);
    }
}

// ---------- toast ----------
static void showToast(JNIEnv *env, jobject ctx, const char *text) {
    if (!env || !ctx) return;
    jstring js = env->NewStringUTF(text);
    jclass toast = env->FindClass(OBFUSCATE("android/widget/Toast"));
    if (!js || !toast) { env->ExceptionClear(); return; }
    jmethodID mk = env->GetStaticMethodID(toast, OBFUSCATE("makeText"), OBFUSCATE("(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;"));
    jmethodID show = env->GetMethodID(toast, OBFUSCATE("show"), OBFUSCATE("()V"));
    if (!mk || !show) { env->ExceptionClear(); return; }
    jobject t = env->CallStaticObjectMethod(toast, mk, ctx, js, 1);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (t) env->CallVoidMethod(t, show);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// ---------- mode debug ----------
static std::atomic<void *> g_pbm(nullptr);      // dari SelectedBackpack
static std::atomic<void *> g_pbmFree(nullptr);  // dari FreeCells
static std::atomic<void *> g_pbmAmt(nullptr);   // dari AmountThing
static std::atomic<void *> g_db(nullptr);
static std::atomic<int> pending(0);
static std::atomic<bool> g_tick(false);
static std::atomic<int> g_dumps(0);
static std::atomic<int> g_rows(0);
static std::set<std::string> seenUids;
static std::mutex gMtx;
static void *g_conts[64];
static int g_nc = 0, g_ring = 0;
static std::string g_outDir = OUT_DIR_FALLBACK;
static std::string g_lastFile;
static std::atomic<uint64_t> g_seenCache[4096];

static std::string outDir() {
    std::lock_guard<std::mutex> lk(gMtx);
    return g_outDir;
}

// Ambil folder yang bisa dibaca user: /storage/emulated/0/Android/data/<pkg>/files
// (tidak butuh izin storage dan tidak butuh root). Dipanggil dari thread UI (tombol menu).
static void resolveOutDir(JNIEnv *env, jobject ctx) {
    static bool done = false;
    if (done || !env || !ctx) return;
    jclass cc = env->GetObjectClass(ctx);
    jmethodID m = cc ? env->GetMethodID(cc, OBFUSCATE("getExternalFilesDir"), OBFUSCATE("(Ljava/lang/String;)Ljava/io/File;")) : nullptr;
    jobject file = m ? env->CallObjectMethod(ctx, m, (jstring) nullptr) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); file = nullptr; }
    if (file) {
        jclass fc = env->GetObjectClass(file);
        jmethodID gp = fc ? env->GetMethodID(fc, OBFUSCATE("getAbsolutePath"), OBFUSCATE("()Ljava/lang/String;")) : nullptr;
        jstring ps = gp ? (jstring) env->CallObjectMethod(file, gp) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); ps = nullptr; }
        if (ps) {
            const char *c = env->GetStringUTFChars(ps, nullptr);
            if (c) {
                std::lock_guard<std::mutex> lk(gMtx);
                g_outDir = c;
                done = true;
                env->ReleaseStringUTFChars(ps, c);
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Cek apakah memori [p, p+n) bisa dibaca TANPA crash (trik write() ke pipe: EFAULT kalau tidak valid).
// Hanya dipakai di jalur dump (jarang), bukan di hook yang sering dipanggil.
static bool canRead(const void *p, size_t n) {
    static int fds[2] = {-1, -1};
    if (!p || (uintptr_t) p < 0x10000) return false;
    if (fds[0] == -2) return true;  // pipe tidak tersedia, tidak bisa cek
    if (fds[0] == -1) {
        if (pipe(fds) != 0) { fds[0] = -2; return true; }
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        fcntl(fds[1], F_SETFL, O_NONBLOCK);
    }
    const char *c = (const char *) p;
    char sink[512];
    while (n > 0) {
        size_t chunk = n < sizeof sink ? n : sizeof sink;
        ssize_t w = write(fds[1], c, chunk);
        if (w <= 0) return false;
        ssize_t r = read(fds[0], sink, (size_t) w);  // kosongkan pipe
        (void) r;
        c += w;
        n -= (size_t) w;
    }
    return true;
}

template<typename T>
static bool rd(const void *base, size_t off, T &out) {
    const char *p = (const char *) base + off;
    if (!canRead(p, sizeof(T))) return false;
    memcpy(&out, p, sizeof(T));
    return true;
}

static void *fieldPtr(void *base, size_t off) {
    void *r = nullptr;
    if (base) rd(base, off, r);
    return r;
}

// Il2Cpp string: panjang di +0x10, karakter UTF-16 di +0x14
static std::string readStr(void *s) {  // cepat, dipakai di hook
    std::string r;
    if (!s) return r;
    int len = *(int *) ((char *) s + 0x10);
    if (len <= 0 || len > 128) return r;
    const uint16_t *c = (const uint16_t *) ((char *) s + 0x14);
    for (int i = 0; i < len; i++) r += (c[i] < 128) ? (char) c[i] : '?';
    return r;
}

static std::string readStrSafe(void *s) {  // aman, dipakai di dump
    std::string r;
    int len = 0;
    if (!s || !rd(s, 0x10, len) || len <= 0 || len > 256) return r;
    if (!canRead((char *) s + 0x14, (size_t) len * 2)) return r;
    const uint16_t *c = (const uint16_t *) ((char *) s + 0x14);
    for (int i = 0; i < len; i++) r += (c[i] < 128) ? (char) c[i] : '?';
    return r;
}

// Dipanggil dari hook Get/TryGet yang bisa dipanggil ribuan kali per frame.
// Dedupe berdasarkan ISI string (bukan alamat pointer, karena alamat bisa dipakai ulang oleh GC),
// pakai cache hash tanpa lock supaya tidak bikin lag.
static void noteUid(void *s) {
    if (!s) return;
    int len = *(int *) ((char *) s + 0x10);
    if (len <= 0 || len > 128) return;
    const uint16_t *c = (const uint16_t *) ((char *) s + 0x14);
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) { h ^= c[i]; h *= 1099511628211ULL; }
    h |= 1;
    std::atomic<uint64_t> &slot = g_seenCache[h & 4095];
    if (slot.load(std::memory_order_relaxed) == h) return;
    slot.store(h, std::memory_order_relaxed);
    std::string u = readStr(s);
    if (u.empty()) return;
    std::lock_guard<std::mutex> lk(gMtx);
    if (seenUids.size() < 50000) seenUids.insert(u);
}

static void noteContainer(void *c) {
    static std::atomic<void *> last(nullptr);
    if (!c || last.load(std::memory_order_relaxed) == c) return;
    last.store(c, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(gMtx);
    for (int i = 0; i < g_nc; i++) if (g_conts[i] == c) return;
    if (g_nc < 64) g_conts[g_nc++] = c;
    else { g_conts[g_ring] = c; g_ring = (g_ring + 1) % 64; }  // penuh: timpa yang paling lama
}

static void *containerStorage(void *sc) {
    return fieldPtr(sc, 0x10);
}

// Panggil method instance tanpa argumen lewat il2cpp (get_Cells, GetData, ...)
static void *invokeNoArg(FILE *f, void *obj, const char *name, const char *label) {
    auto getMethod = (void *(*)(void *, const char *, int)) getAbsoluteAddress(targetLibName, "0x22227e8");
    void *klass = nullptr;
    if (!obj || !rd(obj, 0, klass) || !klass) {
        fprintf(f, "# %s: objek %p tidak valid / kelas tidak terbaca\n", label, obj);
        fflush(f);
        return nullptr;
    }
    void *mi = getMethod(klass, name, 0);
    if (!mi) {
        fprintf(f, "# %s: method %s tidak ditemukan di kelas %p\n", label, name, klass);
        fflush(f);
        return nullptr;
    }
    void *fnp = nullptr;
    if (!rd(mi, 0, fnp) || !fnp) {
        fprintf(f, "# %s: methodPointer %s kosong\n", label, name);
        fflush(f);
        return nullptr;
    }
    fprintf(f, "# %s: memanggil %s (kelas %p)\n", label, name, klass);
    fflush(f);  // kalau game crash setelah baris ini, berarti crash ada di dalam pemanggilan itu
    auto fn = (void *(*)(void *, void *)) fnp;
    return fn(obj, mi);
}

static void writeItem(FILE *f, const char *prefix, char *item, int extra) {
    void *uidp = nullptr, *iconp = nullptr;
    int kind = -1;
    rd(item, 0x10, uidp);
    rd(item, 0x28, iconp);
    rd(item, 0x18, kind);
    std::string uid = readStrSafe(uidp);
    std::string icon = readStrSafe(iconp);
    fprintf(f, "%s%s|%s|%d|%d\n", prefix, uid.c_str(), icon.c_str(), kind, extra);
    g_rows++;
}

static void dumpStorage(FILE *f, void *storage, const char *label) {
    if (!storage) { fprintf(f, "# %s: kosong\n", label); fflush(f); return; }
    char *arr = (char *) invokeNoArg(f, storage, "get_Cells", label);
    if (!arr) { fprintf(f, "# %s: array kosong\n", label); fflush(f); return; }
    long n = 0;
    if (!rd(arr, 0x18, n) || n < 0 || n > 10000) {
        fprintf(f, "# %s: panjang array tidak masuk akal (%ld). Kalau get_Cells ternyata List<T>, bukan T[], layout offsetnya beda\n", label, n);
        fflush(f);
        return;
    }
    fprintf(f, "# %s: %ld slot\n", label, n);
    char prefix[32];
    snprintf(prefix, sizeof prefix, "%s|", label);
    for (long i = 0; i < n && i < 1000; i++) {
        char *cell = nullptr, *item = nullptr;
        int count = 0;
        if (!rd(arr, 0x20 + i * 8, cell) || !cell) continue;
        if (!rd(cell, 0x10, item) || !item) continue;
        rd(cell, 0x20, count);
        writeItem(f, prefix, item, count);
    }
    fflush(f);
}

// Buka file output; kalau folder utama gagal, coba folder fallback
static FILE *openOut(const char *name) {
    std::string dir = outDir();
    mkdir(dir.c_str(), 0777);
    std::string path = dir + "/" + name;
    FILE *f = fopen(path.c_str(), "w");
    if (!f && dir != OUT_DIR_FALLBACK) {
        LOGI(OBFUSCATE("gagal membuka %s (errno=%d), coba fallback"), path.c_str(), errno);
        mkdir(OUT_DIR_FALLBACK, 0777);
        path = std::string(OUT_DIR_FALLBACK) + "/" + name;
        f = fopen(path.c_str(), "w");
    }
    if (!f) {
        LOGI(OBFUSCATE("gagal membuka %s (errno=%d)"), path.c_str(), errno);
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(gMtx);
    g_lastFile = path;
    return f;
}

static void doDumpInventory() {
    FILE *f = openOut("inv_dump.txt");
    if (!f) return;
    g_rows = 0;
    fprintf(f, "# label|uid|icon|kind|jumlah\n");
    std::vector<void *> done;
    auto fresh = [&](void *s) -> bool {
        if (!s) return false;
        for (void *d : done) if (d == s) return false;
        done.push_back(s);
        return true;
    };
    void *p1 = g_pbm.load(), *p2 = g_pbmFree.load(), *p3 = g_pbmAmt.load();
    void *pbm = p1 ? p1 : (p2 ? p2 : p3);
    if (pbm) {
        void *k1 = fieldPtr(p1, 0), *k2 = fieldPtr(p2, 0), *k3 = fieldPtr(p3, 0);
        fprintf(f, "# pbm: SelectedBackpack=%p(kelas %p) FreeCells=%p(kelas %p) AmountThing=%p(kelas %p)\n", p1, k1, p2, k2, p3, k3);
        if ((p1 && p2 && k1 != k2) || (p1 && p3 && k1 != k3) || (p2 && p3 && k2 != k3))
            fprintf(f, "# PERINGATAN: hook-hook itu dipanggil dari kelas yang berbeda, offset 0x48/0x50/0x58 bisa salah objek\n");
        fflush(f);
        void *st[3] = {containerStorage(fieldPtr(pbm, 0x48)),
                       containerStorage(fieldPtr(pbm, 0x50)),
                       containerStorage(fieldPtr(pbm, 0x58))};
        const char *nm[3] = {"backpack", "belt", "equipment"};
        for (int i = 0; i < 3; i++) if (fresh(st[i])) dumpStorage(f, st[i], nm[i]);
        auto getTab = (void *(*)(void *, int, void *)) getAbsoluteAddress(targetLibName, "0x2648AD0");
        for (int i = 0; i < 3; i++) {
            char lb[16];
            snprintf(lb, sizeof lb, "tab%d", i);
            fprintf(f, "# memanggil GetTab(%d)\n", i);
            fflush(f);
            void *s = getTab(pbm, i, nullptr);
            if (fresh(s)) dumpStorage(f, s, lb);
        }
    } else {
        fprintf(f, "# PlayerBackpacksManager belum tertangkap (buka inventory / ganti slot di game dulu, lalu dump lagi)\n");
    }
    int n = 0;
    void *snap[64];
    {
        std::lock_guard<std::mutex> lk(gMtx);
        n = g_nc;
        memcpy(snap, g_conts, sizeof(void *) * n);
    }
    for (int i = 0; i < n; i++) {
        void *s = containerStorage(snap[i]);
        if (fresh(s)) {
            char lb[16];
            snprintf(lb, sizeof lb, "cont%d", i);
            dumpStorage(f, s, lb);
        }
    }
    fprintf(f, "# kontainer tertangkap: %d, baris: %d\n", n, g_rows.load());
    fclose(f);
    g_dumps++;
    LOGI(OBFUSCATE("inv_dump selesai, kontainer=%d baris=%d"), n, g_rows.load());
}

static void doDumpDb() {
    FILE *f = openOut("db_dump.txt");
    if (!f) return;
    g_rows = 0;
    fprintf(f, "# uid|icon|kind|stack\n");
    void *db = g_db.load();
    if (db) {
        char *arr = (char *) invokeNoArg(f, db, "GetData", "db");
        long n = 0;
        if (arr) {
            if (!rd(arr, 0x18, n) || n < 0 || n > 100000) {
                fprintf(f, "# db: panjang array tidak masuk akal (%ld). Kalau GetData ternyata List<T>/generic, layout beda\n", n);
                n = 0;
            }
            for (long i = 0; i < n && i < 5000; i++) {
                char *item = nullptr;
                int stack = 0;
                if (!rd(arr, 0x20 + i * 8, item) || !item) continue;
                rd(item, 0x3C, stack);
                writeItem(f, "", item, stack);
            }
        }
    } else fprintf(f, "# database belum tertangkap (buka craft/inventory di game dulu)\n");
    std::vector<std::string> uids;
    {
        std::lock_guard<std::mutex> lk(gMtx);
        uids.assign(seenUids.begin(), seenUids.end());
    }
    fprintf(f, "# --- uid yang pernah dicari game (%zu) ---\n", uids.size());
    for (auto &u : uids) fprintf(f, "%s\n", u.c_str());
    fclose(f);
    g_dumps++;
    LOGI(OBFUSCATE("db_dump selesai, baris=%d uid=%zu"), g_rows.load(), uids.size());
}

static void runPending() {
    if (!pending.load(std::memory_order_relaxed)) return;
    int p = pending.exchange(0);
    if (p & 1) doDumpInventory();
    if (p & 2) doDumpDb();
}

void (*old_Tick)(void *, void *) = nullptr;
void hk_Tick(void *self, void *mi) {
    ONCE("hook Tick aktif")
    g_tick = true;
    runPending();
    if (old_Tick) old_Tick(self, mi);
}
int (*old_Count)(void *, void *, void *) = nullptr;
int hk_Count(void *self, void *uid, void *mi) {
    ONCE("hook Count aktif")
    noteContainer(self);
    return old_Count ? old_Count(self, uid, mi) : 0;
}
bool (*old_Enough)(void *, void *, void *) = nullptr;
bool hk_Enough(void *self, void *recipe, void *mi) {
    ONCE("hook IsEnoughResources aktif")
    noteContainer(self);
    return old_Enough ? old_Enough(self, recipe, mi) : false;
}
void *(*old_SelBackpack)(void *, void *) = nullptr;
void *hk_SelBackpack(void *self, void *mi) {
    ONCE("hook SelectedBackpack aktif")
    if (self) g_pbm = self;
    return old_SelBackpack ? old_SelBackpack(self, mi) : nullptr;
}
int (*old_FreeCells)(void *, void *) = nullptr;
int hk_FreeCells(void *self, void *mi) {
    ONCE("hook FreeCells aktif")
    if (self) g_pbmFree = self;
    return old_FreeCells ? old_FreeCells(self, mi) : 0;
}
int (*old_AmountThing)(void *, void *, void *) = nullptr;
int hk_AmountThing(void *self, void *thing, void *mi) {
    ONCE("hook AmountThing aktif")
    if (self) g_pbmAmt = self;
    return old_AmountThing ? old_AmountThing(self, thing, mi) : 0;
}
void *(*old_Get)(void *, void *, void *) = nullptr;
void *hk_Get(void *db, void *t, void *mi) {
    ONCE("hook Get aktif")
    if (db) g_db = db;
    noteUid(t);
    return old_Get ? old_Get(db, t, mi) : nullptr;
}
bool (*old_TryGet)(void *, void *, void *, void *) = nullptr;
bool hk_TryGet(void *db, void *t, void *out, void *mi) {
    ONCE("hook TryGet aktif")
    if (db) g_db = db;
    noteUid(t);
    return old_TryGet ? old_TryGet(db, t, out, mi) : false;
}

static const int kHookTotal = 8;

static int hookCount() {
    return (old_Tick ? 1 : 0) + (old_Count ? 1 : 0) + (old_Enough ? 1 : 0) + (old_SelBackpack ? 1 : 0) +
           (old_FreeCells ? 1 : 0) + (old_AmountThing ? 1 : 0) + (old_Get ? 1 : 0) + (old_TryGet ? 1 : 0);
}

// Pasang hook hanya kalau belum terpasang; berhasil kalau Dobby mengisi pointer aslinya
#define TRY_HOOK(H_OFF, H_FN, H_ORIG) \
    do { \
        if (!(H_ORIG)) { \
            HOOK(targetLibName, H_OFF, H_FN, H_ORIG); \
            if (!(H_ORIG)) { failed++; LOGI(OBFUSCATE("hook gagal: " H_OFF)); } \
        } \
    } while (0)

// return: jumlah hook yang gagal, atau -1 kalau libil2cpp belum termuat
static int installDebug() {
    if (!isLibraryLoaded(targetLibName)) return -1;
    int failed = 0;
    TRY_HOOK("0x25788E8", hk_Tick, old_Tick);
    TRY_HOOK("0x27C1910", hk_Count, old_Count);
    TRY_HOOK("0x27C1D6C", hk_Enough, old_Enough);
    TRY_HOOK("0x2647140", hk_SelBackpack, old_SelBackpack);
    TRY_HOOK("0x2648D00", hk_FreeCells, old_FreeCells);
    TRY_HOOK("0x2647348", hk_AmountThing, old_AmountThing);
    TRY_HOOK("0x23DE354", hk_Get, old_Get);
    TRY_HOOK("0x23DE3FC", hk_TryGet, old_TryGet);
    LOGI(OBFUSCATE("hook debug: terpasang=%d gagal=%d"), hookCount(), failed);
    return failed;
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
            OBFUSCATE("Button_Dump database item"),
            OBFUSCATE("Button_Status (cek tangkapan)")
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
        case 9: {
            int failed = installDebug();
            char b[96];
            if (failed < 0) snprintf(b, sizeof b, "libil2cpp belum termuat, coba lagi sebentar");
            else if (failed == 0) snprintf(b, sizeof b, "Hook debug terpasang (%d/%d)", hookCount(), kHookTotal);
            else snprintf(b, sizeof b, "Hook gagal %d dari %d, cek offset (terpasang %d)", failed, kHookTotal, hookCount());
            showToast(env, obj, b);
            break;
        }
        case 10:
        case 11: {
            resolveOutDir(env, obj);
            pending |= (featNum == 10 ? 1 : 2);
            char b[512];
            snprintf(b, sizeof b, "%s diminta%s\nHasil: %s", featNum == 10 ? "Dump inventory" : "Dump database",
                     g_tick.load() ? "" : " (hook Tick belum aktif: tekan Siapkan dulu & pastikan sudah di dalam game)",
                     outDir().c_str());
            showToast(env, obj, b);
            LOGI(OBFUSCATE("tombol dump ditekan: %d"), (int) featNum);
            break;
        }
        case 12: {
            resolveOutDir(env, obj);
            char b[640];
            int nc;
            size_t nu;
            std::string last;
            {
                std::lock_guard<std::mutex> lk(gMtx);
                nc = g_nc;
                nu = seenUids.size();
                last = g_lastFile;
            }
            snprintf(b, sizeof b, "hook=%d/%d tick=%d pbm=%d db=%d kontainer=%d uid=%zu dump=%d baris=%d\nfile: %s",
                     hookCount(), kHookTotal, g_tick.load() ? 1 : 0,
                     (g_pbm.load() || g_pbmFree.load() || g_pbmAmt.load()) ? 1 : 0,
                     g_db.load() ? 1 : 0, nc, nu, g_dumps.load(), g_rows.load(),
                     last.empty() ? outDir().c_str() : last.c_str());
            showToast(env, obj, b);
            LOGI(OBFUSCATE("status: %s"), b);
            break;
        }
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
