#include <list>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <initializer_list>
#include <set>
#include <map>
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
// Folder output dump. HP root: /data/data/... paling gampang.
// Kalau mau folder yang bisa dibuka tanpa root, ubah USE_EXTERNAL_DIR jadi 1
// (hasil ke /storage/emulated/0/Android/data/<pkg>/files).
#define OUT_DIR_FALLBACK "/data/data/ocean.nomad.survival.simulator/files"
#define USE_EXTERNAL_DIR 0
#define ONCE(msg) { static bool _o = false; if (!_o) { _o = true; LOGI(OBFUSCATE(msg)); fileLog("%s", msg); } }

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
static std::map<std::string, uintptr_t> g_caller;  // uid -> alamat pemanggil pertama (kode game yang melakukan lookup)
static thread_local bool tlSkip = false;      // true saat kita sendiri yang memanggil TryGet (verifikasi), supaya tidak tercatat

// ---------- input runtime dari menu ----------
static std::mutex gAddMtx;
static std::string g_manualUid;               // diisi lewat InputText_ di menu
static int g_manualAmount = 1;                // diisi lewat InputValue_ di menu
static std::string g_addMsg = "-";

// ---------- input untuk fitur ubah jumlah ----------
static std::mutex gEditMtx;
static int g_editIndex = 0;                   // index global 1..N dari inv_items.txt
static int g_editAmount = 1;                  // jumlah baru

static std::string getManualUid() {
    std::lock_guard<std::mutex> lk(gAddMtx);
    return g_manualUid;
}
static int getManualAmount() {
    std::lock_guard<std::mutex> lk(gAddMtx);
    return g_manualAmount;
}
static void setManualUid(const char *s) {
    std::lock_guard<std::mutex> lk(gAddMtx);
    g_manualUid = s ? s : "";
}
static void setManualAmount(int n) {
    std::lock_guard<std::mutex> lk(gAddMtx);
    if (n < 1) n = 1;
    if (n > 999999) n = 999999;
    g_manualAmount = n;
}
static void setAddMsg(const char *m) {
    std::lock_guard<std::mutex> lk(gAddMtx);
    g_addMsg = m ? m : "-";
}

static int  getEditIndex()  { std::lock_guard<std::mutex> lk(gEditMtx); return g_editIndex; }
static int  getEditAmount() { std::lock_guard<std::mutex> lk(gEditMtx); return g_editAmount; }
static void setEditIndex(int n)  { std::lock_guard<std::mutex> lk(gEditMtx); g_editIndex = n < 0 ? 0 : n; }
static void setEditAmount(int n) { std::lock_guard<std::mutex> lk(gEditMtx); if (n < 0) n = 0; if (n > 999999) n = 999999; g_editAmount = n; }

static std::string outDir() {
    std::lock_guard<std::mutex> lk(gMtx);
    return g_outDir;
}

// Log ke file (mod_log.txt di folder output). Tiap baris langsung di-write + fsync, jadi kalau game
// mati mendadak, baris terakhir di file = langkah terakhir yang sempat dijalankan.
// Mode append: log beberapa sesi (termasuk yang crash) tetap tersimpan. Dipotong kalau > 1 MB.
static std::mutex gLogMtx;
static void fileLog(const char *fmt, ...) {
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char line[560];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [pid %d tid %d] %s\n", tmv.tm_hour, tmv.tm_min,
                     tmv.tm_sec, (int) (ts.tv_nsec / 1000000), (int) getpid(), (int) gettid(), msg);
    if (n <= 0) return;
    if (n > (int) sizeof line - 1) n = (int) sizeof line - 1;
    std::lock_guard<std::mutex> lk(gLogMtx);
    std::string dir = outDir();
    mkdir(dir.c_str(), 0777);
    std::string path = dir + "/mod_log.txt";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz > (1 << 20)) {
        if (ftruncate(fd, 0) != 0) { /* abaikan */ }
    }
    ssize_t w = write(fd, line, (size_t) n);
    (void) w;
    fsync(fd);
    close(fd);
}

// Ambil folder yang bisa dibaca user: /storage/emulated/0/Android/data/<pkg>/files
// (tidak butuh izin storage dan tidak butuh root). Dipanggil dari thread UI (tombol menu).
#if USE_EXTERNAL_DIR
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
#else
static void resolveOutDir(JNIEnv *, jobject) {}  // pakai OUT_DIR_FALLBACK (/data/data/...)
#endif

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
static void noteUid(void *s, uintptr_t caller = 0) {
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
    if (caller && !g_caller.count(u) && g_caller.size() < 50000) g_caller[u] = caller;
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

// Ambil array item dari objek database lewat NAMA FIELD (il2cpp_class_get_field_from_name + il2cpp_field_get_offset),
// karena method GetData tidak ditemukan di kelas runtime-nya.
static char *dbArray(FILE *f, void *db) {
    void *klass = nullptr;
    if (!rd(db, 0, klass) || !klass) { fprintf(f, "# db: kelas tidak terbaca\n"); fflush(f); return nullptr; }
    auto getField = (void *(*)(void *, const char *)) getAbsoluteAddress(targetLibName, "0x22227e0");
    auto fieldOff = (int (*)(void *)) getAbsoluteAddress(targetLibName, "0x2222e5c");
    const char *names[] = {"Data", "_data", "_items", "Items", "_database", "_list"};
    for (const char *nm : names) {
        void *fi = getField(klass, nm);
        if (!fi) continue;
        int off = fieldOff(fi);
        void *arr = fieldPtr(db, (size_t) off);
        fprintf(f, "# db: field %s offset 0x%x -> %p (kelas %p)\n", nm, off, arr, klass);
        fflush(f);
        if (arr) return (char *) arr;
    }
    fprintf(f, "# db: tidak ada field Data/_data/_items/Items/_database/_list di kelas %p\n", klass);
    fflush(f);
    return nullptr;
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
            fprintf(f, "# GetTab(%d) -> %p\n", i, s);
            fflush(f);
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

// ======================= SNAPSHOT TAS (dipakai fitur ubah jumlah) =======================

struct BagSlot {
    void *cell;        // Cell*
    void *item;        // Item*
    std::string uid;
    std::string icon;
    int kind;
    int count;         // nilai saat snapshot
};

// Ambil semua Cell dari satu storage. Return jumlah slot.
static int snapshotStorage(void *storage, std::vector<BagSlot> &out) {
    if (!storage) return 0;
    auto getMethod = (void *(*)(void *, const char *, int)) getAbsoluteAddress(targetLibName, "0x22227e8");
    void *klass = nullptr;
    if (!rd(storage, 0, klass) || !klass) return 0;
    void *mi = getMethod(klass, "get_Cells", 0);
    if (!mi) return 0;
    void *fnp = nullptr;
    if (!rd(mi, 0, fnp) || !fnp) return 0;
    auto fn = (void *(*)(void *, void *)) fnp;
    char *arr = (char *) fn(storage, mi);
    if (!arr) return 0;
    long n = 0;
    if (!rd(arr, 0x18, n) || n < 0 || n > 10000) return 0;

    for (long i = 0; i < n; i++) {
        char *cell = nullptr, *item = nullptr;
        if (!rd(arr, 0x20 + i * 8, cell) || !cell) continue;
        if (!rd(cell, 0x10, item) || !item) continue;
        BagSlot s;
        s.cell = cell;
        s.item = item;
        void *up = nullptr, *iconp = nullptr;
        rd(item, 0x10, up); rd(item, 0x28, iconp); rd(item, 0x18, s.kind);
        rd(cell, 0x20, s.count);
        s.uid  = readStrSafe(up);
        s.icon = readStrSafe(iconp);
        out.push_back(s);
    }
    return (int)out.size();
}

// Kumpulkan semua storage yang valid: backpack, belt, equipment, tab0-2, dan kontainer tertangkap.
static std::vector<std::pair<std::string, void *>> collectStorages() {
    std::vector<std::pair<std::string, void *>> result;
    std::set<void *> seen;

    void *pbm = g_pbm.load();
    if (!pbm) pbm = g_pbmFree.load();
    if (!pbm) pbm = g_pbmAmt.load();

    if (pbm) {
        const char *nm[3] = {"backpack", "belt", "equipment"};
        const size_t off[3] = {0x48, 0x50, 0x58};
        for (int i = 0; i < 3; i++) {
            void *wrapper = fieldPtr(pbm, off[i]);
            void *st = containerStorage(wrapper);
            if (st && seen.insert(st).second) result.push_back({nm[i], st});
        }
        auto getTab = (void *(*)(void *, int, void *)) getAbsoluteAddress(targetLibName, "0x2648AD0");
        for (int i = 0; i < 3; i++) {
            char lb[16];
            snprintf(lb, sizeof lb, "tab%d", i);
            void *st = getTab(pbm, i, nullptr);
            if (st && seen.insert(st).second) result.push_back({lb, st});
        }
    }

    void *snap[64];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(gMtx);
        n = g_nc;
        memcpy(snap, g_conts, sizeof(void *) * n);
    }
    for (int i = 0; i < n; i++) {
        void *st = containerStorage(snap[i]);
        if (st && seen.insert(st).second) {
            char lb[16];
            snprintf(lb, sizeof lb, "cont%d", i);
            result.push_back({lb, st});
        }
    }
    return result;
}

// Tulis daftar item di tas ke inv_items.txt (format: nomor|label|index_dalam_storage|uid|icon|kind|count)
static void doDumpBagItems() {
    FILE *f = openOut("inv_items.txt");
    if (!f) return;
    g_rows = 0;
    fprintf(f, "# Format: nomor|label|index_dalam_storage|uid|icon|kind|count\n");
    fprintf(f, "# Pakai kolom 'nomor' untuk mengisi field 'Index item' di menu.\n");

    auto storages = collectStorages();
    if (storages.empty()) {
        fprintf(f, "# tas belum tertangkap. Tekan Siapkan + buka inventory di game dulu.\n");
        fclose(f);
        fileLog("inv_items: tas belum tertangkap");
        return;
    }

    int globalIdx = 0;
    for (auto &kv : storages) {
        std::vector<BagSlot> slots;
        int n = snapshotStorage(kv.second, slots);
        fprintf(f, "# === %s (%d slot) ===\n", kv.first.c_str(), n);
        for (int i = 0; i < (int)slots.size(); i++) {
            globalIdx++;
            fprintf(f, "%d|%s|%d|%s|%s|%d|%d\n",
                    globalIdx, kv.first.c_str(), i,
                    slots[i].uid.c_str(), slots[i].icon.c_str(),
                    slots[i].kind, slots[i].count);
            g_rows++;
        }
    }
    fprintf(f, "# total item terdaftar: %d\n", globalIdx);
    fclose(f);
    fileLog("inv_items selesai, %d item", globalIdx);
    LOGI(OBFUSCATE("inv_items selesai, %d item"), globalIdx);
}

// ======================= HITUNG UID DI STORAGE (verifikasi put) =======================

// Kembalikan total count item ber-uid `uid` di dalam storage (via get_Cells).
// -1 kalau storage tidak valid / get_Cells tidak bisa dipanggil.
static int countUidInStorage(void *storage, const std::string &uid) {
    if (!storage || uid.empty()) return -1;
    auto getMethod = (void *(*)(void *, const char *, int)) getAbsoluteAddress(targetLibName, "0x22227e8");
    void *klass = nullptr;
    if (!rd(storage, 0, klass) || !klass) return -1;
    void *mi = getMethod(klass, "get_Cells", 0);
    if (!mi) return -1;
    void *fnp = nullptr;
    if (!rd(mi, 0, fnp) || !fnp) return -1;
    auto fn = (void *(*)(void *, void *)) fnp;
    char *arr = (char *) fn(storage, mi);
    if (!arr) return -1;
    long n = 0;
    if (!rd(arr, 0x18, n) || n < 0 || n > 10000) return -1;

    int total = 0;
    for (long i = 0; i < n; i++) {
        char *cell = nullptr, *item = nullptr;
        if (!rd(arr, 0x20 + i * 8, cell) || !cell) continue;
        if (!rd(cell, 0x10, item) || !item) continue;
        void *up = nullptr;
        rd(item, 0x10, up);
        std::string iu = readStrSafe(up);
        if (iu != uid) continue;
        int c = 0;
        rd(cell, 0x20, c);
        total += (c > 0 ? c : 1);
    }
    return total;
}

// ======================= PROBE UID (verifikasi sebelum add) =======================

enum class UidStatus {
    EMPTY,      // uid string kosong
    NO_DB,      // database belum tertangkap hook
    NOT_FOUND,  // TryGet return false -> uid tidak ada di database
    ITEM_NULL,  // TryGet true tapi out item null (bug internal)
    ALIAS,      // item ditemukan, tapi Item.Uid != uid input
    OK,         // item ditemukan & Item.Uid == uid input
    STR_FAIL    // il2cpp_string_new gagal
};

struct UidProbe {
    UidStatus status = UidStatus::EMPTY;
    std::string resolvedUid;    // Item.Uid asli (kalau ALIAS, ini yang benar)
    std::string icon;           // nama icon (untuk pesan)
    void *itemPtr = nullptr;    // pointer Item (informatif)
};

static UidProbe probeUid(const std::string &uid) {
    UidProbe p;
    if (uid.empty()) { p.status = UidStatus::EMPTY; return p; }
    void *db = g_db.load();
    if (!db) { p.status = UidStatus::NO_DB; return p; }

    auto newStr = (void *(*)(const char *)) getAbsoluteAddress(targetLibName, "0x22230c4");
    auto tryGet = (bool (*)(void *, void *, void *, void *)) getAbsoluteAddress(targetLibName, "0x23DE3FC");
    if (!newStr || !tryGet) { p.status = UidStatus::NO_DB; return p; }

    void *str = newStr(uid.c_str());
    if (!str) { p.status = UidStatus::STR_FAIL; return p; }

    void *item = nullptr;
    bool prevSkip = tlSkip;
    tlSkip = true;
    bool found = tryGet(db, str, &item, nullptr);
    tlSkip = prevSkip;

    if (!found) { p.status = UidStatus::NOT_FOUND; return p; }
    if (!item)  { p.status = UidStatus::ITEM_NULL; return p; }

    void *up = nullptr, *iconp = nullptr;
    rd(item, 0x10, up);
    rd(item, 0x28, iconp);
    p.itemPtr    = item;
    p.resolvedUid = readStrSafe(up);
    p.icon        = readStrSafe(iconp);
    p.status = (p.resolvedUid == uid) ? UidStatus::OK : UidStatus::ALIAS;
    return p;
}

// ======================= DUMP DATABASE + VERIFY =======================

static void doVerifyUids();

static void doDumpDb() {
    FILE *f = openOut("db_dump.txt");
    if (!f) return;
    g_rows = 0;
    fprintf(f, "# uid|icon|kind|stack\n");
    void *db = g_db.load();
    if (db) {
        char *arr = dbArray(f, db);
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
    doVerifyUids();
}

static void doVerifyUids() {
    FILE *f = openOut("db_verify.txt");
    if (!f) return;
    fprintf(f, "# uid|status|item|icon|bgIcon|kind|use|mail|destruct|stack|rare|dur|cost|group|special|skin|pemanggil|menu\n");
    void *db = g_db.load();
    if (!db) {
        fprintf(f, "# database belum tertangkap (buka craft/inventory di game dulu)\n");
        fclose(f);
        return;
    }
    auto newStr = (void *(*)(const char *)) getAbsoluteAddress(targetLibName, "0x22230c4");
    auto tryGet = (bool (*)(void *, void *, void *, void *)) getAbsoluteAddress(targetLibName, "0x23DE3FC");
    uintptr_t base = (uintptr_t) getAbsoluteAddress(targetLibName, "0x0");
    std::set<std::string> all, menu;
    std::map<std::string, uintptr_t> callers;
    {
        std::lock_guard<std::mutex> lk(gMtx);
        all = seenUids;
        callers = g_caller;
    }
    std::string mu = getManualUid();
    if (!mu.empty()) {
        menu.insert(mu);
        all.insert(mu);
    }
    int nOk = 0, nBeda = 0, nNo = 0, nRaw = 0;
    std::vector<std::string> menuBad, raws;
    tlSkip = true;
    for (const std::string &u : all) {
        void *str = newStr(u.c_str());
        void *item = nullptr;
        bool ok = str ? tryGet(db, str, &item, nullptr) : false;
        std::string status;
        if (!ok) status = "TIDAK_ADA";
        else if (!item) status = "ITEM_NULL";
        else {
            void *up = nullptr;
            rd(item, 0x10, up);
            std::string iu = readStrSafe(up);
            status = (iu == u) ? "OK" : "UID_BEDA:" + iu;
        }
        bool good = (status == "OK");
        if (good) nOk++; else if (status == "TIDAK_ADA") nNo++; else nBeda++;
        bool inMenu = menu.count(u) > 0;
        if (inMenu && !good) menuBad.push_back(u + "=" + status);

        void *iconp = nullptr, *bgp = nullptr;
        int kind = -1, use = -1, mail = -1, stack = -1, rare = -1, dur = -1, group = -1, skin = -1;
        float cost = 0;
        unsigned char destr = 0, special = 0;
        if (item) {
            rd(item, 0x28, iconp); rd(item, 0x30, bgp);
            rd(item, 0x18, kind); rd(item, 0x1C, use); rd(item, 0x20, mail);
            rd(item, 0x38, destr); rd(item, 0x3C, stack); rd(item, 0x40, rare); rd(item, 0x44, dur);
            rd(item, 0x48, cost); rd(item, 0x4C, special); rd(item, 0x50, group); rd(item, 0x54, skin);
        }
        char cal[32] = "-";
        auto it = callers.find(u);
        if (it != callers.end() && it->second > base) snprintf(cal, sizeof cal, "caller=0x%lX", (unsigned long) (it->second - base));
        fprintf(f, "%s|%s|%p|%s|%s|%d|%d|%d|%d|%d|%d|%d|%.2f|%d|%d|%d|%s|%s\n",
                u.c_str(), status.c_str(), item, readStrSafe(iconp).c_str(), readStrSafe(bgp).c_str(),
                kind, use, mail, (int) destr, stack, rare, dur, cost, group, (int) special, skin, cal, inMenu ? "M" : "-");
        g_rows++;
        if (item && (!good || nRaw < 8)) {
            nRaw++;
            std::string r = "# raw " + u + " item=";
            char t[64];
            snprintf(t, sizeof t, "%p:", item);
            r += t;
            for (size_t off = 0x10; off < 0x58; off += 8) {
                unsigned long long w = 0;
                rd(item, off, w);
                snprintf(t, sizeof t, " +0x%zx=0x%llx", off, w);
                r += t;
                std::string sv = readStrSafe((void *) w);
                if (!sv.empty()) r += "(\"" + sv + "\")";
            }
            raws.push_back(r);
        }
    }
    tlSkip = false;
    for (auto &r : raws) fprintf(f, "%s\n", r.c_str());
    fprintf(f, "# ringkasan: OK=%d UID_BEDA/ITEM_NULL=%d TIDAK_ADA=%d dari %zu uid\n", nOk, nBeda, nNo, all.size());
    fprintf(f, "# uid menu Tambah item yang TIDAK OK (%zu):", menuBad.size());
    for (auto &b : menuBad) fprintf(f, " %s", b.c_str());
    fprintf(f, "\n");
    fclose(f);
    LOGI(OBFUSCATE("db_verify selesai: OK=%d beda=%d tidak ada=%d"), nOk, nBeda, nNo);
    fileLog("db_verify selesai: OK=%d beda=%d tidak_ada=%d menu_bermasalah=%zu", nOk, nBeda, nNo, menuBad.size());
}

// ======================= ADD ITEM (UID dari menu) =======================

static void doAddItem() {
    std::string uid = getManualUid();
    int cnt = getManualAmount();

    if (uid.empty()) {
        setAddMsg("GAGAL: UID kosong");
        fileLog("add: uid kosong");
        return;
    }

    // --- Tahap 1: verifikasi UID ---
    UidProbe p = probeUid(uid);
    switch (p.status) {
        case UidStatus::NO_DB:
            setAddMsg("GAGAL: database belum tertangkap. Buka inventory/craft dulu.");
            fileLog("add: db belum tertangkap");
            return;
        case UidStatus::NOT_FOUND:
            setAddMsg(("GAGAL: UID tidak ada di database: " + uid).c_str());
            fileLog("add: uid TIDAK_ADA = %s", uid.c_str());
            return;
        case UidStatus::ITEM_NULL:
            setAddMsg("GAGAL: TryGet true tapi item null (bug internal)");
            fileLog("add: uid=%s TryGet true item null", uid.c_str());
            return;
        case UidStatus::ALIAS: {
            fileLog("add: uid '%s' adalah alias, resolved -> '%s'", uid.c_str(), p.resolvedUid.c_str());
            setAddMsg(("UID alias, memakai yang asli: " + p.resolvedUid).c_str());
            uid = p.resolvedUid;
            break;
        }
        case UidStatus::STR_FAIL:
            setAddMsg("GAGAL: il2cpp_string_new gagal");
            return;
        case UidStatus::OK:
            break;
        default:
            return;
    }

    // --- Tahap 2: tas tertangkap? ---
    void *pbm = g_pbm.load();
    if (!pbm) pbm = g_pbmFree.load();
    if (!pbm) pbm = g_pbmAmt.load();
    if (!pbm) {
        setAddMsg("GAGAL: tas belum tertangkap. Buka inventory dulu.");
        fileLog("add: pbm belum tertangkap");
        return;
    }

    auto newStr  = (void *(*)(const char *)) getAbsoluteAddress(targetLibName, "0x22230c4");
    auto getLarge= (void *(*)(void *, void *)) getAbsoluteAddress(targetLibName, "0x2646A0C");
    auto put     = (bool (*)(void *, void *, int, int, void *)) getAbsoluteAddress(targetLibName, "0x27C0354");
    if (!newStr || !getLarge || !put) {
        setAddMsg("GAGAL: fungsi il2cpp tidak tersedia");
        return;
    }

    void *str = newStr(uid.c_str());
    if (!str) { setAddMsg("GAGAL: string uid gagal"); return; }

    void *cont = getLarge(pbm, nullptr);
    void *bpFallback = fieldPtr(pbm, 0x48);

    // --- Tahap 3: catat kondisi tas SEBELUM put ---
    int beforeLarge = countUidInStorage(cont, uid);
    int beforeBp    = countUidInStorage(bpFallback, uid);
    fileLog("add: SEBELUM large=%d bp=%d (uid=%s, cnt=%d)", beforeLarge, beforeBp, uid.c_str(), cnt);

    // --- Tahap 4: put ---
    bool okLarge = false, okBp = false;
    if (cont) okLarge = put(cont, str, cnt, 0, nullptr);
    fileLog("add: Put(large) -> %d", okLarge ? 1 : 0);
    if (!okLarge && bpFallback) {
        okBp = put(bpFallback, str, cnt, 0, nullptr);
        fileLog("add: Put(backpack) -> %d", okBp ? 1 : 0);
    }

    // --- Tahap 5: catat kondisi tas SESUDAH put ---
    int afterLarge = countUidInStorage(cont, uid);
    int afterBp    = countUidInStorage(bpFallback, uid);
    fileLog("add: SESUDAH large=%d bp=%d", afterLarge, afterBp);

    // --- Tahap 6: diagnosa hasil ---
    bool masukLarge = (afterLarge > beforeLarge);
    bool masukBp    = (afterBp > beforeBp);
    bool putClaimed = (okLarge || okBp);

    char msg[256];
    if (masukLarge || masukBp) {
        snprintf(msg, sizeof msg, "OK %d x %s (large +%d, bp +%d)",
                 cnt, uid.c_str(),
                 afterLarge - beforeLarge, afterBp - beforeBp);
    } else if (putClaimed) {
        snprintf(msg, sizeof msg,
                 "GAGAL-UI: put()=true tapi item tidak masuk. Kemungkinan UI belum refresh atau salah container.");
    } else {
        snprintf(msg, sizeof msg,
                 "GAGAL-PUT: tas menolak (penuh? uid valid tapi signature put salah?).");
    }
    setAddMsg(msg);
    fileLog("add: HASIL %s", msg);
}

// ======================= UBAH JUMLAH ITEM DI TAS =======================

// Ubah count Cell di index global tertentu.
// Return: 1 sukses, 0 index tidak valid, -1 tas tidak ada, -2 tulis gagal
static int changeCellCount(int globalIndex, int newAmount) {
    if (globalIndex < 1) return 0;
    auto storages = collectStorages();
    if (storages.empty()) return -1;

    int counter = 0;
    for (auto &kv : storages) {
        std::vector<BagSlot> slots;
        snapshotStorage(kv.second, slots);
        for (auto &s : slots) {
            counter++;
            if (counter != globalIndex) continue;

            int before = 0;
            rd(s.cell, 0x20, before);

            // Tulis langsung ke Cell+0x20
            int *target = (int *) ((char *) s.cell + 0x20);
            *target = newAmount;

            int after = 0;
            rd(s.cell, 0x20, after);

            fileLog("edit: %s uid=%s before=%d want=%d after=%d",
                    kv.first.c_str(), s.uid.c_str(), before, newAmount, after);
            return (after == newAmount) ? 1 : -2;
        }
    }
    return 0;
}

static void doEditAmount() {
    int idx = getEditIndex();
    int amt = getEditAmount();
    if (idx < 1) {
        setAddMsg("GAGAL-EDIT: index item belum diisi (lihat inv_items.txt)");
        fileLog("edit: index invalid");
        return;
    }
    int r = changeCellCount(idx, amt);
    switch (r) {
        case  1: setAddMsg(("OK-EDIT: item #" + std::to_string(idx) + " -> " + std::to_string(amt)).c_str()); break;
        case  0: setAddMsg("GAGAL-EDIT: index di luar jangkauan. Refresh daftar item dulu."); break;
        case -1: setAddMsg("GAGAL-EDIT: tas belum tertangkap. Buka inventory dulu."); break;
        case -2: setAddMsg("GAGAL-EDIT: tulis cell+0x20 tidak berubah. Offset count mungkin salah (lihat mod_log.txt)."); break;
    }
    fileLog("edit: hasil=%d idx=%d amt=%d", r, idx, amt);
}

// ======================= PENDING DISPATCH =======================

static void runPending() {
    int p = pending.load(std::memory_order_relaxed);
    if (!p) return;

    // Bit 4: add item
    if (p & 4) {
        pending.fetch_and(~4);
        fileLog("runPending: proses tambah UID");
        doAddItem();
    }
    // Bit 8: ubah jumlah
    if (p & 8) {
        pending.fetch_and(~8);
        fileLog("runPending: proses ubah jumlah");
        doEditAmount();
    }
    // Bit 16: refresh daftar item
    if (p & 16) {
        pending.fetch_and(~16);
        fileLog("runPending: refresh daftar item");
        doDumpBagItems();
        UidProbe pr = probeUid(getManualUid());
        if (pr.status == UidStatus::NOT_FOUND)
            fileLog("refresh: uid menu '%s' TIDAK ada di db", getManualUid().c_str());
    }

    p = pending.load(std::memory_order_relaxed);
    if (!(p & 3)) return;

    if ((!g_pbm.load() && !g_pbmFree.load() && !g_pbmAmt.load()) || !g_db.load()) {
        static std::atomic<bool> waitingLogged(false);
        if (!waitingLogged.exchange(true))
            fileLog("dump gabungan menunggu capture: pbm=%d db=%d",
                    (g_pbm.load() || g_pbmFree.load() || g_pbmAmt.load()) ? 1 : 0,
                    g_db.load() ? 1 : 0);
        return;
    }

    int dumpFlags = pending.fetch_and(~3) & 3;
    if (!dumpFlags) return;
    fileLog("runPending dumpFlags=%d", dumpFlags);
    if (dumpFlags & 1) doDumpInventory();
    if (dumpFlags & 2) doDumpDb();
    fileLog("runPending selesai");
}

// ======================= HOOKS =======================

void (*old_Tick)(void *, void *) = nullptr;
void hk_Tick(void *self, void *mi) {
    ONCE("hook Tick aktif")
    g_tick = true;
    {
        static time_t lastBeat = 0;
        static int beats = 0;
        time_t now = time(nullptr);
        if (now - lastBeat >= 2) {
            lastBeat = now;
            fileLog("tick hidup #%d", ++beats);
        }
    }
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
    if (!tlSkip) noteUid(t, (uintptr_t) __builtin_return_address(0));
    return old_Get ? old_Get(db, t, mi) : nullptr;
}
bool (*old_TryGet)(void *, void *, void *, void *) = nullptr;
bool hk_TryGet(void *db, void *t, void *out, void *mi) {
    ONCE("hook TryGet aktif")
    if (db) g_db = db;
    if (!tlSkip) noteUid(t, (uintptr_t) __builtin_return_address(0));
    return old_TryGet ? old_TryGet(db, t, out, mi) : false;
}

static const int kHookTotal = 8;

static int hookCount() {
    return (old_Tick ? 1 : 0) + (old_Count ? 1 : 0) + (old_Enough ? 1 : 0) + (old_SelBackpack ? 1 : 0) +
           (old_FreeCells ? 1 : 0) + (old_AmountThing ? 1 : 0) + (old_Get ? 1 : 0) + (old_TryGet ? 1 : 0);
}

#define LOG_BYTES(H_OFF) \
    do { \
        const uint32_t *bp_ = (const uint32_t *) getAbsoluteAddress(targetLibName, H_OFF); \
        int ret_ = -1; \
        for (int i_ = 0; i_ < 6; i_++) if (bp_[i_] == 0xd65f03c0u) { ret_ = i_; break; } \
        LOGI(OBFUSCATE("bytes " H_OFF " @%p: %08x %08x %08x %08x %08x %08x ret@%d"), \
             (const void *) bp_, bp_[0], bp_[1], bp_[2], bp_[3], bp_[4], bp_[5], ret_); \
        fileLog("bytes " H_OFF " @%p: %08x %08x %08x %08x %08x %08x ret@%d", \
                (const void *) bp_, bp_[0], bp_[1], bp_[2], bp_[3], bp_[4], bp_[5], ret_); \
    } while (0)

#define TRY_HOOK(H_OFF, H_FN, H_ORIG) \
    do { \
        if (!(H_ORIG)) { \
            LOG_BYTES(H_OFF); \
            LOGI(OBFUSCATE("memasang hook " H_OFF " (" #H_FN ")")); \
            fileLog("MEMASANG hook " H_OFF " (" #H_FN ")  <- kalau game mati setelah baris ini, hook inilah penyebabnya"); \
            HOOK(targetLibName, H_OFF, H_FN, H_ORIG); \
            LOGI(OBFUSCATE("hook selesai " H_OFF)); \
            fileLog("hook selesai " H_OFF " orig=%p", (void *) (H_ORIG)); \
            if (!(H_ORIG)) { failed++; LOGI(OBFUSCATE("hook gagal: " H_OFF)); fileLog("hook GAGAL " H_OFF); } \
        } \
    } while (0)

static int installAll(const char **summary) {
    if (!isLibraryLoaded(targetLibName)) {
        *summary = "libil2cpp belum termuat";
        return -1;
    }

    int failed = 0;
    TRY_HOOK("0x2576748", hk_Tick, old_Tick);
    TRY_HOOK("0x27C1910", hk_Count, old_Count);
    TRY_HOOK("0x27C1D6C", hk_Enough, old_Enough);
    TRY_HOOK("0x2647140", hk_SelBackpack, old_SelBackpack);
    TRY_HOOK("0x2648D00", hk_FreeCells, old_FreeCells);
    TRY_HOOK("0x2647348", hk_AmountThing, old_AmountThing);
    TRY_HOOK("0x23DE354", hk_Get, old_Get);
    TRY_HOOK("0x23DE3FC", hk_TryGet, old_TryGet);

    const int installed = hookCount();
    static char result[96];
    snprintf(result, sizeof result, "hook terpasang %d/%d, gagal=%d",
             installed, kHookTotal, failed);
    *summary = result;

    fileLog("installAll selesai: hook=%d/%d gagal=%d", installed, kHookTotal, failed);
    return failed ? 0 : 1;
}

// ======================= MENU =======================

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    const char *features[] = {
        OBFUSCATE("Button_Siapkan (8 hooks + dump inventory & database)"),  // 0
        OBFUSCATE("InputText_UID item (string)"),                           // 1
        OBFUSCATE("InputValue_Jumlah item (angka)"),                        // 2
        OBFUSCATE("Button_Tambahkan UID ke tas"),                           // 3
        OBFUSCATE("Button_Refresh daftar item di tas (inv_items.txt)"),     // 4
        OBFUSCATE("InputValue_Index item (1..N, lihat inv_items.txt)"),     // 5
        OBFUSCATE("InputValue_Jumlah baru"),                                // 6
        OBFUSCATE("Button_Ubah jumlah item #index")                         // 7
    };

    const int total = (int)(sizeof(features) / sizeof(features[0]));
    jobjectArray ret = (jobjectArray) env->NewObjectArray(
        total, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName,
             jint value, jlong Lvalue, jboolean boolean, jstring text) {
    // ---- featNum 1: input UID (text) ----
    if (featNum == 1) {
        if (text) {
            const char *c = env->GetStringUTFChars(text, nullptr);
            if (c) {
                setManualUid(c);
                fileLog("menu: UID diisi = '%s'", c);
                env->ReleaseStringUTFChars(text, c);
            }
        }
        return;
    }

    // ---- featNum 2: input jumlah ----
    if (featNum == 2) {
        int n = (int) value;
        if (n <= 0 && text) {
            const char *c = env->GetStringUTFChars(text, nullptr);
            if (c) { n = atoi(c); env->ReleaseStringUTFChars(text, c); }
        }
        setManualAmount(n);
        fileLog("menu: jumlah diisi = %d", getManualAmount());
        return;
    }

    // ---- featNum 5: input index item ----
    if (featNum == 5) {
        setEditIndex((int) value);
        fileLog("menu: edit index = %d", getEditIndex());
        return;
    }

    // ---- featNum 6: input jumlah baru ----
    if (featNum == 6) {
        int n = (int) value;
        if (n <= 0 && text) {
            const char *c = env->GetStringUTFChars(text, nullptr);
            if (c) { n = atoi(c); env->ReleaseStringUTFChars(text, c); }
        }
        setEditAmount(n);
        fileLog("menu: jumlah baru = %d", getEditAmount());
        return;
    }

    switch (featNum) {
        case 0: {   // tombol Siapkan
            resolveOutDir(env, obj);
            const char *summary = nullptr;
            fileLog("tombol Siapkan ditekan: mulai memasang 8 hook");
            int result = installAll(&summary);

            char msg[256];
            if (result == -1) {
                snprintf(msg, sizeof msg, "%s", summary ? summary : "libil2cpp belum termuat");
            } else {
                pending.fetch_or(3);
                snprintf(msg, sizeof msg, "%s. Dump inventory + database dijadwalkan. %s",
                         summary ? summary : "Hook selesai",
                         g_tick.load() ? "Menunggu objek tertangkap." : "Menunggu Tick game berjalan.");
            }
            showToast(env, obj, msg);
            break;
        }
        case 3: {   // tombol Tambahkan UID
            std::string uid = getManualUid();
            int cnt = getManualAmount();
            if (uid.empty()) {
                showToast(env, obj, "UID masih kosong. Isi di menu dulu.");
                fileLog("add: ditolak, uid kosong");
                break;
            }
            pending.fetch_or(4);
            char msg[224];
            snprintf(msg, sizeof msg, "Dijadwalkan: %d x %s", cnt, uid.c_str());
            showToast(env, obj, msg);
            fileLog("permintaan tambah UID dijadwalkan: uid=%s jumlah=%d", uid.c_str(), cnt);
            break;
        }
        case 4: {   // tombol Refresh daftar item
            if (!g_pbm.load() && !g_pbmFree.load() && !g_pbmAmt.load()) {
                showToast(env, obj, "Tas belum tertangkap. Buka inventory dulu.");
                fileLog("refresh: ditolak, tas belum tertangkap");
                break;
            }
            pending.fetch_or(16);
            showToast(env, obj, "Refresh daftar item dijadwalkan. Lihat inv_items.txt");
            fileLog("refresh: dijadwalkan");
            break;
        }
        case 7: {   // tombol Ubah jumlah item
            if (getEditIndex() < 1) {
                showToast(env, obj, "Index item belum diisi. Lihat inv_items.txt lalu isi field Index.");
                fileLog("edit: ditolak, index kosong");
                break;
            }
            pending.fetch_or(8);
            char msg[160];
            snprintf(msg, sizeof msg, "Dijadwalkan: item #%d -> %d", getEditIndex(), getEditAmount());
            showToast(env, obj, msg);
            fileLog("edit: dijadwalkan idx=%d amt=%d", getEditIndex(), getEditAmount());
            break;
        }
        default:
            break;
    }
}

// ======================= ENTRY =======================

void hack_thread() {
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }
    LOGI(OBFUSCATE("Done"));
    fileLog("libil2cpp termuat, mod siap");
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}