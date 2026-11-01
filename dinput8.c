/*
 * Prince of Persia: The Revived Sands - smart enemy AI, v3.11  (32-bit proxy for dinput8.dll)
 *
 * Build:  python -m ziglang cc -target x86-windows-gnu -O2 -shared -DPUBLIC_BUILD -o dinput8.dll dinput8.c dinput8.def
 *         (without -DPUBLIC_BUILD = developer build: log files and developer keys are on by default)
 * The notes below are the development history from v1.9 on (kept as written during development).
 *
 *
 * v2.1: charge: enemies closer than near_attack_distance attack without limits (they try to stop the charge), the far ones keep the ring;
 *       earlier detection of the charge (hold 220 ms, ramp 150 ms, punish 300 ms); after an attack that missed (the enemy ends it far from the
 *       Prince) the group steps back for 2.5 s (not when only one enemy is left); roll detection from the Prince's speed (log + optional reaction).
 * v2.0: fixed the hold timer (about every second light click started a charge cycle and blocked enemy attacks for 2.5 s);
 *       the ring never gets narrower than ring_distance (more enemies = wider); only enemies near the Prince are counted;
 *       the cycle does not start against a single enemy (a duel stays as in the original game); spam reaction is off by default;
 *       DIST lines carry the 0..1 value found at +0x1C of the enemy object; snapshots include the actor header and the target.
 * v1.9: ring distance and number of simultaneous attackers depend on the number of enemies in the fight;
 *       the last enemy attacks with shorter pauses (never suicidal); a chain of left-click attacks (3rd in a row and later)
 *       makes the ring step back so the combo hits air; F6/F7/F8 = snapshots of enemy memory (revived_entities.txt).
 *
 * No attack tokens: enemies attack whenever the game's AI lets them.
 *
 * Pattern "surround" (hold):
 *   While the Prince HOLDS the left mouse button (strong attack) for hold_ms:
 *     - enemies start no NEW sword attacks (attacks already in progress finish);
 *     - the fight-ring radii of all enemy types are switched in memory to ring_distance (default 3.0),
 *       so enemies take their ring slots at that distance. Radii are restored on release.
 *   The ring settings (FightRingSetup_*, 4 floats per enemy type) are found in the process memory by their
 *   float values; all hits are logged ("RING found ...") together with the bytes around them.
 *
 * Settings: revived.ini next to the game exe (created with defaults on first run).
 *   Keys in game: F3 = patterns on/off, F5 = reload revived.ini, F2 = rescan ring settings,
 *                 F9 = save memory image, F6/F7/F8 = marker in the trace + snapshot of enemy memory
 * Logs: revived_log.txt, revived_trace.txt.   Uninstall: delete dinput8.dll from the game folder.
 *
 * Build (MSVC, 32-bit): cl /nologo /O2 /LD dinput8.c /link /DEF:dinput8.def /OUT:dinput8.dll
 * Build (zig):  python -m ziglang cc -target x86-windows-gnu -O2 -shared -o dinput8.dll dinput8.c dinput8.def
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#ifdef _MSC_VER
#pragma comment(linker, "/EXPORT:DirectInput8Create=_DirectInput8Create@20")
#define EXPORT
#define THISCALL __thiscall
#define HOOKCALL __fastcall
#else
#define EXPORT __declspec(dllexport)
#define THISCALL __attribute__((thiscall))
#define HOOKCALL __attribute__((fastcall))
#endif

typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);

static HMODULE    g_real;
static PFN_Create g_realCreate;
static FILE      *g_log;
static FILE      *g_trace;
/* Logs and developer hotkeys (F4, F6-F9). A public build (-DPUBLIC_BUILD) writes NO files unless revived.ini has [log] enabled=1;
   a developer build writes them unless [log] enabled=0. */
#ifdef PUBLIC_BUILD
#define LOG_DEFAULT 0
#else
#define LOG_DEFAULT 1
#endif
static int        g_logging = LOG_DEFAULT;
static char       g_dir[MAX_PATH];
static LARGE_INTEGER g_qpf, g_t0;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void path_in_dir(char *out, size_t n, const char *name)
{
    _snprintf(out, n, "%s%s", g_dir, name);
    out[n - 1] = 0;
}

static void open_log(void)
{
    char p[MAX_PATH];
    if (g_log || !g_logging) return;
    path_in_dir(p, sizeof p, "revived_log.txt");
    g_log = fopen(p, "a");
}

static unsigned now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (unsigned)((t.QuadPart - g_t0.QuadPart) * 1000 / g_qpf.QuadPart);
}

/* time since t; never negative (timestamps stored with |1 can be 1 ms ahead of 'now') */
static unsigned age_ms(unsigned now, unsigned t)
{
    int d = (int)(now - t);
    return d < 0 ? 0u : (unsigned)d;
}


/* ---- v2.44: roll lag. The game reads the keyboard through DirectInput; after a tired roll the roll key (and optionally jump / movement) is ignored for a short time.
 * We wrap IDirectInput8::CreateDevice (vtable slot 3) and, for keyboard devices, GetDeviceState (slot 9) and GetDeviceData (slot 10). */
static volatile unsigned g_lmb_dn_t, g_lmb_up_t;
static int g_pr_on = 1, g_pr_ms = 1800, g_pr_pct = 75, g_pr_rise10 = 10, g_kl_on = 1, g_kl_pct = 65, g_kl_ms = 1500;
typedef struct { uint32_t ent; unsigned t; } AsRec; static AsRec g_asr[64];
static void as_note(uint32_t ent, unsigned now) { int i, f = -1; for (i = 0; i < 64; i++) { if (g_asr[i].ent == ent) { g_asr[i].t = now | 1u; return; } if (f < 0 && !g_asr[i].ent) f = i; } if (f < 0) f = (int)(now & 63u); g_asr[f].ent = ent; g_asr[f].t = now | 1u; }
static unsigned as_last(uint32_t ent) { int i; for (i = 0; i < 64; i++) if (g_asr[i].ent == ent) return g_asr[i].t; return 0; }
static volatile unsigned g_rb_until;            /* now_ms() value until which the keys are blocked */
static volatile int g_rl_on = 0, g_rl_step = 300, g_rl_max = 900, g_rl_move = 1, g_rl_jump = 1;
static volatile unsigned g_di_state_calls, g_di_data_calls, g_di_blocked;
typedef HRESULT (WINAPI *DI_CreateDev)(void *self, const GUID *g, void **out, void *unk);
typedef HRESULT (WINAPI *DI_GetState)(void *self, DWORD cb, void *data);
typedef HRESULT (WINAPI *DI_GetData)(void *self, DWORD cbobj, void *rgdod, DWORD *pn, DWORD flags);
typedef struct { void **vt; DI_GetState st; DI_GetData da; } DiVt;
static DiVt g_divt[8]; static int g_ndivt;
static void *g_kbd[4]; static int g_nkbd;
static DI_CreateDev g_orig_create; static void **g_di_vt_patched;
static int di_blocking(void) { unsigned u = g_rb_until; return u && (int)(now_ms() - u) < 0; }
static int di_blocked_key(unsigned k)
{
    if (k == 0x1D || k == 0x9D) return 1;                                            /* Ctrl = roll */
    if (g_rl_jump && k == 0x39) return 1;                                            /* Space */
    if (g_rl_move && (k == 0x11 || k == 0x1E || k == 0x1F || k == 0x20 || k == 0xC8 || k == 0xCB || k == 0xCD || k == 0xD0)) return 1;   /* W A S D, arrows */
    return 0;
}
static DiVt *di_find(void *self) { void **vt = *(void ***)self; int i; for (i = 0; i < g_ndivt; i++) if (g_divt[i].vt == vt) return &g_divt[i]; return NULL; }
static int di_is_kbd(void *self) { int i; for (i = 0; i < g_nkbd; i++) if (g_kbd[i] == self) return 1; return 0; }
static HRESULT WINAPI di_GetState(void *self, DWORD cb, void *data)
{
    DiVt *v = di_find(self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->st(self, cb, data);
    if (SUCCEEDED(hr) && cb == 256 && di_is_kbd(self)) {
        g_di_state_calls++;
        if (di_blocking()) { unsigned k; uint8_t *b = (uint8_t *)data; for (k = 0; k < 256; k++) if (di_blocked_key(k) && b[k]) { b[k] = 0; g_di_blocked++; } }
    }
    return hr;
}
static HRESULT WINAPI di_GetData(void *self, DWORD cbobj, void *rgdod, DWORD *pn, DWORD flags)
{
    DiVt *v = di_find(self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->da(self, cbobj, rgdod, pn, flags);
    if (SUCCEEDED(hr) && rgdod && pn && cbobj >= 8 && di_is_kbd(self)) {
        g_di_data_calls++;
        if (di_blocking() && *pn) {
            DWORD i, o = 0; uint8_t *a = (uint8_t *)rgdod;
            for (i = 0; i < *pn; i++) {
                uint8_t *e = a + (size_t)i * cbobj; DWORD ofs = *(DWORD *)e, dat = *(DWORD *)(e + 4);
                if (di_blocked_key(ofs) && (dat & 0x80)) { g_di_blocked++; continue; }   /* drop key-down events only */
                if (o != i) memmove(a + (size_t)o * cbobj, e, cbobj);
                o++;
            }
            *pn = o;
        }
    }
    return hr;
}
static HRESULT WINAPI di_CreateDevice(void *self, const GUID *g, void **out, void *unk)
{
    HRESULT hr = g_orig_create(self, g, out, unk);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out; int i, known = 0;
        for (i = 0; i < g_ndivt; i++) if (g_divt[i].vt == vt) known = 1;
        if (!known && g_ndivt < 8) {
            DWORD old;
            g_divt[g_ndivt].vt = vt; g_divt[g_ndivt].st = (DI_GetState)vt[9]; g_divt[g_ndivt].da = (DI_GetData)vt[10]; g_ndivt++;
            if (VirtualProtect(vt + 9, 8, PAGE_READWRITE, &old)) { vt[9] = (void *)di_GetState; vt[10] = (void *)di_GetData; VirtualProtect(vt + 9, 8, old, &old); }
        }
        if (g && g->Data1 == 0x6F1D2B61u && g_nkbd < 4) { g_kbd[g_nkbd++] = *out; logf_("[observer] DirectInput keyboard device %p wrapped\n", *out); }
    }
    return hr;
}
static void di_wrap(void *obj)
{
    void **vt; DWORD old;
    if (!obj || g_di_vt_patched) return;
    vt = *(void ***)obj;
    g_orig_create = (DI_CreateDev)vt[3];
    if (VirtualProtect(vt + 3, 4, PAGE_READWRITE, &old)) { vt[3] = (void *)di_CreateDevice; VirtualProtect(vt + 3, 4, old, &old); g_di_vt_patched = vt; logf_("[observer] DirectInput8 object %p: CreateDevice wrapped\n", obj); }
}

/* ---- DirectInput8Create: forward to the real DLL (loaded lazily, not in DllMain) ---- */
EXPORT HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID riid, LPVOID *out, void *unk)
{
    if (!g_real) {
        char sys[MAX_PATH], p[MAX_PATH];
        UINT n = GetSystemWow64DirectoryA(sys, MAX_PATH);
        if (!n) n = GetSystemDirectoryA(sys, MAX_PATH);
        _snprintf(p, sizeof p, "%s\\dinput8.dll", sys);
        p[sizeof p - 1] = 0;
        g_real = LoadLibraryA(p);
        if (g_real) g_realCreate = (PFN_Create)GetProcAddress(g_real, "DirectInput8Create");
        open_log();
        logf_("[observer] real dinput8: %s -> %p\n", p, (void *)g_real);
    }
    if (!g_realCreate) return E_FAIL;
    {
        HRESULT hr = g_realCreate(h, v, riid, out, unk);
        if (SUCCEEDED(hr) && out && *out && riid && (riid->Data1 == 0xBF798030u || riid->Data1 == 0xBF798031u)) di_wrap(*out);
        return hr;
    }
}

/* ---- memory dump (F9) ---- */
static int dump_range(const char *file, uint8_t *base, size_t size)
{
    char p[MAX_PATH];
    FILE *f;
    size_t off = 0;
    uint8_t zero[4096];
    memset(zero, 0, sizeof zero);
    path_in_dir(p, sizeof p, file);
    f = fopen(p, "wb");
    if (!f) { logf_("[observer] cannot create %s\n", p); return 0; }
    while (off < size) {
        MEMORY_BASIC_INFORMATION mbi;
        size_t chunk;
        if (!VirtualQuery(base + off, &mbi, sizeof mbi)) break;
        chunk = (size_t)((uint8_t *)mbi.BaseAddress + mbi.RegionSize - (base + off));
        if (chunk > size - off) chunk = size - off;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            fwrite(base + off, 1, chunk, f);
        } else {
            size_t z = chunk;
            while (z) { size_t w = z > sizeof zero ? sizeof zero : z; fwrite(zero, 1, w, f); z -= w; }
        }
        off += chunk;
    }
    fclose(f);
    return 1;
}

static void do_dump(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    char p[MAX_PATH];
    FILE *info;
    int i;

    path_in_dir(p, sizeof p, "revived_dump_info.txt");
    info = fopen(p, "w");
    if (info) {
        fprintf(info, "module_base=0x%08lX\nimage_base_preferred=0x%08lX\nsize_of_image=0x%lX\nentry_rva=0x%lX\n",
                (unsigned long)(uintptr_t)base, (unsigned long)nt->OptionalHeader.ImageBase,
                (unsigned long)nt->OptionalHeader.SizeOfImage, (unsigned long)nt->OptionalHeader.AddressOfEntryPoint);
    }
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9];
        const char *file = NULL;
        memcpy(name, sec[i].Name, 8); name[8] = 0;
        if (info) fprintf(info, "section %s rva=0x%lX vsize=0x%lX\n", name,
                          (unsigned long)sec[i].VirtualAddress, (unsigned long)sec[i].Misc.VirtualSize);
        if (!strcmp(name, ".text"))  file = "revived_dump_text.bin";
        if (!strcmp(name, ".rdata")) file = "revived_dump_rdata.bin";
        if (!strcmp(name, ".data"))  file = "revived_dump_data.bin";
        if (file) {
            int ok = dump_range(file, base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize);
            logf_("[observer] dumped %s (%s) -> %s\n", name, ok ? "ok" : "FAILED", file);
        }
    }
    if (info) fclose(info);
}

/* ---- settings ---- */
#define NTYPES 6   /* 0 Soldier, 1 Sergeant, 2 Wraith, 3 Scarab, 4 Ghoul, 5 Specter */
static const char *g_typename[NTYPES] = { "soldier", "sergeant", "wraith", "scarab", "ghoul", "specter" };
typedef struct {
    int patterns, hold_ms, ramp_up_ms, full_ms, ramp_down_ms, block_attacks, atk_min, atk_max, slots_inner, slots_outer, slots_ring0, punish_ms, second_ms, third_ms, max_hold_ms, esc_min_pct;
    int by_count, cnt_lo, cnt_hi, pct_lo, per_attacker, last_pct, pct_hi, min_enemies;
    float engage;
    int spam_on, spam_from, spam_gap, spam_hold, spam_pct, spam_step, spam_up;
    float min_dist;
    float near_dist;
    int retreat_on, retreat_pct, retreat_ms, retreat_up, retreat_min;
    float retreat_fail, roll_speed;
    int roll_on, roll_from, roll_window, roll_pct, roll_ms;
    float dist[NTYPES];
    int pulse_on, pulse_gap_min, pulse_gap_max, pulse_ms, pulse_pct, pulse_min_gap_pct, release_ms, cool_ms, near_gap, pulse_first, des_on;
    float des_lo, des_hi, des_reach;
    int des_cache, diag, slot_events, map_ms;
    float app_dist;            /* v2.11: the chosen attacker may swing only when this close */
    int app_on, hold_on, des_to;
    int pr_mode, pr_slots; float pr_dist;
    int crowd_on, crowd_min, crowd_r0, crowd_in, spd_dump;   /* v2.14: more places in the close rings = more simultaneous attackers in normal fights */
    float wave_dist; int wave_ms, wave_on;   /* v2.13: after a charge only close enemies may attack for a while */   /* v2.12: in a pulse only the nearest ring tightens, with few places */
} Config;
static volatile int g_always;
static volatile int g_nolt;
static volatile int g_min0, g_mini, g_mino;   /* v2.29: never fewer places than this in the three rings (all enemy types, from the start, also at rest) */   /* v2.29: 1 = the game's 'straight line to the slot is clear' test (0x59A8E0) always says yes */
static volatile int g_lim_on, g_lim_max = 2, g_lim_ms = 1800;   /* v2.29: cap on simultaneous attackers in a normal fight (ring not wide) */
typedef struct { uint32_t ent; unsigned t; } LimGrant;
static LimGrant g_lim[8];
static unsigned g_lim_refused;
static volatile int g_shield_stay = 1;   /* [shield] no_retreat */
static volatile int g_shield_nocount = 1;   /* [shield] ignore_in_count: shield-bearers do not count toward the crowd that widens the ring */
static volatile int g_wnd_hits = 2, g_wnd_on = 1, g_wnd_ms = 5000, g_wnd_min = 5; static float g_wnd_near = 2.2f;
static volatile int g_mixed_on = 1;          /* [mixed] shield_first */
static volatile int g_ps_on = 1, g_ps_leave = 0; static volatile unsigned g_ps_ref, g_ps_out;   /* v2.96 [pulse_solo]: in a pulse only the chosen attacker may stand in ring 0; the other soldiers leave it and are refused a place there */
static volatile int g_heal_on = 1, g_heal_pct = 50;
static volatile int g_sr_on = 1, g_sr_sq = 0; static volatile int g_sr_inner = 1;   /* v3.00: reserve ring 1 too, and squeeze ring 0 while the shield-bearers are the only ones in the inner rings */
static volatile int g_sr_dummy; static volatile int g_sr_p0 = 24, g_sr_p1 = 36, g_sr_p2 = 90;   /* v3.03 places per ring while shield-bearers are in the crowd */ static volatile float g_sr_free = 2.5f; static volatile unsigned g_sr_ref, g_sr_ring, g_sr_out;   /* v2.99 [shield_reserve]: while a shield-bearer is in the crowd, ring 0 belongs to shield-bearers (soldiers are refused a place there / sent out) and a soldier farther than free_dist may not start an attack */
static volatile int g_sf_on = 1, g_sf_win = 1500; static volatile float g_sf_dist = 5.5f, g_sf_free = 2.0f; static volatile unsigned g_sf_ref;   /* v2.98 [shield_first]: a soldier at a distance does not start an attack for a short window while a shield-bearer is close enough to come in */
static volatile int g_nd_on = 1, g_nd_ms = 2500; static float g_nd_back = 0.10f, g_nd_fwd = 0.08f, g_nd_min = 2.0f, g_nd_safe = 5.0f; static volatile uint32_t g_ptgt;   /* v2.93 [nudge]: after a charge starts soldiers closer than safe distance are pushed back, stuck shield-bearers pushed forward (steps per ~50 ms) */
static volatile int g_sd_on = 1, g_sd_pct = 15; static volatile unsigned g_sd_n;   /* v2.92 [shield_discount]: the game gives an enemy the free slot nearest to where he stands (0x599810), so a shield-bearer stays in the ring he is in; inner slots get their squared distance cut to pct percent for him */
static volatile int g_evt_on = 0, g_evt_leave = 0; static volatile unsigned g_evt_until = 0, g_evt_ms = 2500, g_evt_n = 0;   /* v2.91 [evict_test]: at the start of a pulse every soldier leaves his ring and is refused a new one for evt_ms; only shield-bearers keep places */
static volatile int g_sq_on = 1, g_sq_pct = 60, g_sq_ring = -1;   /* v2.89 [shieldring]: while the ring is wide, the ring where the shield-bearers stand is squeezed to pct percent */
static volatile int g_tp_on = 1, g_tp_small_pct = 70, g_tp_shield_pct = 70, g_tp_crowd_pct = 130, g_tp_crowd_n = 5;   /* v2.87 [tempo]: pulse gap scale by crowd make-up (percent of the normal gap) */
static float g_mx_hold = 6.0f, g_mx_free = 3.0f;
static volatile int g_release_open = 0, g_wnd_ring = 1;
static volatile unsigned g_wnd_ring_block, g_wnd_end_t;
static volatile unsigned g_mx_refused, g_mx_as_s, g_mx_as_o;
static volatile unsigned g_mx_ring_block, g_mx_key_miss, g_mx_chosen_out;
static volatile int g_hs_on;   /* v2.42 hit signal probe */
static volatile int g_rf_on, g_rf_grow = 40, g_rf_max = 250, g_rf_test;   /* v2.41 roll fatigue */
static volatile unsigned g_rchain_pub, g_rlast_pub;
typedef struct { volatile double *p; double base; } RfClip; static RfClip g_rf[64]; static volatile int g_rf_n; static float g_rf_cur = 1.0f;
static volatile int g_ml_on, g_ml_ms = 700; static float g_ml_dist = 3.0f; static unsigned g_ml_forced; typedef struct { uint32_t ent; unsigned t; } MlRec; static MlRec g_ml[16];   /* v2.38 melee push */
static volatile int g_fl_on; static float g_fl_pen = 5.0f; static volatile unsigned g_fl_calls;   /* v2.35 flank */
static volatile int g_rg_on; static float g_rg_dist = 2.4f; static unsigned g_rg_refused;   /* v2.32: reach gate */
static volatile int g_base_on, g_base_pct = 50;   /* v2.31: baseline ring width in normal fights (percent of full charge ring) */   /* v2.29: 1 = the wide ring does not wait for the left mouse: it is up whenever enough enemies fight the Prince */
static volatile Config g_cfg = { 1, 220, 150, 4000, 2500, 1, 1500, 4000, 0, 0, 0, 300, 2000, 4500, 0, 35,
                                 1, 2, 6, 100, 4, 100, 115, 2, 18.0f,
                                 0, 3, 900, 1500, 45, 15, 150,
                                 8.0f,
                                 5.0f,
                                 1, 70, 2500, 250, 2,
                                 2.3f, 7.0f,
                                 0, 3, 4000, 80, 3000,
                                 { 8.0f, 0, 0, 0, 0, 0 },
                                 1, 2500, 4500, 1800, 90, 55, 600, 1500, 1500, 700, 1, 3.0f, 6.0f, 12.0f, 1, 1, 10, 500, 2.4f, 1, 0, 3500, 1, 1, 2.4f, 1, 3, 10, 12, 0, 3.0f, 1500, 1 };

static void load_config(int create_if_missing)
{
    char p[MAX_PATH], buf[64];
    int i;
    FILE *f;
    path_in_dir(p, sizeof p, "revived.ini");
    f = fopen(p, "r");
    if (!f) {
        if (create_if_missing && (f = fopen(p, "w")) != NULL) {
            fprintf(f, "[patterns]\n; 1 = patterns on (F3 toggles)\nenabled=1\n"
                       "; the cycle starts only if at least this many enemies are near the Prince (1 = also against a single enemy)\nmin_enemies=2\n"
                       "; holding left mouse this long (ms) starts the 'surround' cycle\nhold_ms=220\n"
                       "; ring grows to full distance in this time (ms)\nramp_up_ms=150\n"
                       "; ring stays at full distance this long (ms), then shrinks back even if the button is still held\nfull_ms=4000\n"
                       "; ring shrinks back in this time (ms)\nramp_down_ms=2500\n; when the button is released after a charge the ring closes this fast (ms) and every enemy may attack: the Prince is open\nrelease_ramp_ms=600\n; after releasing a charge no new wide ring starts for this long (ms): the crowd attacks instead of retreating again; 0 = off\nafter_release_pause_ms=1500\n; enemies closer than near_attack_distance may start an attack at most once per this many ms (all of them together); 0 = unlimited\nnear_attack_gap_ms=1500\n"
                       "; 1 = while the ring is wide, enemies start no attacks except one random enemy now and then\nblock_attacks=1\n"
                       "; that random attack comes once per random interval min..max (ms); max=0 = nobody attacks\nattack_min_ms=1500\nattack_max_ms=4000\n"
                       "; first N ms of the cycle nothing is blocked: enemies that are already close attack the Prince who starts charging\npunish_ms=300\n"
                       "; after this many ms of the cycle a SECOND enemy may attack at the same time (a third one while the ring shrinks)\nsecond_attacker_ms=2000\n"
                       "; pauses between attacks shrink while holding, down to this percent of attack_min/max\nescalate_min_pct=35\n"
                       "; a THIRD enemy may attack at the same time after this many ms of the cycle\nthird_attacker_ms=4500\n"
                       "; 0 = the ring stays wide as long as the button is held (pressure grows through attacks); otherwise the ring shrinks after this many ms\nmax_hold_ms=0\n"
                       "\n[ring_distance]\n; distance (game units) of the ring while surrounding, per enemy type; 0 = this type is not changed\n"
                       "soldier=8.0\nsergeant=0\nwraith=0\nscarab=0\nghoul=0\nspecter=0\n"
                       "; enemies closer than this (game units) to the Prince attack without limits while he charges; 0 = off\nnear_attack_distance=4.5\n"
                       "\n[ring_slots]\n; number of slots of the 3 rings while surrounding (0 = not changed): ring0 = the nearest ring (default 4), inner (default 6), outer (default 15)\nring0=10\ninner=12\nouter=24\n"
                       "\n[scale_by_enemies]\n; 1 = the wide ring depends on how many enemies fight (asked the AI in the last 4 s)\nenabled=1\n"
                       "; up to count_few enemies the ring is pct_at_few percent of ring_distance, from count_many enemies on pct_at_many percent (in between: smoothly)\ncount_few=2\ncount_many=6\npct_at_few=100\npct_at_many=115\n"
                       "; only enemies closer to the Prince than this (game units) are counted\nengage_distance=18\n"
                       "; the ring is never narrower than this: 8.0 keeps the nearest ring (5.1 units) out of reach of the strong attack; 0 = no limit\nmin_distance=8.0\n"
                       "; one extra simultaneous attacker per this many enemies (4 enemies: 2 attackers, 8: 3); 0 = off\nenemies_per_extra_attacker=4\n"
                       "\n[last_enemy]\n; when the wide ring is already up and only one enemy is left, his pauses between attacks are this percent of the normal ones (100 = no change)\npause_pct=100\n"
                       "\n[retreat]\n; 1 = after an attack that missed the group steps back (not when fewer than min_enemies are near the Prince)\nenabled=1\n; an attack counts as missed if the enemy ends it at least this far (game units) from the Prince; 0 = every attack\nmissed_distance=2.3\n; how far the ring steps back (percent of the full charge ring), how long it stays (ms), how fast it widens (ms)\nstrength_pct=70\nhold_ms=2500\nramp_up_ms=250\nmin_enemies=2\n"
                       "\n[tease]\n; 1 = while the left mouse is held the ring does not stay fixed: every few seconds it tightens for a moment (a pulse). One enemy steps in, teases and may attack, then the ring widens again\nenabled=1\n; 1 = at each pulse the DLL picks one enemy and makes him attack (the game does not ask him by itself); everybody else is not allowed to attack in a pulse\nchosen_attacker=1\n; only an enemy whose distance to the Prince is between these two values (game units) can be chosen\nattacker_min_dist=0.0\nattacker_max_dist=3.4\n; v2.8: reach used by the game attack check; for the chosen attacker it is raised for one check (0 = off)\nreach_boost=3.2\n; 1 = if the game still says no, write its remembered answer as yes\ncache_fallback=1\n; v2.11: 1 = the chosen attacker may swing only when closer than swing_distance; until then he walks in himself (the ring tightens in a pulse)\napproach=1\nswing_distance=3.0\n; v2.12: 1 = in a pulse only the nearest ring tightens (to near_ring_dist) and has near_ring_slots places; the other rings stay wide. 0 = old: the whole ring tightens\nnear_ring_mode=1\nnear_ring_dist=2.4\nnear_ring_slots=1\n; v2.11 experiment (did not work): 1 = others claim to be in place\nhold_others=0\n; the chosen attacker has this long (ms) to arrive and swing\nchosen_timeout_ms=3500\n; the first pulse comes this long (ms) after the ring is wide; later ones after the pause below\nfirst_pulse_ms=700\n; pause between pulses (ms), random between min and max; it shortens the longer the button is held\ngap_min_ms=2500\ngap_max_ms=4500\n; how long one pulse lasts (ms)\npulse_ms=3600\n; how far the ring tightens at the bottom of a pulse: 100 = stays full (nothing happens), 70 = ring0 at about 4 units, 50 = about 3.4\ntighten_pct=18\n; pauses shrink down to this percent of normal while holding\nescalate_min_pct=55\n\n[crowd]\n; v2.14: 1 = when at least min_enemies fight the Prince, the close rings get more places (close_places / inner_places): more enemies can attack at the same time. 0 = game default (6 / 6)\nenabled=1\nmin_enemies=3\nclose_places=10\ninner_places=12\n\n[wave]\n; v2.13: 1 = for window_ms after a charge is released only enemies closer than max_distance (game units) may start an attack\nenabled=1\nwindow_ms=1500\nmax_distance=3.0\n\n[diag]\n; 1 = extra diagnostics in revived_trace.txt (SLOT, POSCAND, RMGR, SETUP, WATCH, MAP lines); does not change behaviour\nenabled=1\n; how many chosen-attacker events get the full slot dump (each is about 40 lines)\nslot_events=10\n; a MAP line (all enemies and their distances) every this many ms while the left mouse is held (0 = off)\nmap_ms=500\n\n[log]\n; 1 = write revived_log.txt and revived_trace.txt and enable the developer keys F4/F6/F7/F8/F9 (for bug reports; read at game start). 0 = write nothing. Public builds default to 0\nenabled=0\n"
                       "\n[roll]\n; 1 = a chain of rolls makes the ring tighter (experimental; the roll is detected from the Prince speed, see ROLL? lines in the trace)\nreaction=0\n; horizontal speed (units/s) that counts as a roll; look at PRINCE-MOVE lines in the trace to find the right value\nroll_speed=7.0\n; the reaction starts from this roll in a row; two rolls closer than window_ms belong to one chain\nfrom_roll=3\nwindow_ms=4000\n; ring size in percent of normal while the reaction lasts, and how long it lasts (ms)\ntighten_pct=80\nhold_ms=3000\n"
                       "\n[spam]\n; 1 = a chain of quick left-click attacks makes the ring step back (experimental; 0 = off)\nenabled=0\n"
                       "; the reaction starts from this attack in a row\nfrom_attack=3\n"
                       "; two clicks closer than this (ms) belong to one chain\nchain_gap_ms=900\n"
                       "; the ring stays wide this long (ms) after the last click\nhold_ms=1500\n"
                       "; strength (percent of the full ring change) on the first reacting attack, and added for each next attack\nstrength_pct=45\nstep_pct=15\n"
                       "; the ring widens in this time (ms); it narrows in ramp_down_ms\nramp_up_ms=150\n");
            fclose(f);
        }
        return;
    }
    fclose(f);
    g_cfg.patterns     = GetPrivateProfileIntA("patterns", "enabled", 1, p);
    g_cfg.hold_ms      = GetPrivateProfileIntA("patterns", "hold_ms", 220, p);
    g_always = GetPrivateProfileIntA("patterns", "always_on", 0, p);
    g_nolt   = GetPrivateProfileIntA("slots", "ignore_path_block", 0, p);
    g_min0   = GetPrivateProfileIntA("slots", "min_ring0", 0, p);
    g_mini   = GetPrivateProfileIntA("slots", "min_inner", 0, p);
    g_mino   = GetPrivateProfileIntA("slots", "min_outer", 0, p);
    g_lim_on  = GetPrivateProfileIntA("limit", "enabled", 0, p);
    g_lim_max = GetPrivateProfileIntA("limit", "max_attackers", 2, p);
    g_lim_ms  = GetPrivateProfileIntA("limit", "grant_ms", 1800, p);
    g_base_on  = GetPrivateProfileIntA("baseline", "enabled", 0, p);
    g_hs_on    = GetPrivateProfileIntA("wounded", "probe_hits", 0, p);
    g_shield_stay = GetPrivateProfileIntA("shield", "no_retreat", 0, p);
    g_mixed_on = GetPrivateProfileIntA("mixed", "shield_first", 1, p);
    g_evt_on = GetPrivateProfileIntA("evict_test", "enabled", 0, p);
    g_ps_on = GetPrivateProfileIntA("pulse_solo", "enabled", 1, p);
    g_sr_on = GetPrivateProfileIntA("shield_reserve", "enabled", 1, p);
    g_sr_p0 = GetPrivateProfileIntA("shield_reserve", "places_ring0", 24, p);
    g_sr_p1 = GetPrivateProfileIntA("shield_reserve", "places_ring1", 36, p);
    g_sr_p2 = GetPrivateProfileIntA("shield_reserve", "places_ring2", 90, p);
    g_sr_free = (float)GetPrivateProfileIntA("shield_reserve", "free_dist_x10", 25, p) / 10.0f;
    g_sf_on = GetPrivateProfileIntA("shield_first", "enabled", 1, p);
    g_sf_win = GetPrivateProfileIntA("shield_first", "window_ms", 1500, p);
    g_sf_dist = (float)GetPrivateProfileIntA("shield_first", "shield_dist_x10", 55, p) / 10.0f;
    g_sf_free = (float)GetPrivateProfileIntA("shield_first", "soldier_free_dist_x10", 20, p) / 10.0f;
    g_nd_on = GetPrivateProfileIntA("nudge", "enabled", 0, p);      /* v2.94: off: the game walks every enemy back to his ring place, a pushed enemy just returns */
    g_nd_ms = GetPrivateProfileIntA("nudge", "window_ms", 2500, p);
    g_nd_back = (float)GetPrivateProfileIntA("nudge", "back_step_x100", 10, p) / 100.0f;
    g_nd_fwd = (float)GetPrivateProfileIntA("nudge", "shield_step_x100", 8, p) / 100.0f;
    if (g_nd_back > 0.30f) g_nd_back = 0.30f; if (g_nd_fwd > 0.30f) g_nd_fwd = 0.30f;
    g_sd_on = GetPrivateProfileIntA("shield_discount", "enabled", 1, p);
    g_sd_pct = GetPrivateProfileIntA("shield_discount", "inner_cost_pct", 15, p);
    if (g_sd_pct < 2) g_sd_pct = 2; if (g_sd_pct > 100) g_sd_pct = 100;
    g_evt_ms = (unsigned)GetPrivateProfileIntA("evict_test", "window_ms", 2500, p);
    if (g_evt_ms < 500u) g_evt_ms = 500u; if (g_evt_ms > 6000u) g_evt_ms = 6000u;
    g_sq_on = GetPrivateProfileIntA("shieldring", "enabled", 0, p);      /* v2.97: off: with the shield-bearers in ring 0 the squeeze put ring 0 at 2.8 for ALL soldiers too (19 of 19 samples) */
    g_sq_pct = GetPrivateProfileIntA("shieldring", "squeeze_pct", 60, p);
    if (g_sq_pct < 35) g_sq_pct = 35; if (g_sq_pct > 100) g_sq_pct = 100;
    g_tp_on = GetPrivateProfileIntA("tempo", "enabled", 1, p);
    g_tp_small_pct = GetPrivateProfileIntA("tempo", "small_group_pct", 70, p);
    g_tp_shield_pct = GetPrivateProfileIntA("tempo", "shields_only_pct", 70, p);
    g_tp_crowd_pct = GetPrivateProfileIntA("tempo", "soldier_crowd_pct", 130, p);
    g_tp_crowd_n = GetPrivateProfileIntA("tempo", "soldier_crowd_from", 5, p);
    if (g_tp_small_pct < 30) g_tp_small_pct = 30; if (g_tp_shield_pct < 30) g_tp_shield_pct = 30; if (g_tp_crowd_pct < 30) g_tp_crowd_pct = 30;
    if (g_tp_small_pct > 300) g_tp_small_pct = 300; if (g_tp_shield_pct > 300) g_tp_shield_pct = 300; if (g_tp_crowd_pct > 300) g_tp_crowd_pct = 300;
    g_release_open = GetPrivateProfileIntA("patterns", "release_open", 0, p);
    g_wnd_ring = GetPrivateProfileIntA("wounded", "outer_ring", 1, p);
    g_mx_hold = (float)GetPrivateProfileIntA("mixed", "soldier_hold_x10", 9999, p) / 10.0f;
    g_mx_free = (float)GetPrivateProfileIntA("mixed", "soldier_free_x10", 30, p) / 10.0f;
    g_wnd_hits = GetPrivateProfileIntA("wounded", "hits_to_wound", 2, p);
    g_wnd_near = (float)GetPrivateProfileIntA("wounded", "attack_dist_x10", 22, p) / 10.0f;
    g_wnd_on = GetPrivateProfileIntA("wounded", "enabled", 1, p);
    g_wnd_ms = GetPrivateProfileIntA("wounded", "wound_ms", 5000, p);
    g_wnd_min = GetPrivateProfileIntA("wounded", "min_enemies", 5, p);
    g_shield_nocount = GetPrivateProfileIntA("shield", "ignore_in_count", 1, p);
    g_rf_on    = GetPrivateProfileIntA("roll_fatigue", "enabled", 0, p);
    g_rf_grow  = GetPrivateProfileIntA("roll_fatigue", "grow_pct", 40, p);
    g_rf_max   = GetPrivateProfileIntA("roll_fatigue", "max_pct", 250, p);
    g_rf_test  = GetPrivateProfileIntA("roll_fatigue", "test_include_roll_clips", 0, p);
    g_rl_on    = GetPrivateProfileIntA("roll_lag", "enabled", 0, p);
    g_rl_step  = GetPrivateProfileIntA("roll_lag", "step_ms", 300, p);
    g_rl_max   = GetPrivateProfileIntA("roll_lag", "max_ms", 900, p);
    g_rl_move  = GetPrivateProfileIntA("roll_lag", "block_move", 1, p);
    g_rl_jump  = GetPrivateProfileIntA("roll_lag", "block_jump", 1, p);
    if (g_rl_step < 0) g_rl_step = 0; if (g_rl_max < 0) g_rl_max = 0; if (g_rl_max > 3000) g_rl_max = 3000;
    if (g_rf_max < 100) g_rf_max = 100; if (g_rf_max > 500) g_rf_max = 500; if (g_rf_grow < 0) g_rf_grow = 0;
    g_ml_on    = GetPrivateProfileIntA("melee", "enabled", 1, p);
    g_ml_ms    = GetPrivateProfileIntA("melee", "interval_ms", 700, p);
    { char b4[32]; GetPrivateProfileStringA("melee", "distance", "3.0", b4, sizeof b4, p); g_ml_dist = (float)atof(b4); }
    g_fl_on    = GetPrivateProfileIntA("flank", "enabled", 0, p);
    { char b3[32]; GetPrivateProfileStringA("flank", "penalty", "5.0", b3, sizeof b3, p); g_fl_pen = (float)atof(b3); if (g_fl_pen < 0.0f) g_fl_pen = 0.0f; if (g_fl_pen > 30.0f) g_fl_pen = 30.0f; }
    g_rg_on    = GetPrivateProfileIntA("reach_gate", "enabled", 0, p);
    { char b2[32]; GetPrivateProfileStringA("reach_gate", "max_distance", "2.4", b2, sizeof b2, p); g_rg_dist = (float)atof(b2); if (g_rg_dist < 1.2f) g_rg_dist = 1.2f; }
    g_base_pct = GetPrivateProfileIntA("baseline", "pct", 50, p);
    if (g_lim_max < 1) g_lim_max = 1;
    if (g_lim_max > 8) g_lim_max = 8;
    g_cfg.ramp_up_ms   = GetPrivateProfileIntA("patterns", "ramp_up_ms", 150, p);
    g_cfg.full_ms      = GetPrivateProfileIntA("patterns", "full_ms", 4000, p);
    g_cfg.ramp_down_ms = GetPrivateProfileIntA("patterns", "ramp_down_ms", 2500, p);
    g_cfg.block_attacks = GetPrivateProfileIntA("patterns", "block_attacks", 1, p);
    g_cfg.atk_min      = GetPrivateProfileIntA("patterns", "attack_min_ms", 1500, p);
    g_cfg.atk_max      = GetPrivateProfileIntA("patterns", "attack_max_ms", 4000, p);
    g_cfg.punish_ms    = GetPrivateProfileIntA("patterns", "punish_ms", 300, p);
    g_cfg.second_ms    = GetPrivateProfileIntA("patterns", "second_attacker_ms", 2000, p);
    g_cfg.third_ms     = GetPrivateProfileIntA("patterns", "third_attacker_ms", 4500, p);
    g_cfg.max_hold_ms  = GetPrivateProfileIntA("patterns", "max_hold_ms", 0, p);
    g_cfg.esc_min_pct  = GetPrivateProfileIntA("patterns", "escalate_min_pct", 35, p);
    if (g_cfg.esc_min_pct < 5) g_cfg.esc_min_pct = 5;
    if (g_cfg.esc_min_pct > 100) g_cfg.esc_min_pct = 100;
    g_cfg.slots_ring0  = GetPrivateProfileIntA("ring_slots", "ring0", 0, p);
    g_cfg.slots_inner  = GetPrivateProfileIntA("ring_slots", "inner", 0, p);
    g_cfg.slots_outer  = GetPrivateProfileIntA("ring_slots", "outer", 0, p);
    for (i = 0; i < NTYPES; i++) {
        float v;
        GetPrivateProfileStringA("ring_distance", g_typename[i], i == 0 ? "8.0" : "0", buf, sizeof buf, p);
        v = (float)atof(buf);
        if (v != 0 && v < 0.5f) v = 0.5f;
        if (v > 20.0f) v = 20.0f;
        g_cfg.dist[i] = v;
    }
    g_cfg.by_count     = GetPrivateProfileIntA("scale_by_enemies", "enabled", 1, p);
    g_cfg.cnt_lo       = GetPrivateProfileIntA("scale_by_enemies", "count_few", 2, p);
    g_cfg.cnt_hi       = GetPrivateProfileIntA("scale_by_enemies", "count_many", 6, p);
    g_cfg.pct_lo       = GetPrivateProfileIntA("scale_by_enemies", "pct_at_few", 100, p);
    g_cfg.pct_hi       = GetPrivateProfileIntA("scale_by_enemies", "pct_at_many", 115, p);
    g_cfg.min_enemies  = GetPrivateProfileIntA("patterns", "min_enemies", 2, p);
    GetPrivateProfileStringA("scale_by_enemies", "engage_distance", "18", buf, sizeof buf, p);
    g_cfg.engage       = (float)atof(buf);
    g_cfg.per_attacker = GetPrivateProfileIntA("scale_by_enemies", "enemies_per_extra_attacker", 4, p);
    GetPrivateProfileStringA("scale_by_enemies", "min_distance", "8.0", buf, sizeof buf, p);
    g_cfg.min_dist     = (float)atof(buf);
    g_cfg.last_pct     = GetPrivateProfileIntA("last_enemy", "pause_pct", 100, p);
    GetPrivateProfileStringA("patterns", "near_attack_distance", "5.0", buf, sizeof buf, p);
    g_cfg.near_dist    = (float)atof(buf);
    g_cfg.retreat_on   = GetPrivateProfileIntA("retreat", "enabled", 1, p);
    GetPrivateProfileStringA("retreat", "missed_distance", "2.3", buf, sizeof buf, p);
    g_cfg.retreat_fail = (float)atof(buf);
    g_cfg.retreat_pct  = GetPrivateProfileIntA("retreat", "strength_pct", 70, p);
    g_cfg.retreat_ms   = GetPrivateProfileIntA("retreat", "hold_ms", 2500, p);
    g_cfg.retreat_up   = GetPrivateProfileIntA("retreat", "ramp_up_ms", 250, p);
    g_cfg.retreat_min  = GetPrivateProfileIntA("retreat", "min_enemies", 2, p);
    g_cfg.des_on       = GetPrivateProfileIntA("tease", "chosen_attacker", 1, p);
    GetPrivateProfileStringA("tease", "attacker_min_dist", "0.0", buf, sizeof buf, p);
    g_cfg.des_lo       = (float)atof(buf);
    GetPrivateProfileStringA("tease", "attacker_max_dist", "3.4", buf, sizeof buf, p);
    g_cfg.des_hi       = (float)atof(buf);
    GetPrivateProfileStringA("tease", "reach_boost", "3.2", buf, sizeof buf, p);
    g_cfg.des_reach    = (float)atof(buf);
    g_cfg.des_cache    = GetPrivateProfileIntA("tease", "cache_fallback", 1, p);
    g_cfg.spd_dump     = GetPrivateProfileIntA("diag", "speed_dump", 0, p);
    g_cfg.crowd_on     = GetPrivateProfileIntA("crowd", "enabled", 1, p);
    g_cfg.crowd_min    = GetPrivateProfileIntA("crowd", "min_enemies", 3, p);
    g_cfg.crowd_r0     = GetPrivateProfileIntA("crowd", "close_places", 10, p);
    g_cfg.crowd_in     = GetPrivateProfileIntA("crowd", "inner_places", 12, p);
    if (g_cfg.crowd_min < 2) g_cfg.crowd_min = 2;
    g_cfg.wave_on      = GetPrivateProfileIntA("wave", "enabled", 1, p);
    g_cfg.wave_ms      = GetPrivateProfileIntA("wave", "window_ms", 1500, p);
    GetPrivateProfileStringA("wave", "max_distance", "3.0", buf, sizeof buf, p);
    g_cfg.wave_dist    = (float)atof(buf);
    if (g_cfg.wave_ms < 0) g_cfg.wave_ms = 0;
    if (g_cfg.wave_dist < 1.0f) g_cfg.wave_dist = 1.0f;
    g_cfg.app_on       = GetPrivateProfileIntA("tease", "approach", 1, p);
    GetPrivateProfileStringA("tease", "swing_distance", "3.0", buf, sizeof buf, p);
    g_cfg.app_dist     = (float)atof(buf);
    g_cfg.hold_on      = GetPrivateProfileIntA("tease", "hold_others", 0, p);
    g_cfg.pr_mode      = GetPrivateProfileIntA("tease", "near_ring_mode", 1, p);
    g_cfg.pr_slots     = GetPrivateProfileIntA("tease", "near_ring_slots", 1, p);
    GetPrivateProfileStringA("tease", "near_ring_dist", "2.4", buf, sizeof buf, p);
    g_cfg.pr_dist      = (float)atof(buf);
    if (g_cfg.pr_slots < 1) g_cfg.pr_slots = 1;
    if (g_cfg.pr_dist < 1.2f) g_cfg.pr_dist = 1.2f;
    g_cfg.des_to       = GetPrivateProfileIntA("tease", "chosen_timeout_ms", 3500, p);
    if (g_cfg.app_dist < 1.0f) g_cfg.app_dist = 1.0f;
    if (g_cfg.des_to < 800) g_cfg.des_to = 800;
    g_cfg.diag         = GetPrivateProfileIntA("diag", "enabled", 1, p);
    g_heal_on = GetPrivateProfileIntA("heal", "enabled", 1, p); g_heal_pct = GetPrivateProfileIntA("heal", "scale_pct", 50, p); if (g_heal_pct < 10) g_heal_pct = 10; if (g_heal_pct > 100) g_heal_pct = 100;
    g_cfg.slot_events  = GetPrivateProfileIntA("diag", "slot_events", 10, p);
    g_cfg.map_ms       = GetPrivateProfileIntA("diag", "map_ms", 500, p);
    g_cfg.cool_ms      = GetPrivateProfileIntA("patterns", "after_release_pause_ms", 1500, p);
    g_cfg.near_gap     = GetPrivateProfileIntA("patterns", "near_attack_gap_ms", 1500, p);
    g_cfg.pulse_first  = GetPrivateProfileIntA("tease", "first_pulse_ms", 700, p);
    if (g_cfg.cool_ms < 0) g_cfg.cool_ms = 0;
    if (g_cfg.near_gap < 0) g_cfg.near_gap = 0;
    if (g_cfg.pulse_first < 200) g_cfg.pulse_first = 200;
    g_cfg.release_ms   = GetPrivateProfileIntA("patterns", "release_ramp_ms", 600, p);
    if (g_cfg.release_ms < 50) g_cfg.release_ms = 50;
    g_cfg.pulse_on     = GetPrivateProfileIntA("tease", "enabled", 1, p);
    g_cfg.pulse_gap_min = GetPrivateProfileIntA("tease", "gap_min_ms", 2500, p);
    g_cfg.pulse_gap_max = GetPrivateProfileIntA("tease", "gap_max_ms", 4500, p);
    g_cfg.pulse_ms     = GetPrivateProfileIntA("tease", "pulse_ms", 3600, p);
    g_cfg.pulse_pct    = GetPrivateProfileIntA("tease", "tighten_pct", 18, p);
    g_cfg.pulse_min_gap_pct = GetPrivateProfileIntA("tease", "escalate_min_pct", 55, p);
    g_cfg.roll_on      = GetPrivateProfileIntA("roll", "reaction", 0, p);
    GetPrivateProfileStringA("roll", "roll_speed", "7.0", buf, sizeof buf, p);
    g_cfg.roll_speed   = (float)atof(buf);
    g_cfg.roll_from    = GetPrivateProfileIntA("roll", "from_roll", 3, p);
    g_cfg.roll_window  = GetPrivateProfileIntA("roll", "window_ms", 4000, p);
    g_cfg.roll_pct     = GetPrivateProfileIntA("roll", "tighten_pct", 80, p);
    g_cfg.roll_ms      = GetPrivateProfileIntA("roll", "hold_ms", 3000, p);
    if (g_cfg.near_dist < 0) g_cfg.near_dist = 0;
    g_mixed_on = 0;      /* v2.88: the shield-bearer chosen to come in arrived 3 times out of 11 (8 timeouts), while soldiers were refused 290 times: the whole crowd stood still */
    g_cfg.near_dist = 3.5f; g_cfg.near_gap = 0;      /* v2.80 (no group limit; was 700 in v2.79): the 1-2 enemies that stay close during a charge (median 3.0-3.6 units in the log) strike more often: closer than 3.5 units, one swing start per 0.7 s for the group */
    if (g_cfg.pulse_gap_min < 500) g_cfg.pulse_gap_min = 500;
    if (g_cfg.pulse_gap_max < g_cfg.pulse_gap_min) g_cfg.pulse_gap_max = g_cfg.pulse_gap_min;
    if (g_cfg.pulse_ms < 600) g_cfg.pulse_ms = 600;
    if (g_cfg.pulse_pct < 8) g_cfg.pulse_pct = 8;
    if (g_cfg.pulse_pct > 100) g_cfg.pulse_pct = 100;
    if (g_cfg.pulse_min_gap_pct < 20) g_cfg.pulse_min_gap_pct = 20;
    if (g_cfg.pulse_min_gap_pct > 100) g_cfg.pulse_min_gap_pct = 100;
    if (g_cfg.retreat_pct < 5) g_cfg.retreat_pct = 5;
    if (g_cfg.retreat_pct > 100) g_cfg.retreat_pct = 100;
    if (g_cfg.retreat_ms < 200) g_cfg.retreat_ms = 200;
    if (g_cfg.retreat_up < 50) g_cfg.retreat_up = 50;
    if (g_cfg.retreat_min < 1) g_cfg.retreat_min = 1;
    if (g_cfg.roll_speed < 2.0f) g_cfg.roll_speed = 2.0f;
    if (g_cfg.roll_from < 2) g_cfg.roll_from = 2;
    if (g_cfg.roll_pct < 40) g_cfg.roll_pct = 40;
    if (g_cfg.roll_pct > 100) g_cfg.roll_pct = 100;
    if (g_cfg.roll_ms < 200) g_cfg.roll_ms = 200;
    g_rf_on = 0;      /* v2.84: SWITCHED OFF again: rewriting the clip lengths in memory did not change the roll spam interval (see log numbers). v2.82: roll fatigue is switched on in the code (it was off in the ini); first roll normal, the next ones lengthen the recovery clips */
    g_pr_on     = GetPrivateProfileIntA("pressure", "retreat_pressure", 1, p);
    g_pr_ms     = GetPrivateProfileIntA("pressure", "retreat_hold_ms", 1800, p);
    g_pr_pct    = GetPrivateProfileIntA("pressure", "retreat_ring_pct", 75, p);
    g_pr_rise10 = GetPrivateProfileIntA("pressure", "retreat_rise_x10", 10, p);
    g_kl_on     = GetPrivateProfileIntA("pressure", "kill_answer", 1, p);
    g_kl_pct    = GetPrivateProfileIntA("pressure", "kill_ring_pct", 65, p);
    g_kl_ms     = GetPrivateProfileIntA("pressure", "kill_hold_ms", 1500, p);
    if (g_pr_pct < 40) g_pr_pct = 40; if (g_pr_pct > 100) g_pr_pct = 100;
    if (g_kl_pct < 40) g_kl_pct = 40; if (g_kl_pct > 100) g_kl_pct = 100;
    if (g_pr_ms < 300) g_pr_ms = 300; if (g_kl_ms < 300) g_kl_ms = 300;
    if (g_pr_rise10 < 3) g_pr_rise10 = 3;
    g_cfg.spam_on      = GetPrivateProfileIntA("spam", "enabled", 0, p);
    g_cfg.spam_from    = GetPrivateProfileIntA("spam", "from_attack", 3, p);
    g_cfg.spam_gap     = GetPrivateProfileIntA("spam", "chain_gap_ms", 900, p);
    g_cfg.spam_hold    = GetPrivateProfileIntA("spam", "hold_ms", 1500, p);
    g_cfg.spam_pct     = GetPrivateProfileIntA("spam", "strength_pct", 45, p);
    g_cfg.spam_step    = GetPrivateProfileIntA("spam", "step_pct", 15, p);
    g_cfg.spam_up      = GetPrivateProfileIntA("spam", "ramp_up_ms", 150, p);
    if (g_cfg.cnt_hi <= g_cfg.cnt_lo) g_cfg.cnt_hi = g_cfg.cnt_lo + 1;
    if (g_cfg.pct_lo < 100) g_cfg.pct_lo = 100;                     /* the ring is never narrower than ring_distance */
    if (g_cfg.pct_lo > 200) g_cfg.pct_lo = 200;
    if (g_cfg.pct_hi < g_cfg.pct_lo) g_cfg.pct_hi = g_cfg.pct_lo;
    if (g_cfg.pct_hi > 200) g_cfg.pct_hi = 200;
    if (g_cfg.min_enemies < 1) g_cfg.min_enemies = 1;
    if (g_cfg.engage < 6.0f) g_cfg.engage = 6.0f;
    if (g_cfg.per_attacker < 0) g_cfg.per_attacker = 0;
    if (g_cfg.min_dist < 0) g_cfg.min_dist = 0;
    if (g_cfg.last_pct < 10) g_cfg.last_pct = 10;
    if (g_cfg.last_pct > 100) g_cfg.last_pct = 100;
    if (g_cfg.spam_from < 2) g_cfg.spam_from = 2;
    if (g_cfg.spam_gap < 200) g_cfg.spam_gap = 200;
    if (g_cfg.spam_hold < 200) g_cfg.spam_hold = 200;
    if (g_cfg.spam_pct < 5) g_cfg.spam_pct = 5;
    if (g_cfg.spam_pct > 100) g_cfg.spam_pct = 100;
    if (g_cfg.spam_step < 0) g_cfg.spam_step = 0;
    if (g_cfg.spam_up < 50) g_cfg.spam_up = 50;
    if (g_cfg.ramp_up_ms < 50) g_cfg.ramp_up_ms = 50;
    if (g_cfg.ramp_down_ms < 50) g_cfg.ramp_down_ms = 50;
    if (g_cfg.full_ms < 0) g_cfg.full_ms = 0;
    if (g_cfg.slots_ring0 < 0) g_cfg.slots_ring0 = 0;
    if (g_cfg.slots_ring0 > 40) g_cfg.slots_ring0 = 40;
    if (g_cfg.slots_inner < 0) g_cfg.slots_inner = 0;
    if (g_cfg.slots_inner > 40) g_cfg.slots_inner = 40;
    if (g_cfg.slots_outer < 0) g_cfg.slots_outer = 0;
    if (g_cfg.slots_outer > 60) g_cfg.slots_outer = 60;
    if (g_cfg.atk_max > 0 && g_cfg.atk_max < g_cfg.atk_min) g_cfg.atk_max = g_cfg.atk_min;
}

/* ---- trace buffer: hooks only append; a worker thread writes to disk ---- */
#define RING 8192
typedef struct {
    unsigned t;
    char     kind[3];
    uint8_t  res;
    uint32_t ent, entvt, p1, p2;
    float    f;
    char     txt[480];
} Event;

static Event          g_ring[RING];
static volatile LONG  g_head, g_tail, g_lock;
static volatile LONG  g_dropped;

static Event *begin_event(void)
{
    LONG h;
    while (InterlockedExchange(&g_lock, 1)) { }
    h = g_head;
    if (((h + 1) % RING) == g_tail) { g_dropped++; InterlockedExchange(&g_lock, 0); return NULL; }
    return &g_ring[h];   /* lock stays held until end_event */
}
static void end_event(void)
{
    g_head = (g_head + 1) % RING;
    InterlockedExchange(&g_lock, 0);
}

static void push_event(const char *kind, uint32_t ent, uint32_t p1, uint32_t p2, float f, uint8_t res)
{
    Event *e = begin_event();
    if (!e) return;
    e->t = now_ms();
    e->kind[0] = kind[0]; e->kind[1] = kind[1]; e->kind[2] = 0;
    e->res = res; e->ent = ent; e->p1 = p1; e->p2 = p2; e->f = f;
    e->entvt = 0;
    e->txt[0] = 0;
    if (ent && !IsBadReadPtr((void *)(uintptr_t)ent, 4)) e->entvt = *(uint32_t *)(uintptr_t)ent;
    end_event();
}

static void push_text(const char *fmt, ...)
{
    va_list ap;
    Event *e = begin_event();
    if (!e) return;
    e->t = now_ms();
    e->kind[0] = 'T'; e->kind[1] = 'X'; e->kind[2] = 0;
    va_start(ap, fmt);
    _vsnprintf(e->txt, sizeof e->txt, fmt, ap);
    va_end(ap);
    e->txt[sizeof e->txt - 1] = 0;
    end_event();
}

#define STATE_N 4096
typedef struct { uint32_t key_ent, key_p; uint8_t kind, res, used; } State;
static State g_state[STATE_N];

/* report only when the result for (condition kind, enemy, parameter) changes */
static void note(uint8_t kindId, const char *kind, void *ent, uint32_t p1, uint32_t p2, float f, uint8_t res)
{
    uint32_t e = (uint32_t)(uintptr_t)ent;
    uint32_t key = p1 * 31u + p2 * 17u + (uint32_t)kindId * 131u + (uint32_t)(f * 100.0f);
    uint32_t idx = (e * 2654435761u ^ key * 40503u) % STATE_N;
    int tries;
    for (tries = 0; tries < 8; tries++) {
        State *s = &g_state[(idx + tries) % STATE_N];
        if (!s->used) {
            s->used = 1; s->key_ent = e; s->key_p = key; s->kind = kindId; s->res = res;
            push_event(kind, e, p1, p2, f, res);
            return;
        }
        if (s->key_ent == e && s->key_p == key && s->kind == kindId) {
            if (s->res != res) { s->res = res; push_event(kind, e, p1, p2, f, res); }
            return;
        }
    }
}

/* ---- ring settings in memory ---- */
static volatile unsigned g_rel_t, g_rel_hold;   /* time the left mouse was released after a long hold + how long it was held */
static volatile unsigned g_lmb_since;   /* 0 = left mouse not held, else time it went down (|1) */
static volatile int g_wide;             /* 1 while the ring is (mostly) wide: attacks are gated */
static volatile LONG g_relayout;

/* enemies that asked the AI recently (filled by the CombatBehavior hook) */
#define NSEEN 64
#define SEEN_MS 2000u
typedef struct { uint32_t ent; unsigned t; float d; } Seen;     /* d = last measured distance to the Prince, -1 = unknown */
static Seen g_seen[NSEEN];
static volatile float g_cnt_k = 1.0f;            /* ring scale from the number of enemies (smoothed) */
static volatile int g_nenemies;

static void seen_touch(uint32_t ent, unsigned now)
{
    int i, free_i = -1;
    for (i = 0; i < NSEEN; i++) {
        if (g_seen[i].ent == ent) { g_seen[i].t = now; return; }
        if (free_i < 0 && (g_seen[i].ent == 0 || now - g_seen[i].t > SEEN_MS)) free_i = i;
    }
    if (free_i >= 0) { g_seen[free_i].ent = ent; g_seen[free_i].t = now; g_seen[free_i].d = -1.0f; }
}

static float seen_get_dist(uint32_t ent)
{
    int i;
    for (i = 0; i < NSEEN; i++) if (g_seen[i].ent == ent) return g_seen[i].d;
    return -1.0f;
}

static void seen_set_dist(uint32_t ent, float d)
{
    int i;
    for (i = 0; i < NSEEN; i++) if (g_seen[i].ent == ent) { g_seen[i].d = d; return; }
}

static int ent_kind(uint32_t ent);
static int ring_of(uint32_t member);
static uint32_t member_of(uint32_t ent);
static void shield_ring_track(unsigned now);
static void slotd_tick(unsigned now);
static volatile int g_n_shield, g_n_other;   /* v2.53: shield-bearers / the rest near the Prince (updated by enemy_count) */
static int wounded_now(uint32_t ent);
static int enemy_count(unsigned now)
{
    int i, n = 0, ns = 0, no = 0;
    for (i = 0; i < NSEEN; i++)
        if (g_seen[i].ent && now - g_seen[i].t <= SEEN_MS && (g_seen[i].d < 0 || g_seen[i].d <= g_cfg.engage)) {
            if (ent_kind(g_seen[i].ent) == 2) ns++; else no++;
            n++;
        }
    g_n_shield = ns; g_n_other = no;
    if (g_shield_nocount && no == 0) return 0;      /* only shield-bearers: no crowd, the ring is not widened */
    return n;
}

/* spam of the left button: chain of quick clicks */
static volatile unsigned g_last_press;           /* time of the last click (|1), 0 = none */
static volatile unsigned g_chain;                /* length of the click chain */
static float g_fs;                               /* ring factor from the spam reaction and the retreat, 0..1 */
static float g_ft, g_ft_written = -1.0f;        /* ring tightening from the roll reaction, 0..1 */
static volatile unsigned g_retreat_until, g_roll_until;
/* v2.75: pressure when the Prince backs off after attacking, and the answer to a kill */
static float g_fpr, g_fpr_written = -1.0f, g_fpr_pct = 75.0f;       /* ring tightening 0..1 and the ring size in percent while it is on */
static volatile unsigned g_press_until, g_kill_until;
static volatile unsigned g_last_hit_t;
/* v2.85: fight rating: counters for one fight, written to revived_fights.csv when the fight ends */
static volatile unsigned g_fr_kills, g_fr_hits, g_fr_rolls, g_fr_holds, g_fr_forced;
static float g_hab[8]; static volatile int g_pun_dir;      /* v2.82: where the Prince tends to roll to (8 sectors, decaying) */
static void hab_add(float dx, float dy)
{
    int i, b; float a = atan2f(dy, dx);
    b = (int)floorf((a + 3.14159265f) / 0.78539816f) & 7;
    for (i = 0; i < 8; i++) g_hab[i] *= 0.85f;
    g_hab[b] += 1.0f;
}
static int hab_dir(float *ang)
{
    int i, best = 0; float sum = 0.0f, loc;
    for (i = 0; i < 8; i++) { sum += g_hab[i]; if (g_hab[i] > g_hab[best]) best = i; }
    if (sum < 2.5f) return 0;
    loc = g_hab[best] + g_hab[(best + 1) & 7] + g_hab[(best + 7) & 7];
    if (loc / sum < 0.6f) return 0;
    *ang = -3.14159265f + ((float)best + 0.5f) * 0.78539816f;
    return 1;
}
static volatile int g_pun_budget; static volatile unsigned g_pun_until, g_pun_from;      /* v2.76: swings the answer to a kill / to the Prince's back-off may still force */
static int recent_hit_ent(uint32_t ent, unsigned within);
static int wound_return_due(uint32_t ent);
static void wound_return_done(uint32_t ent);
static int kill_open(void) { return g_kl_on && g_kill_until && (int)(g_kill_until - now_ms()) > 0; }
static volatile int g_stat_allowed, g_stat_blocked, g_stat_asks, g_stat_near;

#define MAXHIT 48
#define HITW 10                          /* words copied per structure: 4 radius floats, then +0x20..+0x34 of the setup */
typedef struct { float *p; uint32_t orig[HITW]; int type; } Hit;
static Hit g_hits[MAXHIT];
static int g_nhits;

static const float g_quads[][4] = {          /* Soldier, Wraith/Scarab, Ghoul, Sergeant, Specter (from Boot.bin) */
    {1.6f, 1.2f, 2.5f, 3.5f}, {2.25f, 1.5f, 3.5f, 4.5f}, {2.0f, 1.5f, 4.5f, 6.0f},
    {2.0f, 1.5f, 4.0f, 5.5f}, {2.25f, 1.5f, 4.8f, 6.6f}
};

static int hit_type(int q, uint32_t first_int)
{
    switch (q) {
        case 0: return 0;
        case 1: return first_int == 1 ? 3 : 2;
        case 2: return 4;
        case 3: return 1;
        default: return 5;
    }
}

static float f_of(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static uint32_t w_of(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

static int g_crowd, g_crowd_written = -1;   /* v2.14 */
static float g_pd, g_pd_written = -1.0f;   /* v2.12: pulse depth 0..1: the nearest ring comes in */

/* write the ring settings of one structure for factor f (0 = original, 1 = full) */
static void write_hit(Hit *h, float f)
{
    uint32_t v[HITW];
    float target = g_cfg.dist[h->type];
    memcpy(v, h->orig, sizeof v);
    if (g_cfg.by_count && target > 0) {
        float t2 = target * g_cnt_k;
        if (t2 < g_cfg.min_dist && target >= g_cfg.min_dist) t2 = g_cfg.min_dist;
        target = t2;
    }
    float kt = (1.0f - g_ft * (1.0f - (float)g_cfg.roll_pct / 100.0f)) * (1.0f - g_fpr * (1.0f - g_fpr_pct / 100.0f));
    if (f > 0.001f && target > 0) {
        float k = (1.0f + f * (target / f_of(h->orig[2]) - 1.0f)) * kt;
        int j;
        for (j = 0; j < 4; j++) v[j] = w_of(f_of(h->orig[j]) * k);
        if (g_sq_on && f > 0.05f && g_sq_ring >= 0 && g_sq_ring < 3) {      /* v2.89: the ring with the shield-bearers comes closer (guess: ring index = radius index; the SHRING log shows whether it fits) */
            float rr = f_of(v[g_sq_ring]) * (float)g_sq_pct / 100.0f;
            if (rr < 2.5f) rr = 2.5f;
            if (rr < f_of(v[g_sq_ring])) v[g_sq_ring] = w_of(rr);
        }
        if (g_sr_sq && f > 0.05f) {      /* v3.03: most ring places are blocked by walls (RINGSTAT: 60-76% blocked) and the rest are full; with shield-bearers around the rings get 3x the places so a free usable place exists for them */
            { float r0q = f_of(v[0]) * (float)g_sq_pct / 100.0f; if (r0q < 2.5f) r0q = 2.5f; if (r0q < f_of(v[0])) v[0] = w_of(r0q); }      /* v3.05: ring 0 to 60%: the shield-bearers come in to 3-4 */
            if ((int)v[7] < g_sr_p0) { v[7] = (uint32_t)g_sr_p0; v[4] = (uint32_t)g_sr_p0; }
            if ((int)v[8] < g_sr_p1) v[8] = (uint32_t)g_sr_p1;
            if ((int)v[9] < g_sr_p2) v[9] = (uint32_t)g_sr_p2;
        }
        if (f > 0.05f && g_cfg.slots_ring0 > 0) { v[4] = (uint32_t)g_cfg.slots_ring0; v[7] = (uint32_t)g_cfg.slots_ring0 > h->orig[7] ? (uint32_t)g_cfg.slots_ring0 : h->orig[7]; }
        if (f > 0.05f && g_cfg.slots_inner > 0) v[8] = (uint32_t)g_cfg.slots_inner;
        if (f > 0.05f && g_cfg.slots_outer > 0) v[9] = (uint32_t)g_cfg.slots_outer;
        if (g_pd > 0.01f && h->type == 0) {      /* soldiers: only the nearest ring comes in, with few places; the other rings stay wide */
            float a = f_of(v[0]), b = f_of(v[1]), ta = g_cfg.pr_dist, tb = g_cfg.pr_dist * 0.75f;
            if (ta < a) { v[0] = w_of(a + (ta - a) * g_pd); v[1] = w_of(b + (tb - b) * g_pd); }
            if (g_pd >= 0.5f) { v[4] = (uint32_t)g_cfg.pr_slots; v[7] = (uint32_t)g_cfg.pr_slots; }
        }
    } else if (kt < 0.999f) {
        int j;
        for (j = 0; j < 4; j++) v[j] = w_of(f_of(h->orig[j]) * kt);
    }
    if (g_crowd && h->type == 0 && !(g_pd >= 0.5f)) {      /* v2.29: not while the nearest ring is in a pulse (it has to keep its single place) */              /* v2.14: in a fight of several enemies the close rings get more places: more of them can attack at once */
        if (g_cfg.crowd_r0 > (int)v[7]) { v[7] = (uint32_t)g_cfg.crowd_r0; v[4] = (uint32_t)g_cfg.crowd_r0; }
        if (g_cfg.crowd_in > (int)v[8]) v[8] = (uint32_t)g_cfg.crowd_in;
    }
    if (g_min0 > (int)v[7] && !(g_pd >= 0.5f && h->type == 0)) { v[7] = (uint32_t)g_min0; if (g_min0 > (int)v[4]) v[4] = (uint32_t)g_min0; }   /* v2.29: not while the nearest ring is in a pulse */
    if (g_mini > (int)v[8]) v[8] = (uint32_t)g_mini;
    if (g_mino > (int)v[9]) v[9] = (uint32_t)g_mino;
    /* radius floats and slot counts are separate ranges; write the whole span, they are adjacent */
    WriteProcessMemory(GetCurrentProcess(), h->p, v, sizeof v, NULL);
}

#define CHUNK 0x100000
static uint8_t g_chunk[CHUNK + 64];

/* Scans committed read/write memory with ReadProcessMemory (never faults, even if the game frees memory meanwhile) */
static void scan_rings(void)
{
    SYSTEM_INFO si;
    uint8_t *a;
    HANDLE me = GetCurrentProcess();
    GetSystemInfo(&si);
    for (a = (uint8_t *)si.lpMinimumApplicationAddress; a < (uint8_t *)si.lpMaximumApplicationAddress; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize > 48) {
            uint8_t *base = (uint8_t *)mbi.BaseAddress;
            size_t done = 0;
            while (done < mbi.RegionSize && g_nhits < MAXHIT) {
                size_t want = mbi.RegionSize - done, i;
                SIZE_T got = 0;
                if (want > CHUNK) want = CHUNK;
                if (want > 48 && ReadProcessMemory(me, base + done, g_chunk, want, &got) && got >= 40) {
                    for (i = 0; i + 40 <= got && g_nhits < MAXHIT; i += 4) {
                        int q;
                        uint8_t *at = base + done + i;
                        if (at >= (uint8_t *)g_hits && at < (uint8_t *)(g_hits + MAXHIT)) continue;
                        for (q = 0; q < 5; q++) {
                            if (memcmp(g_chunk + i, g_quads[q], 16) == 0) {
                                Hit *h;
                                int k, dup = 0;
                                for (k = 0; k < g_nhits; k++) if ((uint8_t *)g_hits[k].p == at) dup = 1;
                                if (dup) break;
                                h = &g_hits[g_nhits++];
                                h->p = (float *)at;
                                memcpy(h->orig, g_chunk + i, sizeof h->orig);
                                h->type = hit_type(q, h->orig[4]);
                                push_text("RING found at %p type=%s slots=%u/%u/%u", (void *)at, g_typename[h->type],
                                          h->orig[7], h->orig[8], h->orig[9]);
                                break;
                            }
                        }
                    }
                }
                done += want > 48 ? want - 48 : want;
            }
        }
        a = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (a <= (uint8_t *)mbi.BaseAddress) break;
    }
    push_text("RING scan done: %d hits", g_nhits);
    if (g_min0 || g_mini || g_mino) { int q; for (q = 0; q < g_nhits; q++) write_hit(&g_hits[q], 0.0f); g_relayout = 1; push_text("SLOTS minimum places applied (%d/%d/%d)", g_min0, g_mini, g_mino); }
}

/* ---- surround cycle: IDLE -> UP -> FULL -> DOWN -> (SPENT until the button is released) -> IDLE ---- */
enum { PH_IDLE, PH_UP, PH_FULL, PH_DOWN, PH_SPENT };
static void rmgr_log(const char *tag);   /* v2.10 diagnostics, defined below */
static int g_phase;
static unsigned g_cycle_t0;
static float g_f, g_fc, g_f_down0, g_f_written = -1.0f, g_k_written = -1.0f;   /* g_fc = charge cycle, g_f = max(g_fc, spam) */
static unsigned g_t_phase, g_t_write, g_t_tick;
static volatile int g_pulse;                       /* 1 while a tease pulse is at its tight part */
static unsigned g_pulse_t0, g_pulse_next;
static int g_pulse_run;
static unsigned g_pulse_n;
static uint32_t g_des_ent, g_des_last;                /* the enemy chosen to attack in the current pulse */
static unsigned g_des_t, g_des_pn, g_des_last_t;
static int g_des_count;
static unsigned g_down_ms = 2500;                  /* how fast the ring closes in the DOWN phase */

static void set_phase(int ph, unsigned now)
{
    static const char *nm[] = { "IDLE", "UP", "FULL", "DOWN", "SPENT" };
    if (ph == PH_IDLE && g_phase != PH_IDLE)
        push_text("CYCLE END dur=%ums allowed=%d blocked=%d near_allowed=%d enemies=%d", now - g_cycle_t0, g_stat_allowed, g_stat_blocked, g_stat_near, g_nenemies);
    g_phase = ph; g_t_phase = now;
    push_text("SURROUND phase=%s f=%.2f", nm[ph], g_fc);
}

static uint32_t  g_rng = 2463534242u;

static unsigned rnd_between(unsigned lo, unsigned hi)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return hi <= lo ? lo : lo + g_rng % (hi - lo + 1);
}

static void fight_rating_tick(unsigned now)
{
    static int active; static unsigned t0, t_empty, as_s0, as_o0, k0, h0, r0, hd0, f0; static int maxn;
    int n = g_nenemies;
    if (!active) {
        if (n >= 1) { active = 1; t0 = now; t_empty = 0; maxn = n; as_s0 = g_mx_as_s; as_o0 = g_mx_as_o; k0 = g_fr_kills; h0 = g_fr_hits; r0 = g_fr_rolls; hd0 = g_fr_holds; f0 = g_fr_forced; push_text("FIGHT start: %d enemies", n); }
        return;
    }
    if (n > maxn) maxn = n;
    if (n >= 1) { t_empty = 0; return; }
    if (!t_empty) { t_empty = now | 1u; return; }
    if (age_ms(now, t_empty) < 4000u) return;
    active = 0;
    {
        unsigned dur = t_empty - t0, kills = g_fr_kills - k0, hits = g_fr_hits - h0, ats = g_mx_as_s - as_s0, ato = g_mx_as_o - as_o0, rolls = g_fr_rolls - r0, holds = g_fr_holds - hd0, forced = g_fr_forced - f0;
        float sec = (float)dur / 1000.0f;
        if (sec < 5.0f) return;
        push_text("FIGHT-SUMMARY dur=%.0fs max_enemies=%d kills=%u hits_dealt=%u enemy_attacks=%u (shield %u, soldier %u) per_10s=%.1f rolls=%u charged_holds=%u forced_swings=%u hits_per_kill=%.1f", sec, maxn, kills, hits, ats + ato, ats, ato, 10.0f * (float)(ats + ato) / sec, rolls, holds, forced, kills ? (float)hits / (float)kills : 0.0f);
        /* v3.10: the revived_fights.csv file is no longer written (the FIGHT-SUMMARY line in the trace stays) */
    }
}
static void surround_tick(unsigned now)
{
    int lmb = (g_lmb_since != 0 || g_always) && g_cfg.patterns;
    int held = lmb && (g_always || age_ms(now, g_lmb_since) >= (unsigned)g_cfg.hold_ms);
    int i;
    unsigned dt = g_t_tick ? now - g_t_tick : 10u;
    g_t_tick = now;
    if (dt > 200u) dt = 200u;

    /* number of enemies -> ring scale (smoothed, 0.4 per second at most) and logging */
    {
        static int last_n = -1;
        int n = enemy_count(now);
        float want = 1.0f, step = 0.3f * (float)dt / 1000.0f;
        g_nenemies = n;
        g_crowd = (g_cfg.crowd_on && n >= g_cfg.crowd_min) ? 1 : 0;
        if (g_cfg.by_count) {
            float lo = (float)g_cfg.pct_lo / 100.0f, hi = (float)g_cfg.pct_hi / 100.0f;
            float u = (float)(n - g_cfg.cnt_lo) / (float)(g_cfg.cnt_hi - g_cfg.cnt_lo);
            if (u < 0) u = 0;
            if (u > 1) u = 1;
            want = lo + (hi - lo) * u;
        }
        if (g_cnt_k < want) g_cnt_k = g_cnt_k + step > want ? want : g_cnt_k + step;
        else if (g_cnt_k > want) g_cnt_k = g_cnt_k - step < want ? want : g_cnt_k - step;
        if (n != last_n) { last_n = n; push_text("ENEMIES n=%d ring_scale_target=%.2f", n, want); }
    }

    /* ring factor g_fs: spam of left clicks and the retreat after a missed attack */
    {
        unsigned chain = g_chain, lp = g_last_press;
        float target = 0.0f, tspam = 0.0f, tret = 0.0f;
        int upms = g_cfg.spam_up;
        int calm = g_phase != PH_UP && g_phase != PH_FULL;
        if (g_cfg.spam_on && g_cfg.patterns && lp && chain >= (unsigned)g_cfg.spam_from && age_ms(now, lp) < (unsigned)g_cfg.spam_hold && calm) {
            int pct = g_cfg.spam_pct + g_cfg.spam_step * (int)(chain - (unsigned)g_cfg.spam_from);
            tspam = (float)(pct > 100 ? 100 : pct) / 100.0f;
        }
        if (g_cfg.retreat_on && g_cfg.patterns && calm && (int)(g_retreat_until - now) > 0) tret = (float)g_cfg.retreat_pct / 100.0f;
        target = tspam > tret ? tspam : tret;
        if (tret > tspam) upms = g_cfg.retreat_up;
        if (target > g_fs) {
            if (g_fs == 0.0f && tspam >= tret) push_text("SPAM reaction chain=%u target=%.2f", chain, target);
            g_fs += (float)dt / (float)upms;
            if (g_fs > target) g_fs = target;
        } else if (target < g_fs) {
            g_fs -= (float)dt / (float)g_cfg.ramp_down_ms;
            if (g_fs < target) g_fs = target;
        }
    }
    {
        float tt = (g_cfg.roll_on && g_cfg.patterns && (int)(g_roll_until - now) > 0) ? 1.0f : 0.0f;
        if (tt > g_ft) { g_ft += (float)dt / 300.0f; if (g_ft > tt) g_ft = tt; }
        else if (tt < g_ft) { g_ft -= (float)dt / (float)g_cfg.ramp_down_ms; if (g_ft < tt) g_ft = tt; }
    }

    /* v2.75: (a) the Prince attacks and then backs off: the ring closes in on him; (b) an enemy has just died: the ring closes in and the limit on simultaneous attacks is lifted for a moment */
    {
        static float dh[16]; static unsigned th[16]; static int hn; static uint32_t killed[8]; static int kn;
        float dm = 1.0e9f, old = -1.0f; int q, fresh = 0;
        for (q = 0; q < NSEEN; q++) if (g_seen[q].ent && age_ms(now, g_seen[q].t) <= 700u && g_seen[q].d >= 0 && g_seen[q].d < dm) dm = g_seen[q].d;
        if (!hn || age_ms(now, th[(hn - 1) & 15]) >= 100u) { dh[hn & 15] = dm; th[hn & 15] = now | 1u; hn++; }
        for (q = hn - 1; q >= 0 && q > hn - 16; q--) { unsigned a = age_ms(now, th[q & 15]); if (a >= 400u && a <= 800u) { old = dh[q & 15]; break; } }
        if (g_pr_on && g_cfg.patterns && !g_wide && g_last_press && age_ms(now, g_last_press) < 2000u && dm >= 1.5f && dm <= 8.0f && old >= 0.0f && old < 1.0e8f
            && (dm - old) * 10.0f >= (float)g_pr_rise10 && (int)(g_press_until - now) <= 0) {
            g_press_until = now + (unsigned)g_pr_ms; g_pun_budget = 1; g_pun_from = 0; g_pun_dir = 0; g_pun_until = now + (unsigned)g_pr_ms;
            push_text("PRESSURE: the Prince backs off (nearest %.1f -> %.1f), ring closes in for %d ms", old, dm, g_pr_ms);
        }
        for (q = 0; q < NSEEN; q++) if (g_seen[q].ent && age_ms(now, g_seen[q].t) <= 300u) fresh++;
        if (g_kl_on && fresh >= 1) for (q = 0; q < NSEEN; q++) {
            unsigned a; int k2, dup = 0;
            uint32_t e = g_seen[q].ent;
            if (!e) continue;
            a = age_ms(now, g_seen[q].t);
            if (a < 600u || a > 1500u) continue;
            if (!recent_hit_ent(e, 3000u)) continue;
            for (k2 = 0; k2 < 8; k2++) if (killed[k2] == e) dup = 1;
            if (dup) continue;
            killed[kn++ & 7] = e;
            g_kill_until = now + (unsigned)g_kl_ms; g_pun_budget = 2; g_pun_from = 0; g_pun_dir = 0; g_pun_until = now + (unsigned)g_kl_ms;
            g_fr_kills++;
            push_text("KILL: enemy %08lX is gone, the ring closes in and the attack limit is lifted for %d ms", (unsigned long)e, g_kl_ms);
        }
        {
            int kact = g_kl_on && (int)(g_kill_until - now) > 0, pact = (int)(g_press_until - now) > 0 && !g_wide;
            float tq = (kact || pact) && !g_wide ? 1.0f : 0.0f;
            if (tq > 0.0f) g_fpr_pct = kact ? (float)g_kl_pct : (float)g_pr_pct;
            if (tq > g_fpr) { g_fpr += (float)dt / 300.0f; if (g_fpr > tq) g_fpr = tq; }
            else if (tq < g_fpr) { g_fpr -= (float)dt / (float)g_cfg.ramp_down_ms; if (g_fpr < tq) g_fpr = tq; }
        }
    }

    {
        static unsigned t_stat;
        if (age_ms(now, t_stat) >= 5000u) {
            if (t_stat && g_nenemies > 0)
                push_text("STAT5 enemies=%d asks=%d ring_scale=%.2f f=%.2f fs=%.2f ph=%d chain=%u flank_calls=%u melee_forced=%u mx_ring_block=%u key_miss=%u", g_nenemies, g_stat_asks, g_cnt_k, g_f, g_fs, g_phase, g_chain, g_fl_calls, g_ml_forced, g_mx_ring_block, g_mx_key_miss);
            g_stat_asks = 0; t_stat = now;
        }
    }

    fight_rating_tick(now);
    if (!g_nhits) return;
    g_pd = 0.0f;
    switch (g_phase) {
    case PH_IDLE:
        if (held && g_nenemies >= g_cfg.min_enemies && age_ms(now, g_rel_t) >= (unsigned)g_cfg.cool_ms) { g_cycle_t0 = now; g_stat_allowed = 0; g_stat_blocked = 0; g_stat_near = 0; set_phase(PH_UP, now); }
        break;
    case PH_UP:
        g_fc = (float)(now - g_t_phase) / (float)g_cfg.ramp_up_ms;
        if (!lmb || g_nenemies < g_cfg.min_enemies) { g_f_down0 = g_fc > 1 ? 1 : g_fc; g_down_ms = (!lmb && g_nenemies >= g_cfg.min_enemies) ? (unsigned)g_cfg.release_ms : (unsigned)g_cfg.ramp_down_ms; set_phase(PH_DOWN, now); }
        else if (g_fc >= 1.0f) { g_fc = 1.0f; g_pulse_next = 0; g_pulse_run = 0; set_phase(PH_FULL, now); }
        break;
    case PH_FULL:
        g_fc = 1.0f;
        g_pulse = 0;
        if (g_cfg.pulse_on) {
            unsigned span = (unsigned)(g_cfg.ramp_up_ms + g_cfg.full_ms);
            float prog = span ? (float)age_ms(now, g_cycle_t0) / (float)span : 1.0f;
            float kk = 1.0f - (1.0f - (float)g_cfg.pulse_min_gap_pct / 100.0f) * (prog > 1.0f ? 1.0f : prog);
            if (!g_pulse_next) g_pulse_next = now + rnd_between((unsigned)g_cfg.pulse_first * 8u / 10u / ((g_mixed_on && g_n_shield > 0 && g_n_other > 0) ? 2u : 1u), (unsigned)g_cfg.pulse_first * 12u / 10u / ((g_mixed_on && g_n_shield > 0 && g_n_other > 0) ? 2u : 1u)) + 1u;
            if (!g_pulse_run && (int)(now - g_pulse_next) >= 0) { g_pulse_run = 1; g_pulse_n++; g_pulse_t0 = now; push_text("TEASE pulse start (held %ums)", age_ms(now, g_cycle_t0)); rmgr_log("pulse"); if (g_evt_on && g_n_shield > 0) { g_evt_until = (now + g_evt_ms) | 1u; g_evt_leave = 1; g_evt_n++; push_text("EVTEST #%u start: soldiers leave their rings for %ums (shield=%d others=%d)", g_evt_n, g_evt_ms, g_n_shield, g_n_other); } }
            if (g_pulse_run) {
                unsigned e = age_ms(now, g_pulse_t0), pm = (unsigned)g_cfg.pulse_ms;
                if (g_mixed_on && g_n_shield > 0 && g_n_other > 0 && g_des_ent && age_ms(now, g_des_t) < (unsigned)g_cfg.des_to * 2u && e + 600u >= pm) pm = e + 600u;   /* v2.58: the chosen shield-bearer is still walking in: the pulse does not end under his feet */
                float low = (float)g_cfg.pulse_pct / 100.0f, sh;
                if (e >= pm) {
                    int tp = 100;      /* v2.87: pace by make-up: a small group or shield-bearers only come back faster, a big soldier crowd slower (fewer at once) */
                    if (g_tp_on) {
                        if (g_n_shield > 0 && g_n_other == 0) tp = g_tp_shield_pct;
                        else if (g_n_shield + g_n_other <= 2) tp = g_tp_small_pct;
                        else if (g_n_shield == 0 && g_n_other >= g_tp_crowd_n) tp = g_tp_crowd_pct;
                    }
                    g_pulse_run = 0;
                    g_pulse_next = now + (unsigned)((float)rnd_between((unsigned)g_cfg.pulse_gap_min, (unsigned)g_cfg.pulse_gap_max) * kk * (float)tp / 100.0f) + 1u;
                    if (tp != 100) push_text("TEMPO gap x%d%% (shield=%d others=%d)", tp, g_n_shield, g_n_other);
                }
                else {
                    sh = e < 400u ? (float)e / 400.0f : (e > pm - 500u ? (float)(pm - e) / 500.0f : 1.0f);
                    if (sh < 0) sh = 0;
                    if (sh > 1) sh = 1;
                    if (g_cfg.pr_mode) g_pd = sh; else g_fc = 1.0f - (1.0f - low) * sh;
                    g_pulse = sh >= 0.5f;
                }
            }
        }
        if (!lmb || g_nenemies < g_cfg.min_enemies || (g_cfg.max_hold_ms > 0 && (now - g_t_phase) >= (unsigned)g_cfg.max_hold_ms)) { g_f_down0 = 1.0f; g_down_ms = (!lmb && g_nenemies >= g_cfg.min_enemies) ? (unsigned)g_cfg.release_ms : (unsigned)g_cfg.ramp_down_ms; set_phase(PH_DOWN, now); }
        break;
    case PH_DOWN:
        g_fc = g_f_down0 * (1.0f - (float)(now - g_t_phase) / (float)g_down_ms);
        /* v2.74: a NEW charge during the closing phase starts the retreat again from the current ring (before, it fell through to SPENT and the crowd rushed in) */
        if (held && !g_always && g_nenemies >= g_cfg.min_enemies && g_lmb_since && age_ms(now, g_lmb_since) < age_ms(now, g_t_phase)) {
            float cur = g_fc < 0.0f ? 0.0f : (g_fc > 1.0f ? 1.0f : g_fc);
            g_fc = cur;
            g_cycle_t0 = now;
            set_phase(PH_UP, now);
            g_t_phase = now - (unsigned)(cur * (float)g_cfg.ramp_up_ms);      /* ramp_up continues from the current width */
            push_text("RECHARGE during DOWN: retreat resumes from f=%.2f", cur);
            break;
        }
        if (g_fc <= 0.0f) { g_fc = 0.0f; set_phase((lmb && !g_always) ? PH_SPENT : PH_IDLE, now); }   /* v2.29: with always_on the button counts as held for ever, so SPENT would never end */
        break;
    default:
        if (!lmb) set_phase(PH_IDLE, now);
        break;
    }
    if (g_phase != PH_FULL) { g_pulse_run = 0; g_pulse = 0; }
    g_wide = g_phase == PH_UP || g_phase == PH_FULL || g_phase == PH_DOWN;
    g_f = g_fc > g_fs ? g_fc : g_fs;
    if (g_base_on && g_nenemies >= g_cfg.min_enemies && !g_pulse_run && g_phase != PH_UP && g_phase != PH_FULL) { float b = (float)g_base_pct / 100.0f; if (g_f < b) g_f = b; }   /* v2.31: slightly wider ring at rest; attacks are not blocked */
    shield_ring_track(now);
    if (((fabsf(g_f - g_f_written) >= 0.04f || (g_f != g_f_written && (g_f == 0.0f || g_f == 1.0f))) || g_crowd != g_crowd_written || fabsf(g_pd - g_pd_written) >= 0.04f || (g_pd == 0.0f && g_pd_written != 0.0f) || g_ft != g_ft_written || g_fpr != g_fpr_written || (g_f > 0.0f && fabsf(g_cnt_k - g_k_written) > 0.02f)) && (now - g_t_write >= 100u || g_f == 0.0f || g_f == 1.0f)) {
        for (i = 0; i < g_nhits; i++) write_hit(&g_hits[i], g_f);
        g_f_written = g_f; g_ft_written = g_ft; g_fpr_written = g_fpr; g_k_written = g_cnt_k; g_pd_written = g_pd; g_crowd_written = g_crowd; g_t_write = now;
        g_relayout = 1;
    }
}

/* The game's fight-ring manager (global pointer at 0x11A53DC) lays the slots out with the ring settings;
 * 0x59A1E0(manager, 0) repeats that layout. It is called from the game's own AI thread (inside a hook). */
typedef void (__attribute__((stdcall)) *LayoutFn)(void *mgr, int flag);
static void relayout_now(void)
{
    void *mgr = NULL;
    uint32_t cnt = 0, setup = 0;
    HANDLE me = GetCurrentProcess();
    g_relayout = 0;
    if (!ReadProcessMemory(me, (void *)0x11A53DC, &mgr, 4, NULL) || !mgr) { push_text("RELAYOUT skipped: no manager"); return; }
    if (!ReadProcessMemory(me, (uint8_t *)mgr + 0x44, &cnt, 4, NULL) || !ReadProcessMemory(me, (uint8_t *)mgr + 0x3c, &setup, 4, NULL)) { push_text("RELAYOUT skipped: manager unreadable"); return; }
    if ((cnt & 0x3fff) == 0 || !setup) { push_text("RELAYOUT skipped: no ring entries (cnt=%u setup=%08X)", cnt & 0x3fff, setup); return; }
    ((LayoutFn)(uintptr_t)0x59A1E0)(mgr, 0);
    { static unsigned lastlog; unsigned t = now_ms(); if (t - lastlog > 1000u) { lastlog = t; { const char *nm = "?"; int q; for (q = 0; q < g_nhits; q++) if ((uint32_t)(uintptr_t)g_hits[q].p - 0x10u == setup) nm = g_typename[g_hits[q].type]; push_text("RELAYOUT done mgr=%p entries=%u setup=%08X (%s) f=%.2f", mgr, cnt & 0x3fff, setup, nm, g_f); } } }
}


/* Who attacks while the ring is wide.
 *  - first punish_ms of the cycle: no blocking at all (enemies that are already close to the Prince hit the one who starts charging);
 *  - afterwards at most N enemies may start an attack at once (N = 1, 2 after second_ms of the cycle, 3 while the ring shrinks),
 *    each new attack comes after a random pause that shrinks with the time spent holding (down to esc_min_pct percent);
 *  - among the enemies that are asking right now the one that attacked longest ago goes first. */
#define NASK 32
typedef struct { uint32_t ent; unsigned last_ask, last_attack; } Asker;
static Asker g_ask[NASK];
static unsigned g_next_allowed;
typedef struct { uint32_t ent; unsigned t; } Grant;
static Grant g_grants[4];
static uint32_t g_near_ent;
static unsigned g_near_t;

static uint8_t hold_attack_gate(uint32_t ent, unsigned now)
{
    int i, me = -1, free_i = -1, active = 0, maxatk;
    unsigned cyc = now - g_cycle_t0, span = (unsigned)(g_cfg.ramp_up_ms + g_cfg.full_ms);
    float prog = span ? (float)cyc / (float)span : 1.0f;
    Asker *best = NULL;
    if (g_mixed_on && g_n_shield > 0 && g_n_other > 0 && ent_kind(ent) != 2 && !(seen_get_dist(ent) >= 0 && seen_get_dist(ent) <= g_mx_free)) {   /* v2.56: a soldier who is already next to the Prince may attack; v2.53 mixed crowd: soldiers never expose themselves, shield-bearers take the blow */
        static unsigned n_mx, t_mx;
        n_mx++; g_mx_refused++; if (age_ms(now, t_mx) >= 2000u) { t_mx = now | 1u; push_text("MIXED soldier attacks refused %u so far (shield=%d others=%d)", n_mx, g_n_shield, g_n_other); }
        return 0;
    }
    if (cyc < (unsigned)g_cfg.punish_ms) return 1;
    if (g_des_ent && ent == g_des_ent) return 1;       /* the chosen attacker */
    if (g_phase == PH_DOWN && g_release_open) return 1;   /* the Prince has just let go of the charge: everybody may attack (v2.57: only if [patterns] release_open=1) */
    {
        float dn = seen_get_dist(ent);
        if (g_cfg.near_dist > 0 && dn >= 0 && dn < g_cfg.near_dist && !(g_cfg.pulse_on && g_pulse_run)) {   /* already close: tries to stop the charge, but not all at once */
            if (g_cfg.near_gap <= 0 || (g_near_ent == ent && age_ms(now, g_near_t) < 1500u) || age_ms(now, g_near_t) >= (unsigned)g_cfg.near_gap) {
                if (g_near_ent != ent || age_ms(now, g_near_t) >= 1500u) { g_near_ent = ent; g_near_t = now | 1u; }
                g_stat_near++;
                return 1;
            }
            return 0;
        }
        if (g_cfg.pulse_on && (g_cfg.des_on || !g_pulse)) return 0;   /* far ones attack only during a pulse (or only when chosen) */
    }
    for (i = 0; i < NASK; i++) {
        if (g_ask[i].ent == ent) me = i;
        else if (free_i < 0 && (g_ask[i].ent == 0 || now - g_ask[i].last_ask > 5000u)) free_i = i;
    }
    if (me < 0) {
        me = free_i >= 0 ? free_i : 0;
        g_ask[me].ent = ent; g_ask[me].last_attack = 0;
    }
    g_ask[me].last_ask = now;
    for (i = 0; i < 4; i++) {
        if (g_grants[i].ent && now - g_grants[i].t < 1500u) {
            active++;
            if (g_grants[i].ent == ent) return 1;                                  /* attack already granted */
        }
    }
    maxatk = 1 + (cyc >= (unsigned)g_cfg.second_ms ? 1 : 0) + (cyc >= (unsigned)g_cfg.third_ms ? 1 : 0);
    if (g_cfg.per_attacker > 0) {                       /* few enemies: fewer simultaneous attackers, a big crowd: more */
        int cap = 1 + g_nenemies / g_cfg.per_attacker;
        if (maxatk > cap) maxatk = cap;
    }
    if (g_cfg.atk_max <= 0 || active >= maxatk || (!g_cfg.pulse_on && now < g_next_allowed)) return 0;
    for (i = 0; i < NASK; i++) {
        Asker *a = &g_ask[i];
        if (a->ent == 0 || now - a->last_ask > 400u) continue;
        if (!best || a->last_attack < best->last_attack) best = a;
    }
    if (best != &g_ask[me]) return 0;
    g_ask[me].last_attack = now;
    for (i = 0; i < 4; i++) if (!g_grants[i].ent || now - g_grants[i].t >= 1500u) break;
    if (i == 4) i = 0;
    g_grants[i].ent = ent; g_grants[i].t = now;
    {
        float k = 1.0f - (1.0f - (float)g_cfg.esc_min_pct / 100.0f) * (prog > 1.0f ? 1.0f : prog);
        unsigned lo, hi;
        if (g_nenemies <= 1) k *= (float)g_cfg.last_pct / 100.0f;      /* the last enemy: shorter pauses */
        lo = (unsigned)((float)g_cfg.atk_min * k); hi = (unsigned)((float)g_cfg.atk_max * k);
        g_next_allowed = now + rnd_between(lo, hi);
    }
    g_stat_allowed++;
    push_text("HOLD-ATTACK allowed ent=%08lX active=%d/%d cycle=%ums next_in=%ums", (unsigned long)ent, active + 1, maxatk, cyc, g_next_allowed - now);
    return 1;
}

/* ---- hooks on the AI conditions ---- */
typedef uint8_t (THISCALL *EvalFn)(void *self, void *ent);
static EvalFn o_FA, o_FR, o_CB, o_MM, o_TQ, o_TD;

static int probe_dist(void *ent, float *raw);
typedef struct { uint32_t ent; uint8_t was; } Att;
static Att g_att[48];

/* ---- v2.10 diagnostics (no change of behaviour). Everything is written to revived_trace.txt:
 *  SLOT/SLOTD/SLOTP   slot object of an enemy (entity+0x30) and the objects it points to, read AFTER the game answered a ring question (the slot is made lazily)
 *  POSCAND            where the enemy's own position (x,y,z) lies in memory: float triples whose distance to the Prince equals the measured distance
 *  RMGR/RMGRP         the ring manager (global 0x11A53DC) and objects it points to
 *  SETUP              the ring setup structure of each enemy type
 *  WATCH              for the chosen attacker and two other enemies, 3.5 s: distance every 150 ms and every change of an answer to MoveMode, TargetDistance, RingQuery, TargetQuery, FightAttack
 *  MAP                every map_ms while the left mouse is held: all enemies and their distances */
static void hex_line(char *out, const uint8_t *b, int n)
{
    static const char *H = "0123456789ABCDEF"; int k;
    for (k = 0; k < n; k++) { out[2 * k] = H[b[k] >> 4]; out[2 * k + 1] = H[b[k] & 15]; }
    out[2 * n] = 0;
}
static int looks_ptr(uint32_t v) { return v >= 0x01700000u && v < 0x7FFF0000u && !(v & 3u); }

static void slot_log(void *ent, const char *tag)
{
    static int nlog;
    HANDLE me = GetCurrentProcess();
    uint32_t s = 0, off;
    uint8_t buf[0x120];
    int i, cnt = 0;
    char hex[100];
    if (!g_cfg.diag || nlog >= 600) return;
    nlog++;
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x30, &s, 4, NULL) || !s) { push_text("SLOT %s ent=%08lX: no slot object", tag, (unsigned long)(uintptr_t)ent); return; }
    push_text("SLOT %s ent=%08lX slot=%08lX dist=%.2f", tag, (unsigned long)(uintptr_t)ent, (unsigned long)s, seen_get_dist((uint32_t)(uintptr_t)ent));
    memset(buf, 0, sizeof buf);
    ReadProcessMemory(me, (void *)(uintptr_t)s, buf, sizeof buf, NULL);
    for (i = 0; i < 6; i++) { hex_line(hex, buf + i * 48, 48); push_text("SLOTD %02X %s", i * 48, hex); }
    for (off = 0; off < 0x60 && cnt < 8; off += 4) {
        uint32_t pv = *(uint32_t *)(buf + off);
        uint8_t pb[0x40];
        if (!looks_ptr(pv) || pv == s) continue;
        memset(pb, 0, sizeof pb);
        if (!ReadProcessMemory(me, (void *)(uintptr_t)pv, pb, sizeof pb, NULL)) continue;
        hex_line(hex, pb, 48);
        push_text("SLOTP +%02X ptr=%08lX %s", off, (unsigned long)pv, hex);
        cnt++;
    }
}

/* the Prince's position: the same chain that prince_tick uses (entity+0x38 -> +0xC -> target ref -> target+0x40) */
static int prince_pos(void *ent, float pos[3])
{
    HANDLE me = GetCurrentProcess();
    uint32_t obj = 0, sref = 0, fl = 0, tgt = 0;
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x38, &obj, 4, NULL) || !obj) return 0;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)obj + 0xC, &sref, 4, NULL) || !sref) return 0;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)sref + 8, &fl, 4, NULL) || !(fl & 0x10000000u)) return 0;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)sref, &tgt, 4, NULL) || !tgt) return 0;
    return ReadProcessMemory(me, (uint8_t *)(uintptr_t)tgt + 0x40, pos, 12, NULL) ? 1 : 0;
}
static int scan_pos(const uint8_t *b, size_t n, const char *src, uint32_t ent, const float *P, float want, float tol, int *found)
{
    size_t o;
    for (o = 0; o + 12 <= n && *found < 10; o += 4) {
        float x, y, z, d3, dxy;
        memcpy(&x, b + o, 4); memcpy(&y, b + o + 4, 4); memcpy(&z, b + o + 8, 4);
        if (!(x > -5000.0f && x < 5000.0f && y > -5000.0f && y < 5000.0f && z > -5000.0f && z < 5000.0f)) continue;
        if (fabsf(x - P[0]) > 40.0f || fabsf(y - P[1]) > 40.0f || fabsf(z - P[2]) > 40.0f) continue;
        d3 = sqrtf((x - P[0]) * (x - P[0]) + (y - P[1]) * (y - P[1]) + (z - P[2]) * (z - P[2]));
        dxy = sqrtf((x - P[0]) * (x - P[0]) + (y - P[1]) * (y - P[1]));
        if (d3 < 0.05f) continue;                                    /* that is the Prince himself */
        if (fabsf(d3 - want) <= tol || fabsf(dxy - want) <= tol) {
            push_text("POSCAND ent=%08lX %s+%03X (%.2f,%.2f,%.2f) d3=%.2f dxy=%.2f want=%.2f", (unsigned long)ent, src, (unsigned)o, x, y, z, d3, dxy, want);
            (*found)++;
        }
    }
    return *found;
}
/* live = called from a game thread (the distance can be measured now); otherwise the last measured distance is used with a wider tolerance */
static void poscand(void *ent, int live)
{
    static int ncalls;
    HANDLE me = GetCurrentProcess();
    uint8_t eb[0x200], ab[0x4F8], ob[0x200], pb[0x180];
    uint32_t obj = 0, v;
    float P[3], raw = 0, want, tol;
    int found = 0, np = 0;
    size_t o;
    char src[24];
    if (!g_cfg.diag || ncalls >= 60) return;
    ncalls++;
    if (!prince_pos(ent, P)) { push_text("POSCAND ent=%08lX: no Prince position", (unsigned long)(uintptr_t)ent); return; }
    if (live && probe_dist(ent, &raw) && raw > 0) { want = sqrtf(raw); tol = 0.08f; }
    else { want = seen_get_dist((uint32_t)(uintptr_t)ent); tol = 0.6f; }
    if (want <= 0) return;
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x38, &obj, 4, NULL) || !obj) return;
    memset(eb, 0, sizeof eb); memset(ab, 0, sizeof ab); memset(ob, 0, sizeof ob);
    ReadProcessMemory(me, ent, eb, sizeof eb, NULL);
    ReadProcessMemory(me, (void *)(uintptr_t)(obj - 0x4F8u), ab, sizeof ab, NULL);
    ReadProcessMemory(me, (void *)(uintptr_t)obj, ob, sizeof ob, NULL);
    push_text("POSCAND ent=%08lX prince=(%.2f,%.2f,%.2f) want=%.2f tol=%.2f", (unsigned long)(uintptr_t)ent, P[0], P[1], P[2], want, tol);
    scan_pos(eb, sizeof eb, "ENT", (uint32_t)(uintptr_t)ent, P, want, tol, &found);
    scan_pos(ab, sizeof ab, "ACT", (uint32_t)(uintptr_t)ent, P, want, tol, &found);
    scan_pos(ob, sizeof ob, "OBJ", (uint32_t)(uintptr_t)ent, P, want, tol, &found);
    {   /* one level of pointers from the three blocks */
        const uint8_t *bl[3] = { eb, ab, ob }; const size_t sz[3] = { sizeof eb, sizeof ab, sizeof ob }; const char *nm[3] = { "ENT", "ACT", "OBJ" };
        int k;
        for (k = 0; k < 3 && np < 150 && found < 10; k++)
            for (o = 0; o + 4 <= sz[k] && np < 150 && found < 10; o += 4) {
                memcpy(&v, bl[k] + o, 4);
                if (!looks_ptr(v)) continue;
                memset(pb, 0, sizeof pb);
                if (!ReadProcessMemory(me, (void *)(uintptr_t)v, pb, sizeof pb, NULL)) continue;
                np++;
                _snprintf(src, sizeof src, "%s@%03X>", nm[k], (unsigned)o);
                src[sizeof src - 1] = 0;
                scan_pos(pb, sizeof pb, src, (uint32_t)(uintptr_t)ent, P, want, tol, &found);
            }
    }
    push_text("POSCAND ent=%08lX done: %d candidates, %d pointers checked", (unsigned long)(uintptr_t)ent, found, np);
}

static void rmgr_log(const char *tag)
{
    static int n;
    HANDLE me = GetCurrentProcess();
    uint32_t m = 0, off;
    uint8_t buf[0x120];
    char hex[100];
    int i, cnt = 0;
    if (!g_cfg.diag || n >= 20) return;
    n++;
    if (!ReadProcessMemory(me, (void *)0x11A53DC, &m, 4, NULL) || !m) { push_text("RMGR %s: no manager", tag); return; }
    push_text("RMGR %s mgr=%08lX enemies=%d", tag, (unsigned long)m, g_nenemies);
    memset(buf, 0, sizeof buf);
    ReadProcessMemory(me, (void *)(uintptr_t)m, buf, sizeof buf, NULL);
    for (i = 0; i < 6; i++) { hex_line(hex, buf + i * 48, 48); push_text("RMGRD %02X %s", i * 48, hex); }
    for (off = 0; off < 0x80 && cnt < 8; off += 4) {
        uint32_t pv = *(uint32_t *)(buf + off);
        uint8_t pb[0x40];
        if (!looks_ptr(pv)) continue;
        memset(pb, 0, sizeof pb);
        if (!ReadProcessMemory(me, (void *)(uintptr_t)pv, pb, sizeof pb, NULL)) continue;
        hex_line(hex, pb, 48);
        push_text("RMGRP +%02X ptr=%08lX %s", off, (unsigned long)pv, hex);
        cnt++;
    }
}
static void setup_log(void)
{
    static int n;
    HANDLE me = GetCurrentProcess();
    int t, i, k;
    if (!g_cfg.diag || n >= 4) return;
    n++;
    for (t = 0; t < NTYPES; t++)
        for (i = 0; i < g_nhits; i++) {
            uint8_t buf[0xC0]; char hex[100];
            if (g_hits[i].type != t) continue;
            memset(buf, 0, sizeof buf);
            ReadProcessMemory(me, (uint8_t *)g_hits[i].p - 0x10, buf, sizeof buf, NULL);
            push_text("SETUP %s at=%p", g_typename[t], (void *)((uint8_t *)g_hits[i].p - 0x10));
            for (k = 0; k < 4; k++) { hex_line(hex, buf + k * 48, 48); push_text("SETUPD %02X %s", k * 48, hex); }
            break;
        }
}

typedef struct { uint32_t ent; unsigned t, dlog; int n, slot_i, pos_done, role, slotlog; uint32_t key[5][16]; int8_t last[5][16]; } Watch;
static Watch g_w[3];
static int g_slot_pending, g_slot_events, g_pos_events;
static unsigned g_map_t;

static Watch *watch_find(void *ent)
{
    int i; uint32_t e = (uint32_t)(uintptr_t)ent;
    for (i = 0; i < 3; i++) if (g_w[i].ent == e) return &g_w[i];
    return NULL;
}
static void watch_start(void *ent, unsigned now)
{
    if (!g_cfg.diag) return;
    memset(g_w, 0, sizeof g_w);
    g_w[0].ent = (uint32_t)(uintptr_t)ent; g_w[0].t = now | 1u; g_w[0].role = 0;
    g_w[0].slotlog = (g_slot_events < g_cfg.slot_events);
    if (g_w[0].slotlog) g_slot_events++;
    g_slot_pending = 2;
}
static void watch_other(void *ent, unsigned now)
{
    int i;
    if (!g_cfg.diag || g_slot_pending <= 0 || !g_w[0].ent || watch_find(ent)) return;
    for (i = 1; i < 3; i++)
        if (!g_w[i].ent) {
            g_w[i].ent = (uint32_t)(uintptr_t)ent; g_w[i].t = now | 1u; g_w[i].role = 1; g_w[i].slotlog = g_w[0].slotlog;
            g_slot_pending--;
            return;
        }
}
static void watch_tick(void *ent, unsigned now)
{
    Watch *w; unsigned age;
    if (!g_cfg.diag || !(w = watch_find(ent))) return;
    age = age_ms(now, w->t);
    if (age > 3500u) { w->ent = 0; return; }
    if (age_ms(now, w->dlog) >= 150u) {
        float raw; w->dlog = now | 1u;
        if (probe_dist(ent, &raw) && raw > 0 && w->n < 80) { w->n++; push_text("WATCH DIST %s ent=%08lX +%ums d=%.2f", w->role ? "other" : "chosen", (unsigned long)(uintptr_t)ent, age, sqrtf(raw)); }
    }
}
/* called from the RingQuery hook AFTER the game answered, so the slot object exists */
static void watch_fr_post(void *ent, unsigned now)
{
    static const unsigned T[4] = { 0u, 500u, 1000u, 2000u };
    Watch *w; unsigned age; char tag[32];
    if (!g_cfg.diag || !(w = watch_find(ent)) || !w->slotlog) return;
    age = age_ms(now, w->t);
    if (w->slot_i < 4 && age >= T[w->slot_i]) {
        _snprintf(tag, sizeof tag, "%s +%ums", w->role ? "other" : "chosen", age); tag[sizeof tag - 1] = 0;
        slot_log(ent, tag);
        w->slot_i++;
    }
    if (!w->pos_done && g_pos_events < 8) { w->pos_done = 1; g_pos_events++; poscand(ent, 1); }
}
/* kind: 0 MoveMode, 1 TargetDistance, 2 RingQuery, 3 TargetQuery, 4 FightAttack. An answer is written only when it changes */
static void watch_answer(int kind, void *ent, void *self, uint8_t r)
{
    static const char *K[5] = { "MM", "TD", "FR", "TQ", "FA" };
    Watch *w; uint32_t param, a = 0, key; float b = 0; int j, slot = -1;
    if (!g_cfg.diag || !(w = watch_find(ent))) return;
    param = *(uint32_t *)((uint8_t *)self + 8);
    key = param;
    if (kind == 1) { a = *(uint32_t *)((uint8_t *)self + 0xC); b = *(float *)((uint8_t *)self + 0x10); key = param | (a << 8) | (((uint32_t)(int)(b * 10.0f) & 0xFFFFu) << 16); }
    key |= 0x80000000u;
    for (j = 0; j < 16; j++) { if (w->key[kind][j] == key) { slot = j; break; } if (slot < 0 && !w->key[kind][j]) slot = j; }
    if (slot < 0) return;
    if (w->key[kind][slot] == key && w->last[kind][slot] == (int8_t)r) return;
    w->key[kind][slot] = key; w->last[kind][slot] = (int8_t)r;
    if (w->n >= 80) return;
    w->n++;
    if (kind == 1) push_text("WATCH TD %s ent=%08lX op=%u idx=%u thr=%.2f -> %d +%ums", w->role ? "other" : "chosen", (unsigned long)(uintptr_t)ent, (unsigned)param, (unsigned)a, b, (int)r, age_ms(now_ms(), w->t));
    else push_text("WATCH %s %s ent=%08lX param=%u -> %d +%ums", K[kind], w->role ? "other" : "chosen", (unsigned long)(uintptr_t)ent, (unsigned)param, (int)r, age_ms(now_ms(), w->t));
}
/* the crowd as a list: enemy (last 6 hex digits) and distance to the Prince */

/* v3.05: two quiet diagnostics.  FALL: an enemy whose height drops fast is logged with everything that could explain it.
 * HEAL: the Prince's objects are scanned for a float that rises by 3..90 in one step (a health pickup), so the offset of his health can be found. */

/* v3.05: the Prince's health lives in a component (type descriptor 0x11197e0) that the game finds with 0x97C2C0 (esi = entity, one stack argument, ret 4):
 * +0x58 = current, +0x3C = maximum (both int; the game's own stats dump at 0x78A2FC reads them the same way).
 * A health pickup raises the current value in one step.  The step is cut to g_heal_pct percent.  The fields count as verified after the current value was seen to fall once. */
static uint32_t get_comp(uint32_t ent, uint32_t type)
{
    uint32_t r;
    __asm__ volatile ("pushl %2\n\tcall *%3" : "=a"(r), "+S"(ent) : "r"(type), "r"(0x97C2C0u) : "ecx", "edx", "memory", "cc");
    return r;
}
static uint32_t prince_ent_ptr(void)
{
    uint32_t mgr = *(volatile uint32_t *)0x11A53DC, ref, fl, ent;
    if (mgr < 0x10000u || IsBadReadPtr((void *)(uintptr_t)(mgr + 0x10), 4)) return 0;
    ref = *(volatile uint32_t *)(uintptr_t)(mgr + 0x10);
    if (ref < 0x10000u || IsBadReadPtr((void *)(uintptr_t)ref, 12)) return 0;
    fl = *(volatile uint32_t *)(uintptr_t)(ref + 8);
    if (!(fl & 0x10000000u)) return 0;
    ent = *(volatile uint32_t *)(uintptr_t)ref;
    return ent < 0x10000u ? 0 : ent;
}
static void hp_tick(unsigned now)
{
    static int last_cur = -1, last_max = -1, verified; static unsigned last_t;
    uint32_t ent = prince_ent_ptr(), comp = 0; volatile int *pc; int cur, mx; static unsigned t_hb; static int why;
    if (!ent || IsBadReadPtr((void *)(uintptr_t)(ent + 0x84), 4)) { why = 1; }
    else {
        comp = get_comp(ent, 0x11197E0u);
        if (comp < 0x10000u || IsBadReadPtr((void *)(uintptr_t)comp, 0x60)) { why = 2; comp = 0; }
    }
    if (!comp) { if (age_ms(now, t_hb) >= 5000u) { t_hb = now | 1u; push_text("HP-BEAT no component (reason %d: 1=no Prince entity, 2=component lookup failed)", why); } return; }
    pc = (volatile int *)(uintptr_t)(comp + 0x58); cur = *pc; mx = *(volatile int *)(uintptr_t)(comp + 0x3C);      /* v3.09: HP-BEAT showed +0x58 falling (100,40,37,61) while +0x3C stayed 100: +0x58 is the CURRENT health, +0x3C the maximum */
    if (age_ms(now, t_hb) >= 5000u) { t_hb = now | 1u; push_text("HP-BEAT cur=%d max=%d f58=%d f40=%d f44=%d f48=%d f4c=%d f50=%d f54=%d", cur, mx, *(volatile int *)(uintptr_t)(comp + 0x58), *(volatile int *)(uintptr_t)(comp + 0x40), *(volatile int *)(uintptr_t)(comp + 0x44), *(volatile int *)(uintptr_t)(comp + 0x48), *(volatile int *)(uintptr_t)(comp + 0x4C), *(volatile int *)(uintptr_t)(comp + 0x50), *(volatile int *)(uintptr_t)(comp + 0x54)); }
    if (mx < 1 || mx > 5000 || cur < 0 || cur > mx) { last_cur = -1; return; }
    if (mx != last_max) { push_text("HP max=%d cur=%d (comp=%08lX)", mx, cur, (unsigned long)comp); last_max = mx; last_cur = cur; return; }
    if (last_cur >= 0 && cur != last_cur) {
        if (cur < last_cur) { verified = 1; push_text("HP %d -> %d (hit, max=%d)", last_cur, cur, mx); }
        else {
            int delta = cur - last_cur; int contiguous = last_t && age_ms(now, last_t) < 400u && last_cur > 0;
            if (g_heal_on && verified && contiguous && delta >= 3 && g_heal_pct < 100) {
                int add = (delta * g_heal_pct + 50) / 100, nc; if (add < 1) add = 1;
                nc = last_cur + add; *pc = nc;
                push_text("HEAL-SCALE +%d cut to +%d (%d%%): HP %d -> %d (max=%d)", delta, add, g_heal_pct, last_cur, nc, mx);
                cur = nc;
            } else push_text("HP %d -> %d (+%d, not changed: verified=%d contiguous=%d)", last_cur, cur, delta, verified, contiguous);
        }
    }
    { static int stable; if (last_t && age_ms(now, last_t) < 400u) { if (++stable >= 10) verified = 1; } else stable = 0; }   /* v3.11: 1 s of steady readings also counts as verified (pickups before the first hit were not cut) */
    last_cur = cur; last_t = now | 1u;
}
static int nd_get(uint32_t ent, float *pos, uint32_t *objp);
static void v305_tick(unsigned now)
{
    static unsigned t_d; static struct { uint32_t ent; float z; unsigned t; } zs[NSEEN]; static float hs[3][0x300]; static int hs_ok[3]; static unsigned n_heal;
    int i;
    if (age_ms(now, t_d) < 100u) return;
    t_d = now | 1u;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent; float pos[3]; uint32_t obj = 0;
        if (!e || age_ms(now, g_seen[i].t) > 1500u) { zs[i].ent = 0; continue; }
        if (nd_get(e, pos, &obj) != 1) continue;
        if (zs[i].ent == e && age_ms(now, zs[i].t) <= 400u && zs[i].z - pos[2] > 2.0f) {
            push_text("FALL ent=%08lX kind=%d dz=%.1f in %ums pos=%.1f,%.1f,%.1f dist=%.2f ph=%d wide=%d last_hit_ago=%ums lmb_ago=%ums", (unsigned long)e, ent_kind(e), zs[i].z - pos[2], age_ms(now, zs[i].t), pos[0], pos[1], pos[2], seen_get_dist(e), g_phase, g_wide, g_last_hit_t ? age_ms(now, g_last_hit_t) : 99999u, g_last_press ? age_ms(now, g_last_press) : 99999u);
            zs[i].ent = 0;
        }
        if (zs[i].ent != e || age_ms(now, zs[i].t) > 200u) { zs[i].ent = e; zs[i].z = pos[2]; zs[i].t = now | 1u; }
    }
    hp_tick(now);
}
static void map_tick(unsigned now)
{
    v305_tick(now);
    char line[150]; int i, n = 0, len;
    if (!g_cfg.diag || g_cfg.map_ms <= 0 || age_ms(now, g_map_t) < (unsigned)g_cfg.map_ms) return;
    g_map_t = now | 1u;
    len = _snprintf(line, sizeof line, "MAP ph=%d f=%.2f", g_phase, g_f);
    for (i = 0; i < NSEEN && n < 11 && len > 0 && len < 125; i++)
        if (g_seen[i].ent && age_ms(now, g_seen[i].t) <= 700u && g_seen[i].d >= 0) { len += _snprintf(line + len, sizeof line - len, " %06lX:%.1f", (unsigned long)(g_seen[i].ent & 0xFFFFFFu), g_seen[i].d); n++; }
    line[sizeof line - 1] = 0;
    push_text("%s", line);
    {   /* v2.56: one MX line per map step while the Prince charges: everything needed to judge the mixed-crowd behaviour from a single log */
        static unsigned last_ref, last_blk, last_as, last_ao;
        if (g_wide) {
            char m[420]; int ml; unsigned ch = g_des_ent; int pass;
            ml = _snprintf(m, sizeof m, "MX ph=%d pd=%.2f pulse=%d shields=%d others=%d chosen=%06lX(%c,%.1f) +refused=%u +ring0_blocked=%u +atk_shield=%u +atk_soldier=%u wnd_ring_blocked=%u |",
                           g_phase, g_pd, g_pulse_run ? 1 : 0, g_n_shield, g_n_other, (unsigned long)(ch & 0xFFFFFFu), ch ? (ent_kind(ch) == 2 ? 'S' : 'o') : '-', ch ? seen_get_dist(ch) : -1.0f,
                           g_mx_refused - last_ref, g_mx_ring_block - last_blk, g_mx_as_s - last_as, g_mx_as_o - last_ao, g_wnd_ring_block);
            for (pass = 0; pass < 2; pass++)
                for (i = 0; i < NSEEN && ml > 0 && ml < 380; i++)
                    if (g_seen[i].ent && age_ms(now, g_seen[i].t) <= 700u && g_seen[i].d >= 0 && g_seen[i].d < 20.0f && ((ent_kind(g_seen[i].ent) == 2) == (pass == 0)))
                        ml += _snprintf(m + ml, sizeof m - ml, " %c%06lX:%.1f", pass == 0 ? 'S' : (wounded_now(g_seen[i].ent) ? 'w' : 'o'), (unsigned long)(g_seen[i].ent & 0xFFFFFFu), g_seen[i].d);
            m[sizeof m - 1] = 0;
            push_text("%s", m);
        }
        last_ref = g_mx_refused; last_blk = g_mx_ring_block; last_as = g_mx_as_s; last_ao = g_mx_as_o;
        if (g_wide) slotd_tick(now);
    }
}

/* an enemy finished an attack: if he ended it far from the Prince (the attack missed) the group steps back for a while */
static void on_attack_end(void *ent)
{
    unsigned now = now_ms();
    float raw, d = -1.0f;
    int failed, n = g_nenemies;
    if (probe_dist(ent, &raw) && raw > 0) d = sqrtf(raw);
    int chosen_swing = ((uint32_t)(uintptr_t)ent == g_des_last && g_des_last_t && age_ms(now, g_des_last_t) < 4000u);
    failed = g_cfg.retreat_fail <= 0 || (d >= 0 && d >= g_cfg.retreat_fail);
    if (chosen_swing) failed = 0;   /* v2.9: the teasing swing of the chosen attacker is not a miss, the group does not retreat because of it */
    push_text("ATTACK-END ent=%08lX dist=%.2f enemies=%d missed=%d chosen=%d", (unsigned long)(uintptr_t)ent, d, n, failed, chosen_swing);
    if (g_cfg.retreat_on && g_cfg.patterns && failed && n >= g_cfg.retreat_min && g_phase != PH_UP && g_phase != PH_FULL) {
        if ((int)(g_retreat_until - now) <= 0) push_text("RETREAT start for %d ms", g_cfg.retreat_ms);
        g_retreat_until = now + (unsigned)g_cfg.retreat_ms;
    }
}

/* The chosen attacker of a pulse: the game never asks a far enemy to attack by itself, so the answer 'sword attack' is forced for him */
static float nearest_shield(unsigned now)
{
    int i; float best = 1.0e9f;
    for (i = 0; i < NSEEN; i++)
        if (g_seen[i].ent && now - g_seen[i].t <= SEEN_MS && g_seen[i].d >= 0 && g_seen[i].d < best && ent_kind(g_seen[i].ent) == 2) best = g_seen[i].d;
    return best;
}
static void des_update(void *ent, unsigned now, uint8_t *r)
{
    uint32_t e = (uint32_t)(uintptr_t)ent;
    float d;
    int i, maxd;
    if (!g_cfg.pulse_on || !g_cfg.des_on || !g_cfg.patterns) return;
    if (g_des_ent && age_ms(now, g_des_t) > (unsigned)g_cfg.des_to * ((g_mixed_on && g_n_shield > 0 && g_n_other > 0) ? 2u : 1u)) {
        push_text("TEASE chosen attacker timeout ent=%08lX", (unsigned long)g_des_ent);
        g_des_last = g_des_ent; g_des_ent = 0;
    }
    if (g_des_ent == e) {
        float dd = seen_get_dist(e);
        static unsigned t_app; static uint32_t e_app;
        if (g_cfg.app_on && dd > g_cfg.app_dist) {            /* v2.11: not close enough yet: no swing, he walks in by himself */
            if (e_app != e || age_ms(now, t_app) >= 200u) { e_app = e; t_app = now | 1u; push_text("APPROACH ent=%08lX dist=%.2f t=%ums ring_f=%.2f", (unsigned long)e, dd, age_ms(now, g_des_t), g_f); }
            *r = 0; return;
        }
        *r = 1; return;
    }
    if (g_des_ent || !g_pulse || g_phase != PH_FULL) return;
    if (g_des_pn != g_pulse_n) { g_des_pn = g_pulse_n; g_des_count = 0; }
    maxd = 1 + ((g_nenemies >= 5 && age_ms(now, g_cycle_t0) > 3000u) ? 1 : 0);
    if (g_des_count >= maxd) return;
    if (g_des_count > 0 && age_ms(now, g_des_t) < 600u) return;
    d = seen_get_dist(e);
    if (g_sr_on && g_n_shield > 0 && g_wide && ent_kind(e) != 2) return;      /* v2.99: a soldier is never the chosen attacker while a shield-bearer is around */
    if (g_mixed_on && g_n_shield > 0 && g_n_other > 0) {      /* v2.53c: in a mixed crowd only a shield-bearer can be the one who comes out; v2.58: the nearest one */
        if (ent_kind(e) != 2) return;
        if (d > nearest_shield(now) + 1.5f) return;
    }
    if (d < g_cfg.des_lo || d > ((g_mixed_on && g_n_shield > 0 && g_n_other > 0) ? 14.0f : g_cfg.des_hi)) return;
    if (e == g_des_last && g_nenemies > 2) return;
    for (i = 0; i < 48; i++) if (g_att[i].ent == e && g_att[i].was) return;
    g_des_ent = e; g_des_t = now | 1u; g_des_count++;
    if (g_ps_on) g_ps_leave = 1;
    if (g_mixed_on && g_n_shield > 0 && g_n_other > 0) g_relayout = 1;      /* v2.61: the chosen shield-bearer gets his place again with the pulled-in ring */
    push_text("TEASE chosen attacker ent=%08lX dist=%.1f (%d of %d in this pulse, enemies=%d)", (unsigned long)e, d, g_des_count, maxd, g_nenemies);
    watch_start(ent, now);
    if (g_cfg.app_on && d > g_cfg.app_dist) *r = 0; else *r = 1;      /* v2.13: too far: he walks in first */
}

/* Diagnostics: the object at entity+0x34 answers the 'CombatBehavior' question (vtable slot 0, kind 0 = sword attack).
 * Its memory is written to the trace when the answer is yes, when the enemy is the chosen attacker, and now and then for 'no' */
static void beh_log(void *ent, int natural, int chosen)
{
    static int n_yes, n_no, n_ch; static unsigned t_no;
    unsigned now = now_ms();
    HANDLE me = GetCurrentProcess();
    uint32_t obj = 0, vt = 0, sl[8];
    uint8_t buf[0xC0];
    int i;
    if (natural) { if (n_yes >= 40) return; n_yes++; }
    else if (chosen) { if (n_ch >= 40) return; n_ch++; }
    else { if (n_no >= 40 || age_ms(now, t_no) < 2500u) return; n_no++; t_no = now | 1u; }
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x34, &obj, 4, NULL) || !obj) { push_text("BEH ent=%08lX natural=%d chosen=%d: no object", (unsigned long)(uintptr_t)ent, natural, chosen); return; }
    if (!ReadProcessMemory(me, (void *)(uintptr_t)obj, &vt, 4, NULL) || !vt) return;
    memset(sl, 0, sizeof sl);
    ReadProcessMemory(me, (void *)(uintptr_t)vt, sl, sizeof sl, NULL);
    push_text("BEH ent=%08lX natural=%d chosen=%d obj=%08lX vt=%08lX slots=%08lX %08lX %08lX %08lX %08lX %08lX", (unsigned long)(uintptr_t)ent, natural, chosen, (unsigned long)obj, (unsigned long)vt,
              (unsigned long)sl[0], (unsigned long)sl[1], (unsigned long)sl[2], (unsigned long)sl[3], (unsigned long)sl[4], (unsigned long)sl[5]);
    memset(buf, 0, sizeof buf);
    ReadProcessMemory(me, (void *)(uintptr_t)obj, buf, sizeof buf, NULL);
    for (i = 0; i < 4; i++) {
        char hex[100]; int k;
        for (k = 0; k < 48; k++) { static const char *H = "0123456789ABCDEF"; hex[2 * k] = H[buf[i * 48 + k] >> 4]; hex[2 * k + 1] = H[buf[i * 48 + k] & 15]; }
        hex[96] = 0;
        push_text("BEHD %02X %s", i * 48, hex);
    }
}

/* ---- v2.14 diagnostics: enemy speed (animation tempo) and the angles of the crowd around the Prince ---- */
static int enemy_pos(void *ent, float p[3])
{
    uint32_t a = 0;
    HANDLE me = GetCurrentProcess();
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x20, &a, 4, NULL) || !a) return 0;
    return ReadProcessMemory(me, (uint8_t *)(uintptr_t)a + 0x40, p, 12, NULL) ? 1 : 0;
}
static void hex_dump_tag(const char *tag, const uint8_t *b, int n)
{
    int o, k;
    for (o = 0; o < n; o += 48) {
        char hex[100]; int m = n - o < 48 ? n - o : 48;
        static const char *H = "0123456789ABCDEF";
        for (k = 0; k < m; k++) { hex[2 * k] = H[b[o + k] >> 4]; hex[2 * k + 1] = H[b[o + k] & 15]; }
        hex[2 * m] = 0;
        push_text("SPDD %s %03X %s", tag, o, hex);
    }
}
/* v2.16: follow the pointers found in an object and dump 96 bytes behind each (the animation tempo is probably in a sub-object) */
static void dump_ptees(const char *tag, const uint8_t *b, int n)
{
    int o, cnt = 0;
    HANDLE me = GetCurrentProcess();
    static uint8_t pb[96];
    for (o = 0; o + 4 <= n && cnt < 12; o += 4) {
        uint32_t v = *(const uint32_t *)(b + o);
        if (!looks_ptr(v)) continue;
        if (!ReadProcessMemory(me, (void *)(uintptr_t)v, pb, 96, NULL)) continue;
        cnt++;
        {
            char hex[100]; int k; static const char *H = "0123456789ABCDEF";
            for (k = 0; k < 48; k++) { hex[2 * k] = H[pb[k] >> 4]; hex[2 * k + 1] = H[pb[k] & 15]; }
            hex[96] = 0;
            push_text("SPDD P%s%03X.0 %s", tag, o, hex);
            for (k = 0; k < 48; k++) { hex[2 * k] = H[pb[48 + k] >> 4]; hex[2 * k + 1] = H[pb[48 + k] & 15]; }
            push_text("SPDD P%s%03X.1 %s", tag, o, hex);
        }
    }
}
typedef struct { uint32_t ent; unsigned t; float p[3]; } SpdT;
static SpdT g_sp[4];
static int g_sp_mov, g_sp_still;
static void spd_tick(void *ent, unsigned now)
{
    float p[3], dx, dy, v;
    unsigned dt;
    int i, k = -1, moving, still;
    uint32_t e = (uint32_t)(uintptr_t)ent, act = 0, obj = 0;
    static uint8_t buf[1280];
    HANDLE me = GetCurrentProcess();
    if (!g_cfg.diag || !g_cfg.spd_dump || (g_sp_mov >= 70 && g_sp_still >= 40)) return;
    if (!enemy_pos(ent, p)) return;
    for (i = 0; i < 4; i++) if (g_sp[i].ent == e) { k = i; break; }
    if (k < 0) {
        for (i = 0; i < 4; i++) if (!g_sp[i].ent) { k = i; break; }
        if (k < 0) return;
        g_sp[k].ent = e; g_sp[k].t = now | 1u; memcpy(g_sp[k].p, p, 12);
        return;
    }
    dt = age_ms(now, g_sp[k].t);
    if (dt < 200u) return;
    dx = p[0] - g_sp[k].p[0]; dy = p[1] - g_sp[k].p[1];
    v = sqrtf(dx * dx + dy * dy) / ((float)dt / 1000.0f);
    g_sp[k].t = now | 1u; memcpy(g_sp[k].p, p, 12);
    if (dt > 700u) return;
    moving = v > 0.6f; still = v < 0.08f;
    { float dd = seen_get_dist(e); if (dd < 0 || dd > 10.0f) return; }   /* v2.16: only near the fight, not the run-in from afar */
    if (moving && g_sp_mov < 70) g_sp_mov++; else if (still && g_sp_still < 40) g_sp_still++; else return;
    push_text("SPD ent=%08lX v=%.2f dist=%.2f moving=%d phase=%d wide=%d", (unsigned long)e, v, seen_get_dist(e), moving, g_phase, g_wide);
    if (ReadProcessMemory(me, (uint8_t *)ent + 0x2C, &act, 4, NULL) && act) {
        memset(buf, 0, sizeof buf);
        if (ReadProcessMemory(me, (void *)(uintptr_t)act, buf, 1272, NULL)) { hex_dump_tag("A", buf, 1272); dump_ptees("A", buf, 1272); }
    }
    if (ReadProcessMemory(me, (uint8_t *)ent + 0x38, &obj, 4, NULL) && obj) {
        memset(buf, 0, sizeof buf);
        if (ReadProcessMemory(me, (void *)(uintptr_t)obj, buf, 512, NULL)) { hex_dump_tag("O", buf, 512); dump_ptees("O", buf, 512); }
    }
    {   /* v2.15: the slot object (ent+0x30), the behaviour object (ent+0x34) and what the actor points to */
        uint32_t so = 0, bo = 0;
        if (ReadProcessMemory(me, (uint8_t *)ent + 0x30, &so, 4, NULL) && so) { memset(buf, 0, sizeof buf); if (ReadProcessMemory(me, (void *)(uintptr_t)so, buf, 256, NULL)) hex_dump_tag("S", buf, 256); }
        if (ReadProcessMemory(me, (uint8_t *)ent + 0x34, &bo, 4, NULL) && bo) { memset(buf, 0, sizeof buf); if (ReadProcessMemory(me, (void *)(uintptr_t)bo, buf, 256, NULL)) hex_dump_tag("B", buf, 256); }
    }
}
static void ang_tick(unsigned now)
{
    static unsigned t_last; static float hx, hy, head = 1000.0f; static int have_h;
    float P[3], q[3], rel[16]; int i, n = 0, j, att = 0, near3 = 0, behind = 0, front = 0;
    char line[160]; int len;
    if (!g_cfg.diag || g_nenemies < 3 || age_ms(now, t_last) < 1000u) return;
    t_last = now | 1u;
    for (i = 0; i < NSEEN; i++) if (g_seen[i].ent && age_ms(now, g_seen[i].t) <= 700u && prince_pos((void *)(uintptr_t)g_seen[i].ent, P)) break;
    if (i >= NSEEN) return;
    if (have_h && (fabsf(P[0] - hx) + fabsf(P[1] - hy)) > 0.4f) head = atan2f(P[1] - hy, P[0] - hx) * 57.29578f;
    hx = P[0]; hy = P[1]; have_h = 1;
    for (i = 0; i < NSEEN && n < 16; i++) {
        float a;
        if (!g_seen[i].ent || age_ms(now, g_seen[i].t) > 700u || g_seen[i].d < 0 || g_seen[i].d > 9.0f) continue;
        if (!enemy_pos((void *)(uintptr_t)g_seen[i].ent, q)) continue;
        a = atan2f(q[1] - P[1], q[0] - P[0]) * 57.29578f;
        if (head < 999.0f) { a -= head; while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; }
        rel[n++] = a;
        if (g_seen[i].d < 3.5f) near3++;
        if (fabsf(a) > 120.0f) behind++; else if (fabsf(a) < 60.0f) front++;
    }
    for (i = 0; i < 48; i++) if (g_att[i].ent && g_att[i].was) att++;
    for (i = 1; i < n; i++) { float v = rel[i]; for (j = i - 1; j >= 0 && rel[j] > v; j--) rel[j + 1] = rel[j]; rel[j + 1] = v; }
    len = _snprintf(line, sizeof line, "ANG n=%d att=%d near3=%d front=%d behind=%d head=%s |", n, att, near3, front, behind, head < 999.0f ? "move" : "?");
    for (i = 0; i < n && len < 140; i++) len += _snprintf(line + len, sizeof line - len, " %.0f", rel[i]);
    line[sizeof line - 1] = 0;
    push_text("%s", line);
}

/* ---- v2.15: what do the enemies answer during a melee, by distance? (far = more than 3.5 from the Prince) ---- */
static unsigned g_fq[5][16][2][2];     /* kind, param, answer, far */
static unsigned g_fq_t;
static int melee_now(unsigned now)
{
    int i;
    for (i = 0; i < NSEEN; i++) if (g_seen[i].ent && age_ms(now, g_seen[i].t) <= 700u && g_seen[i].d >= 0 && g_seen[i].d < 2.5f) return 1;
    return 0;
}
static void fq_note(int kind, void *self, void *ent, uint8_t r)
{
    static const char *nm[5] = { "MM", "TD", "FR", "TQ", "FA" };
    unsigned now;
    uint32_t p;
    float d;
    int k, q;
    if (!g_cfg.diag) return;
    now = now_ms();
    p = *(uint32_t *)((uint8_t *)self + 8);
    d = seen_get_dist((uint32_t)(uintptr_t)ent);
    if (p < 16 && d >= 0 && melee_now(now)) g_fq[kind][p][r ? 1 : 0][d > 3.5f ? 1 : 0]++;
    if (age_ms(now, g_fq_t) >= 5000u) {
        g_fq_t = now | 1u;
        for (k = 0; k < 5; k++) {
            char line[160]; int len = 0, any = 0;
            len = _snprintf(line, sizeof line, "FARQ %s (far no/yes | near no/yes):", nm[k]);
            for (q = 0; q < 16 && len < 120; q++) {
                unsigned a = g_fq[k][q][0][1], b = g_fq[k][q][1][1], c = g_fq[k][q][0][0], e = g_fq[k][q][1][0];
                if (a + b + c + e == 0) continue;
                len += _snprintf(line + len, sizeof line - len, " p%d %u/%u|%u/%u", q, a, b, c, e); any = 1;
            }
            line[sizeof line - 1] = 0;
            if (any) push_text("%s", line);
        }
    }
}

/* v2.64: the game's own 'move one ring inwards' routine (0x597C80, eax = the enemy's ring-member object = [entity+0x30]).
 * The AI calls it only when its behaviour tree says so; for the chosen shield-bearer of a mixed crowd we call it ourselves, from the AI thread. */
static uint8_t call_ring_step(uint32_t member)
{
    uint8_t r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(0x597C80u), "0"(member) : "ecx", "edx", "memory", "cc");
    return r;
}
/* v2.68: ring bookkeeping of the game: 0x597400 (eax = ring member, stack = manager) returns the member's entry; entry+0x10 -> ring, ring+0x18 = ring number (0 nearest .. 2 outer).
 * 0x597DA0 (eax = ring member) lets the member leave its ring. */
static uint32_t call_lookup(uint32_t member, uint32_t mgr)
{
    uint32_t r;
    __asm__ volatile ("pushl %2\n\tcall *%1" : "=a"(r) : "r"(0x597400u), "r"(mgr), "0"(member) : "ecx", "edx", "memory", "cc");
    return r;
}
static void call_leave(uint32_t member)
{
    uint32_t r;
    __asm__ volatile ("call *%1" : "=a"(r) : "r"(0x597DA0u), "0"(member) : "ecx", "edx", "memory", "cc");
    (void)r;
}
static int ring_of(uint32_t member)
{
    uint32_t mgr = *(volatile uint32_t *)0x11A53DC, entry, ring, idx = 0;
    HANDLE me = GetCurrentProcess();
    if (mgr < 0x10000u || member < 0x10000u) return -1;
    entry = call_lookup(member, mgr);
    if (entry < 0x10000u) return -1;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)entry + 0x10, &ring, 4, NULL) || ring < 0x10000u) return -1;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)ring + 0x18, &idx, 4, NULL) || idx > 2u) return -1;
    return (int)idx;
}
static uint32_t member_of(uint32_t ent)
{
    uint32_t m = 0; uint8_t f = 0;
    HANDLE me = GetCurrentProcess();
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)ent + 0x30, &m, 4, NULL) || m < 0x10000u) return 0;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)m + 0x22, &f, 1, NULL) || !f) return 0;
    return m;
}
/* the chosen shield-bearer cannot step inwards because the ring in front of him is full of soldiers: send ONE soldier out of that ring */
static void evict_soldier_from_ring(int ring_no, unsigned now)
{
    int i;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m;
        if (!e || e == g_des_ent || age_ms(now, g_seen[i].t) > 2000u) continue;
        if (ent_kind(e) != 1) continue;
        m = member_of(e);
        if (!m || ring_of(m) != ring_no) continue;
        call_leave(m);
        push_text("CHOSEN-EVICT soldier ent=%08lX left ring %d to make room (dist=%.1f)", (unsigned long)e, ring_no, g_seen[i].d);
        return;
    }
}
/* v2.69: occupancy of the three rings (count of slots, occupied, +0x54, +0x58) for the log */
extern volatile unsigned g_dbg_r1_any, g_dbg_r1_ch, g_dbg_r2_ch, g_dbg_r1_inf; extern volatile float g_dbg_r1_min;
static void ring_stats(char *out, size_t n)
{
    uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, rg[3] = {0,0,0}, w[4], glob = 0, g2 = 0;
    HANDLE me = GetCurrentProcess();
    int i, len = 0;
    out[0] = 0;
    if (mgr < 0x10000u || !ReadProcessMemory(me, (uint8_t *)(uintptr_t)mgr + 0x40, &arr, 4, NULL) || arr < 0x10000u) { _snprintf(out, n, "rings?"); return; }
    ReadProcessMemory(me, (uint8_t *)(uintptr_t)arr, rg, 12, NULL);
    for (i = 0; i < 3; i++) {
        w[0] = w[1] = w[2] = w[3] = 0xFFFFu;
        if (rg[i] > 0x10000u) {
            ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[i] + 0x14, &w[0], 4, NULL);
            ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[i] + 0x50, &w[1], 4, NULL);
            ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[i] + 0x54, &w[2], 4, NULL);
            ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[i] + 0x58, &w[3], 4, NULL);
        }
        len += _snprintf(out + len, n - len, " R%d[slots=%u occ=%u m54=%u o58=%u]", i, (unsigned)(w[0] & 0x3fffu), (unsigned)w[1], (unsigned)w[2], (unsigned)w[3]);
    }
    if (ReadProcessMemory(me, (void *)0x11A53ACu, &glob, 4, NULL) && glob > 0x10000u) ReadProcessMemory(me, (uint8_t *)(uintptr_t)glob + 0x68, &g2, 4, NULL);
    len += _snprintf(out + len, n - len, " glob68=%u", (unsigned)g2);
    _snprintf(out + len, n - len, " | slotcalls r1(all)=%u r1(chosen)=%u inf=%u min=%.1f r2(chosen)=%u", g_dbg_r1_any, g_dbg_r1_ch, g_dbg_r1_inf, (double)g_dbg_r1_min, g_dbg_r2_ch);
    g_dbg_r1_any = g_dbg_r1_ch = g_dbg_r1_inf = g_dbg_r2_ch = 0; g_dbg_r1_min = 1e30f;
}
/* v2.77: at the start of a strong attack every enemy farther than near_dist (2.0) but inside the danger zone (7.0) is sent out of his ring ONCE per charge: he asks for a place again and gets one at the widened ring.
 * Shield-bearers stay. Runs on the AI thread (called from the ring/move hooks). */
static void leave_zone_push(void)
{
    static uint32_t done[NSEEN]; static unsigned done_cycle, t_lz; static int dn;
    unsigned now = now_ms(); int i;
    return;      /* v2.78: switched off: the enemies sent out of ring 0 came back closer (see the log numbers) */
    if (!g_cfg.patterns || (g_phase != PH_UP && g_phase != PH_FULL) || age_ms(now, g_cycle_t0) > 3000u) return;
    if (t_lz && age_ms(now, t_lz) < 60u) return;
    t_lz = now | 1u;
    if (done_cycle != g_cycle_t0) { done_cycle = g_cycle_t0; dn = 0; }
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m; float d; int k, dup = 0, rn;
        if (!e || age_ms(now, g_seen[i].t) > 700u) continue;
        d = g_seen[i].d;
        if (d <= g_cfg.near_dist || d > 7.0f) continue;
        if (ent_kind(e) == 2 || wounded_now(e)) continue;
        for (k = 0; k < dn; k++) if (done[k] == e) { dup = 1; break; }
        if (dup) continue;
        m = member_of(e);
        if (!m) continue;
        rn = ring_of(m);
        if (dn < NSEEN) done[dn++] = e;
        if (rn < 0) continue;
        call_leave(m);
        push_text("ZONE-LEAVE ent=%08lX dist=%.2f ring=%d sent out of the ring", (unsigned long)e, d, rn);
        g_relayout = 1;
        return;      /* one per call: the next hook call takes the next enemy */
    }
}
static void evt_leave_all(void)
{
    unsigned now = now_ms(); int i, n = 0;
    if (!g_evt_leave) return;
    g_evt_leave = 0;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m;
        if (!e || age_ms(now, g_seen[i].t) > 1500u || ent_kind(e) != 1) continue;
        m = member_of(e); if (!m || ring_of(m) < 0) continue;
        call_leave(m); n++;
    }
    g_relayout = 1;
    push_text("EVTEST soldiers sent out of their rings: %d", n);
}
static void ps_leave_others(void)
{
    unsigned now = now_ms(); int i, n = 0;
    if (!g_ps_on || !g_ps_leave) return;
    if (!g_pulse_run || !g_des_ent) { g_ps_leave = 0; return; }
    g_ps_leave = 0;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m;
        if (!e || e == g_des_ent || age_ms(now, g_seen[i].t) > 1500u || ent_kind(e) != 1) continue;
        m = member_of(e); if (!m || ring_of(m) != 0) continue;
        call_leave(m); n++;
    }
    g_ps_out += (unsigned)n; g_relayout = 1;
    push_text("PSOLO soldiers sent out of ring 0: %d (chosen %08lX, total %u)", n, (unsigned long)g_des_ent, g_ps_out);
}
static void sr_evict(void)
{
    static unsigned t_ev; static struct { uint32_t ent; unsigned t; } cd[48]; unsigned now = now_ms(); int i, j, st;
    st = (g_sr_on && g_n_shield > 0 && g_wide) ? 1 : 0;   /* v3.03 (v3.01 had st = 0, the squeeze: the ring-0 squeeze is OFF: in the v3.00 test soldiers still stood in ring 0 (43 of 81 samples) and were pulled in to 3.2, so nobody kept distance */
    if (st != g_sr_sq) { g_sr_sq = st; g_f_written = -1.0f; push_text("SHIELD-RESERVE %s: more ring places %s", st ? "on" : "off", st ? "(24/36/90)" : ""); }
    if (!st || age_ms(now, t_ev) < 150u) return;
    t_ev = now | 1u;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m; float d = g_seen[i].d; int rg, free_j = -1, hit = 0;
        if (!e || age_ms(now, g_seen[i].t) > 1500u || d < 0 || d <= g_sr_free || ent_kind(e) != 1) continue;
        m = member_of(e); if (!m) continue;
        rg = ring_of(m); if (rg < 0 || rg > (g_sr_inner ? 1 : 0)) continue;
        for (j = 0; j < 48; j++) { if (cd[j].ent == e) { hit = 1; if (age_ms(now, cd[j].t) < 4000u) break; cd[j].t = now | 1u; hit = 2; break; } if (free_j < 0 && (!cd[j].ent || age_ms(now, cd[j].t) > 10000u)) free_j = j; }
        if (hit == 1) continue;
        if (!hit) { if (free_j < 0) free_j = 0; cd[free_j].ent = e; cd[free_j].t = now | 1u; }
        call_leave(m); g_sr_out++; g_relayout = 1;
        { static unsigned t_l; if (age_ms(now, t_l) >= 1500u) { t_l = now | 1u; push_text("SHIELD-RESERVE soldier %08lX (dist %.1f) sent out of ring %d (%u so far)", (unsigned long)e, d, rg, g_sr_out); } }
        return;
    }
}
static void chosen_ring_push(void *ent)
{
    sr_evict();
    ps_leave_others();
    evt_leave_all();
    leave_zone_push();
    static unsigned t_cp, n_cp, t_dbg;
    uint32_t m = 0, e = g_des_ent;      /* v2.66: the hook is asked about many enemies; the chosen one is pushed whenever ANY of them is asked */
    uint8_t in_ring = 0;
    unsigned now;
    HANDLE me = GetCurrentProcess();
    return;   /* v2.73: the game refuses the step, switched off */
    if (!g_mixed_on || !g_pulse_run || !g_des_ent || g_n_shield <= 0 || g_n_other <= 0) return;
    now = now_ms();
    if (age_ms(now, g_des_t) > 6000u || (t_cp && age_ms(now, t_cp) < 250u)) return;
    t_cp = now | 1u;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)e + 0x30, &m, 4, NULL) || m < 0x10000u) { if (age_ms(now, t_dbg) >= 500u) { t_dbg = now | 1u; push_text("CHOSEN-STEP ent=%08lX: no ring-member object (%08lX)", (unsigned long)e, (unsigned long)m); } return; }
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)(m + 0x22), &in_ring, 1, NULL)) { push_text("CHOSEN-STEP ent=%08lX member=%08lX unreadable", (unsigned long)e, (unsigned long)m); return; }
    if (!in_ring) { if (age_ms(now, t_dbg) >= 500u) { t_dbg = now | 1u; push_text("CHOSEN-STEP ent=%08lX member=%08lX not in a ring", (unsigned long)e, (unsigned long)m); } return; }
    { char rs[420]; int rn = ring_of(m); uint8_t r; uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r1p = 0, f20 = 0, f20n = 0; uint8_t f1c = 0xEE, f48 = 0xEE, f48n = 0xEE; int bypass = 0;
      ring_stats(rs, sizeof rs);
      /* v2.71: the game refuses a newcomer in ring 1 when ring1+0x1C is set and ring1+0x20 is 0, or when ring1+0x48 is 0: lift both for the duration of this one call */
      if (mgr > 0x10000u && ReadProcessMemory(me, (uint8_t *)(uintptr_t)mgr + 0x40, &arr, 4, NULL) && arr > 0x10000u && ReadProcessMemory(me, (uint8_t *)(uintptr_t)arr + 4, &r1p, 4, NULL) && r1p > 0x10000u) {
          ReadProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x1C, &f1c, 1, NULL);
          ReadProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x20, &f20, 4, NULL);
          ReadProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x48, &f48, 1, NULL);
          if (rn == 2) {
              if (f1c && !f20) { f20n = 1; WriteProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x20, &f20n, 4, NULL); bypass |= 1; }
              if (!f48) { f48n = 1; WriteProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x48, &f48n, 1, NULL); bypass |= 2; }
          }
      }
      r = call_ring_step(m);
      if (bypass & 1) WriteProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x20, &f20, 4, NULL);
      if (bypass & 2) WriteProcessMemory(me, (uint8_t *)(uintptr_t)r1p + 0x48, &f48, 1, NULL);
      push_text("CHOSEN-FLAGS ring1: +1C=%u +20=%u +48=%u bypass=%d -> step=%d", (unsigned)f1c, (unsigned)f20, (unsigned)f48, bypass, (int)r); n_cp++; push_text("CHOSEN-STEP #%u ent=%08lX member=%08lX ring=%d -> %d dist=%.2f%s", n_cp, (unsigned long)e, (unsigned long)m, rn, (int)r, seen_get_dist(e), rs);
      if (!r && rn > 0) evict_soldier_from_ring(rn - 1, now); }
}
/* v2.61: the ring re-layout used to wait for an 'attack ask' (CombatBehavior p=5). In a crowd that stands still nobody asks, so the pulled-in ring never reached the slots.
 * Now any ring/move question of the AI thread applies it (at most every 120 ms). */
static void relayout_poll(void)
{
    static unsigned t_rp;
    unsigned now;
    now = now_ms();
    if (!g_relayout) return;
    if (t_rp && age_ms(now, t_rp) < 120u) return;
    t_rp = now | 1u;
    relayout_now();
}
static uint8_t HOOKCALL h_FA(void *self, void *edx, void *ent)
{
    chosen_ring_push(ent);
    uint8_t r = o_FA(self, ent);
    uint32_t p = *(uint32_t *)((uint8_t *)self + 8);
    (void)edx;
    watch_answer(4, ent, self, r); fq_note(4, self, ent, r);
    if (p == 1) {
        int i, slot = -1;
        for (i = 0; i < 48; i++) {
            if (g_att[i].ent == (uint32_t)(uintptr_t)ent) { slot = i; break; }
            if (slot < 0 && g_att[i].ent == 0) slot = i;
        }
        if (slot < 0) { memset(g_att, 0, sizeof g_att); slot = 0; }
        if (g_att[slot].ent == (uint32_t)(uintptr_t)ent && g_att[slot].was && !r) on_attack_end(ent);
        if (r && !(g_att[slot].ent == (uint32_t)(uintptr_t)ent && g_att[slot].was)) {
            static int n_as;
            as_note((uint32_t)(uintptr_t)ent, now_ms());
            { if (ent_kind((uint32_t)(uintptr_t)ent) == 2) g_mx_as_s++; else g_mx_as_o++; }
            if (n_as < 600) { n_as++; push_text("ATTACK-START kind=%d mixed=%d ent=%08lX dist=%.2f since_release=%ums phase=%d pulse=%d chosen=%d", ent_kind((uint32_t)(uintptr_t)ent), (g_n_shield > 0 && g_n_other > 0) ? 1 : 0, (unsigned long)(uintptr_t)ent, seen_get_dist((uint32_t)(uintptr_t)ent), g_rel_t ? age_ms(now_ms(), g_rel_t) : 0u, g_phase, g_pulse_run, g_des_ent == (uint32_t)(uintptr_t)ent); }
        }
        g_att[slot].ent = (uint32_t)(uintptr_t)ent; g_att[slot].was = r ? 1 : 0;
        if (r && g_des_ent == (uint32_t)(uintptr_t)ent) {
            push_text("TEASE chosen attacker started ent=%08lX dist=%.1f after %ums", (unsigned long)g_des_ent, seen_get_dist(g_des_ent), age_ms(now_ms(), g_des_t));
            g_des_last = g_des_ent; g_des_last_t = now_ms() | 1u; g_des_ent = 0;
        }
    }
    if (p == 1 && g_hs_on) {      /* v2.42: probe the other FightAttack questions: 0 = HasPendingDamageReaction, 4 = IsStunned */
        static struct { uint32_t ent; uint8_t pend, stun; } hs[48];
        uint32_t *pp = (uint32_t *)((uint8_t *)self + 8);
        uint32_t sv = *pp; uint8_t r0, r4; int i, sl = -1;
        *pp = 0; r0 = o_FA(self, ent);
        *pp = 4; r4 = o_FA(self, ent);
        *pp = sv;
        for (i = 0; i < 48; i++) { if (hs[i].ent == (uint32_t)(uintptr_t)ent) { sl = i; break; } if (sl < 0 && !hs[i].ent) sl = i; }
        if (sl < 0) { memset(hs, 0, sizeof hs); sl = 0; }
        if (hs[sl].ent != (uint32_t)(uintptr_t)ent) { hs[sl].ent = (uint32_t)(uintptr_t)ent; hs[sl].pend = 0; hs[sl].stun = 0; }
        if ((r0 ? 1 : 0) != hs[sl].pend || (r4 ? 1 : 0) != hs[sl].stun) {
            static int n_hs; if (n_hs < 800) { n_hs++; push_text("HITSIG ent=%08lX pend=%d stun=%d dist=%.2f", (unsigned long)(uintptr_t)ent, r0 ? 1 : 0, r4 ? 1 : 0, seen_get_dist((uint32_t)(uintptr_t)ent)); }
            hs[sl].pend = r0 ? 1 : 0; hs[sl].stun = r4 ? 1 : 0;
        }
    }
    note(1, "FA", ent, p, 0, 0, r);
    return r;
}
/* v2.51: enemy type before the first attack: actor+0xF8C = 1 soldier, 5 shield-bearer (14 of 14 enemies matched +0x2F8 = 15 / 23 in the v2.50 log).  Returns 0 unknown, 1 soldier, 2 shield-bearer. */
static int ent_kind(uint32_t ent)
{
    uint32_t obj = 0, st = 0; HANDLE me = GetCurrentProcess();
    if (!ent || !ReadProcessMemory(me, (void *)(uintptr_t)(ent + 0x38), &obj, 4, NULL) || !obj) return 0;
    if (!ReadProcessMemory(me, (void *)(uintptr_t)(obj - 0x4F8u + 0xF8Cu), &st, 4, NULL)) return 0;
    {   /* v2.86: log every enemy once with the raw type value, to see whether some locations give values other than 1 / 5 */
        static uint32_t lg_ent[256], lg_st[256]; static int lg_n; int i, found = 0;
        for (i = 0; i < lg_n; i++) if (lg_ent[i] == ent) { found = 1; if (lg_st[i] != st) { push_text("KIND-CHANGE ent=%08lX raw=%lu -> %lu", (unsigned long)ent, (unsigned long)lg_st[i], (unsigned long)st); lg_st[i] = st; } break; }
        if (!found && lg_n < 256) {
            uint32_t b2f8 = 0, b2fc = 0; ReadProcessMemory(me, (void *)(uintptr_t)(obj - 0x4F8u + 0x2F8u), &b2f8, 4, NULL);
            lg_ent[lg_n] = ent; lg_st[lg_n] = st; lg_n++;
            push_text("KIND-SEEN ent=%08lX raw_F8C=%lu kind=%d raw_2F8=%lu total=%d", (unsigned long)ent, (unsigned long)st, st == 1 ? 1 : (st == 5 ? 2 : 0), (unsigned long)b2f8, lg_n); (void)b2fc;
        }
    }
    return st == 1 ? 1 : (st == 5 ? 2 : 0);
}

/* v2.89: where do the shield-bearers stand? log their ring index, and remember the commonest one for write_hit */
static void shield_ring_track(unsigned now)
{
    static unsigned t_last; static int n_log;
    int cnt[3] = {0, 0, 0}, i, best = -1, bc = 0;
    if (!g_sq_on || age_ms(now, t_last) < 500u) return;
    t_last = now | 1u;
    for (i = 0; i < NSEEN; i++) {
        uint32_t e = g_seen[i].ent, m; int r;
        if (!e || age_ms(now, g_seen[i].t) > 1500u || ent_kind(e) != 2) continue;
        m = member_of(e); if (!m) continue;
        r = ring_of(m); if (r < 0 || r > 2) continue;
        cnt[r]++;
        if (n_log < 500) { n_log++; push_text("SHRING ent=%08lX ring=%d dist=%.2f f=%.2f sq=%d ev=%d", (unsigned long)e, r, seen_get_dist(e), g_f, g_sq_ring, (g_evt_until && (int)(g_evt_until - now) > 0) ? 1 : 0); }
    }
    for (i = 0; i < 3; i++) if (cnt[i] > bc) { bc = cnt[i]; best = i; }
    {   /* v2.90: the squeezed ring changes only when the new one has been the commonest for 2 s (v2.89 switched 22 times in one fight) */
        static int cand = -2; static unsigned t_cand;
        if (best == g_sq_ring) cand = -2;
        else if (best != cand) { cand = best; t_cand = now | 1u; }
        else if (age_ms(now, t_cand) >= 2000u) { g_sq_ring = best; cand = -2; g_f_written = -1.0f; push_text("SQUEEZE shield ring now %d (%d shield-bearers there)", best, bc); }
    }
}

/* v2.93: direct nudge. Position of an actor: object at ent+0x20; when obj+0x54 is 0 the position (x, y, z) is at obj+0x40 (same layout as the Prince at +0x40; see 0x4D9740) */
static int nd_get(uint32_t ent, float *pos, uint32_t *objp)
{
    HANDLE me = GetCurrentProcess(); uint32_t obj = 0, x54 = 1;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)ent + 0x20, &obj, 4, NULL) || obj < 0x10000u) return 0;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)obj + 0x54, &x54, 4, NULL) || x54) return -1;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)obj + 0x40, pos, 12, NULL)) return 0;
    if (objp) *objp = obj;
    return 1;
}

/* v2.95: where does the game send each enemy?  For every enemy seen: ring number, distance enemy->Prince, enemy->own slot, slot->Prince (all in the x/y plane like 0x59B200),
 * and whether the game's 'in place' test (0x597E30: dist_to_slot^2 - R^2 < 0.0005, R = [mgr+0x3C]->+0x38 or +0x3C) passes. */
static void slot_pos(uint32_t entry, float *out16)
{
    __asm__ volatile ("pushl %1\n\tcall *%2" : "+S"(out16) : "r"(entry), "r"(0x59B010u) : "eax", "ecx", "edx", "memory", "cc");
}
static void slotd_tick(unsigned now)
{
    static int n_lines; static float cfgR[2] = {-1, -1};
    float slot[8] __attribute__((aligned(16))); float pp[3], ep[3];
    uint32_t mgr = *(volatile uint32_t *)0x11A53DC, cfg = 0, m, entry;
    HANDLE me = GetCurrentProcess();
    char line[400]; int len, i, n = 0;
    if (n_lines >= 400 || mgr < 0x10000u || !g_ptgt) return;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)g_ptgt + 0x40, pp, 12, NULL)) return;
    if (ReadProcessMemory(me, (uint8_t *)(uintptr_t)mgr + 0x3C, &cfg, 4, NULL) && cfg > 0x10000u) {
        float rr[2] = {-1, -1};
        if (ReadProcessMemory(me, (uint8_t *)(uintptr_t)cfg + 0x38, rr, 8, NULL) && (rr[0] != cfgR[0] || rr[1] != cfgR[1])) { cfgR[0] = rr[0]; cfgR[1] = rr[1]; push_text("SLOTCFG inplace R38=%.3f R3C=%.3f", rr[0], rr[1]); }
    }
    {   /* v3.02 probe (read only): the three rings: slots, occupants (+0x50), cap (+0x54), blocked (+0x58), enabled slots ([slot+5]) */
        uint32_t arr = 0, rg[3] = {0, 0, 0}; char rs[240]; int rl = 0, ri;
        if (ReadProcessMemory(me, (uint8_t *)(uintptr_t)mgr + 0x40, &arr, 4, NULL) && arr > 0x10000u && ReadProcessMemory(me, (uint8_t *)(uintptr_t)arr, rg, 12, NULL)) {
            rl = _snprintf(rs, sizeof rs, "RINGSTAT ph=%d shields=%d soldiers=%d", g_phase, g_n_shield, g_n_other);
            for (ri = 0; ri < 3; ri++) {
                uint32_t w14 = 0, w50 = 0, w54 = 0, w58 = 0, w10 = 0, sp[64]; int en = 0, k, cnt;
                if (rg[ri] < 0x10000u) continue;
                ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[ri] + 0x14, &w14, 4, NULL); ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[ri] + 0x50, &w50, 4, NULL);
                ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[ri] + 0x54, &w54, 4, NULL); ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[ri] + 0x58, &w58, 4, NULL);
                ReadProcessMemory(me, (uint8_t *)(uintptr_t)rg[ri] + 0x10, &w10, 4, NULL);
                cnt = (int)(w14 & 0x3fffu); if (cnt > 64) cnt = 64;
                if (w10 > 0x10000u && cnt > 0 && ReadProcessMemory(me, (uint8_t *)(uintptr_t)w10, sp, (SIZE_T)cnt * 4, NULL))
                    for (k = 0; k < cnt; k++) { uint8_t f5 = 0; if (sp[k] > 0x10000u && ReadProcessMemory(me, (uint8_t *)(uintptr_t)sp[k] + 5, &f5, 1, NULL) && f5) en++; }
                rl += _snprintf(rs + rl, sizeof rs - rl, " r%d[slots=%d occ=%u cap=%u blk=%u en=%d]", ri, cnt, (unsigned)w50, (unsigned)w54, (unsigned)w58, en);
            }
            rs[sizeof rs - 1] = 0; push_text("%s", rs);
        }
    }
    len = _snprintf(line, sizeof line, "SLOTD ph=%d f=%.2f |", g_phase, g_f);
    for (i = 0; i < NSEEN && n < 9 && len > 0 && len < 340; i++) {
        uint32_t e = g_seen[i].ent; int r, k; float dx, dy, ex, ey, d_ep, d_es, d_sp, d2;
        if (!e || age_ms(now, g_seen[i].t) > 700u || g_seen[i].d < 0 || g_seen[i].d > 14.0f) continue;
        m = member_of(e); if (!m) continue;
        entry = call_lookup(m, mgr); if (entry < 0x10000u) continue;
        r = ring_of(m); if (r < 0) continue;
        if (nd_get(e, ep, NULL) != 1) continue;
        slot[0] = slot[1] = slot[2] = slot[3] = 0; slot_pos(entry, slot);
        dx = ep[0] - pp[0]; dy = ep[1] - pp[1]; d_ep = sqrtf(dx * dx + dy * dy);
        ex = ep[0] - slot[0]; ey = ep[1] - slot[1]; d_es = sqrtf(ex * ex + ey * ey);
        dx = slot[0] - pp[0]; dy = slot[1] - pp[1]; d_sp = sqrtf(dx * dx + dy * dy);
        d2 = d_es * d_es; k = ent_kind(e);
        len += _snprintf(line + len, sizeof line - len, " %c%06lX r%d ep=%.1f(g%.1f) es=%.1f sp=%.1f ip=%d%d;", k == 2 ? 'S' : 'o', (unsigned long)(e & 0xFFFFFFu), r, d_ep, g_seen[i].d, d_es, d_sp,
                         cfgR[0] >= 0 && d2 - cfgR[0] * cfgR[0] < 0.0005f, cfgR[1] >= 0 && d2 - cfgR[1] * cfgR[1] < 0.0005f);
        n++;
    }
    line[sizeof line - 1] = 0;
    if (n) { n_lines++; push_text("%s", line); }
}
static void nudge_tick(uint32_t ent)
{
    static struct { uint32_t ent, obj; unsigned t; float wx, wy; } rec[48];
    static unsigned n_push, n_back, n_fwd, n_over, n_skip, t_log;
    unsigned now = now_ms(); int i, sl = -1, kind; float pp[3], ep[3], dx, dy, d, st; uint32_t obj = 0; int r;
    if (!g_nd_on || !ent || !g_ptgt) return;
    if (g_phase != PH_UP && g_phase != PH_FULL) return;
    if (age_ms(now, g_cycle_t0) > (unsigned)g_nd_ms) return;
    for (i = 0; i < 48; i++) { if (rec[i].ent == ent) { sl = i; break; } if (sl < 0 && !rec[i].ent) sl = i; }
    if (sl < 0) sl = (int)(ent >> 4) % 48;
    if (rec[sl].ent != ent) { memset(&rec[sl], 0, sizeof rec[sl]); rec[sl].ent = ent; }
    if (rec[sl].t && age_ms(now, rec[sl].t) < 50u) return;
    kind = ent_kind(ent);
    if (kind != 1 && kind != 2) return;
    if (ent == g_des_ent) return;
    r = nd_get(ent, ep, &obj); if (r <= 0) { if (r < 0) n_skip++; return; }
    if (!ReadProcessMemory(GetCurrentProcess(), (uint8_t *)(uintptr_t)g_ptgt + 0x40, pp, 12, NULL)) return;
    if (rec[sl].t && age_ms(now, rec[sl].t) < 400u) { float ox = ep[0] - rec[sl].wx, oy = ep[1] - rec[sl].wy; if (ox * ox + oy * oy > 0.0004f) n_over++; }   /* the game moved him since our write */
    dx = ep[0] - pp[0]; dy = ep[1] - pp[1]; d = sqrtf(dx * dx + dy * dy);
    if (d < 0.3f || d > 40.0f) return;
    if (kind == 1) { if (d <= g_nd_min || d >= g_nd_safe || wounded_now(ent)) return; st = g_nd_back; }
    else { if (d < 3.2f || d > 9.0f) return; st = -g_nd_fwd; }
    ep[0] += dx / d * st; ep[1] += dy / d * st;
    if (!WriteProcessMemory(GetCurrentProcess(), (uint8_t *)(uintptr_t)obj + 0x40, ep, 8, NULL)) return;
    rec[sl].obj = obj; rec[sl].t = now | 1u; rec[sl].wx = ep[0]; rec[sl].wy = ep[1];
    n_push++; if (kind == 1) n_back++; else n_fwd++;
    if (age_ms(now, t_log) >= 1000u) { t_log = now | 1u; push_text("NUDGE pushes=%u back(soldiers)=%u forward(shields)=%u game_moved_after=%u skipped(obj54)=%u last: kind=%d ent=%08lX dist=%.2f", n_push, n_back, n_fwd, n_over, n_skip, kind, (unsigned long)ent, d); }
}
static unsigned g_shield_lied;
static uint8_t HOOKCALL h_FR(void *self, void *edx, void *ent)
{
    uint8_t r = o_FR(self, ent); (void)edx;
    relayout_poll(); chosen_ring_push(ent); nudge_tick((uint32_t)(uintptr_t)ent);
    if (!r && g_mixed_on && g_wide && g_n_shield > 0 && g_n_other > 0 && ent_kind((uint32_t)(uintptr_t)ent) == 1 && seen_get_dist((uint32_t)(uintptr_t)ent) >= g_mx_hold) {   /* v2.53d: in a mixed crowd far soldiers do not walk in during a charge */
        static unsigned n_mh, t_mh; r = 1; n_mh++;
        if (age_ms(now_ms(), t_mh) >= 2000u) { t_mh = now_ms() | 1u; push_text("MIXED-HOLD %u soldier ring answers changed so far (last ent=%08lX)", n_mh, (unsigned long)(uintptr_t)ent); }
    }
    if (r && g_mixed_on && g_pulse_run && g_des_ent && (uint32_t)(uintptr_t)ent == g_des_ent && g_n_shield > 0 && g_n_other > 0 && age_ms(now_ms(), g_des_t) < 1500u) {
        uint32_t pp = *(uint32_t *)((uint8_t *)self + 8);
        if (pp == 0u || pp == 12u) {      /* v2.61: the chosen shield-bearer is told 'you are not in your place' so the game gives him a new place: the inner ring (the others are closed for him) */
            static unsigned n_cr, t_cr; r = 0; n_cr++;
            if (age_ms(now_ms(), t_cr) >= 500u) { t_cr = now_ms() | 1u; push_text("CHOSEN-REQUEST %u new-place requests for ent=%08lX (param=%u)", n_cr, (unsigned long)(uintptr_t)ent, pp); }
        }
    }
    if (!r && g_wnd_on && !g_wnd_ring && wounded_now((uint32_t)(uintptr_t)ent)) {            /* v2.53b: a wounded enemy thinks he is in place: he stops closing in on the Prince */
        static unsigned n_wf, t_wf; r = 1; n_wf++;
        if (age_ms(now_ms(), t_wf) >= 2000u) { t_wf = now_ms() | 1u; push_text("WOUNDED-STAY %u ring answers changed so far (last ent=%08lX)", n_wf, (unsigned long)(uintptr_t)ent); }
    }
    if (g_shield_stay && !r && g_wide && *(uint32_t *)((uint8_t *)self + 8) == 12u && ent_kind((uint32_t)(uintptr_t)ent) == 2) {
        static unsigned t_sh;
        r = 1; g_shield_lied++;                                 /* v2.51: a shield-bearer thinks he is in place: he does not back off while the Prince holds LMB */
        if (age_ms(now_ms(), t_sh) >= 1000u) { t_sh = now_ms() | 1u; push_text("SHIELD-STAY %u ring answers changed so far (last ent=%08lX)", g_shield_lied, (unsigned long)(uintptr_t)ent); }
    }
    if (g_cfg.hold_on && g_cfg.app_on && g_pulse_run && !r && *(uint32_t *)((uint8_t *)self + 8) == 12u
        && (uint32_t)(uintptr_t)ent != g_des_ent && !(g_des_last && (uint32_t)(uintptr_t)ent == g_des_last && age_ms(now_ms(), g_des_last_t) < 3000u)) {
        static unsigned n_hold, t_hold;
        r = 1; n_hold++;                                        /* v2.11: the others think they are in place: they do not follow the tight ring */
        if (age_ms(now_ms(), t_hold) >= 1000u) { t_hold = now_ms() | 1u; push_text("HOLD-OTHERS lied %u times so far (last ent=%08lX)", n_hold, (unsigned long)(uintptr_t)ent); }
    }
    watch_tick(ent, now_ms()); watch_answer(2, ent, self, r); watch_fr_post(ent, now_ms()); fq_note(2, self, ent, r);
    note(2, "FR", ent, *(uint32_t *)((uint8_t *)self + 8), 0, 0, r);
    return r;
}
/* Diagnostic: the real distance enemy -> Prince, taken exactly the way FightTargetDistanceCondition does it */
typedef float   (THISCALL *DistFn)(void *obj, int arg);
typedef uint8_t (THISCALL *AliveFn)(void *obj);
static float g_probe_v1c;
static int probe_dist(void *ent, float *raw)
{
    void *obj = NULL, *vt = NULL, *fn0 = NULL, *fn7 = NULL;
    HANDLE me = GetCurrentProcess();
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x38, &obj, 4, NULL) || !obj) return 0;
    if (!ReadProcessMemory(me, obj, &vt, 4, NULL) || !vt) return 0;
    if (!ReadProcessMemory(me, vt, &fn0, 4, NULL) || !ReadProcessMemory(me, (uint8_t *)vt + 0x1c, &fn7, 4, NULL) || !fn0 || !fn7) return 0;
    if (!((AliveFn)fn0)(obj)) return 0;
    *raw = ((DistFn)fn7)(obj, 1);
    g_probe_v1c = -1.0f;
    ReadProcessMemory(me, (uint8_t *)obj + 0x1c, &g_probe_v1c, 4, NULL);
    return 1;
}

/* The Prince is the enemy's fight target: object +0xC -> reference, reference[0] = Prince, his position (x, y, z) is at +0x40 */
/* v2.41: roll fatigue. The roll-end clips (frames) are rewritten in memory: first roll normal, every next roll of a chain makes the recovery clips longer. */
static void rf_apply(unsigned now)
{
    float want = 1.0f;
    int i;
    unsigned ch = g_rchain_pub;
    if (!g_rf_on || g_rf_n <= 0) return;
    if (g_rlast_pub && age_ms(now, g_rlast_pub) <= (unsigned)(g_cfg.roll_window > 1500 ? g_cfg.roll_window : 1500) && ch > 1u) {
        want = 1.0f + (float)g_rf_grow / 100.0f * (float)(ch - 1u);
        if (want > (float)g_rf_max / 100.0f) want = (float)g_rf_max / 100.0f;
    }
    if (fabsf(want - g_rf_cur) < 0.01f) return;
    for (i = 0; i < g_rf_n; i++) { double nv = g_rf[i].base * (double)want; WriteProcessMemory(GetCurrentProcess(), (void *)g_rf[i].p, &nv, 8, NULL); }
    g_rf_cur = want;
    push_text("FATIGUE chain=%u recovery clips x%.2f", ch, want);
}
static void prince_tick(void *ent, unsigned now)
{
    static unsigned t_last, ep_t0, last_roll, rchain;
    static float px, py, peak, ppx, ppy, rx0, ry0;
    static int have, moving, in_roll;
    rf_apply(now);
    HANDLE me = GetCurrentProcess();
    void *obj = NULL;
    uint32_t sref = 0, fl = 0, tgt = 0;
    float pos[3], v, dt;
    if (age_ms(now, t_last) < 50u) return;
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x38, &obj, 4, NULL) || !obj) return;
    if (!ReadProcessMemory(me, (uint8_t *)obj + 0xC, &sref, 4, NULL) || !sref) return;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)sref + 8, &fl, 4, NULL) || !(fl & 0x10000000u)) return;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)sref, &tgt, 4, NULL) || !tgt) return;
    g_ptgt = tgt;
    if (!ReadProcessMemory(me, (uint8_t *)(uintptr_t)tgt + 0x40, pos, sizeof pos, NULL)) return;
    dt = (float)age_ms(now, t_last) / 1000.0f;
    v = have && dt < 0.4f ? sqrtf((pos[0] - px) * (pos[0] - px) + (pos[1] - py) * (pos[1] - py)) / dt : 0.0f;
    ppx = px; ppy = py;
    t_last = now; px = pos[0]; py = pos[1]; have = 1;
    {   /* LUNGE: position of the Prince every 50 ms for 1.6 s after a held left mouse is released */
        static unsigned lt; static float x0, y0; static int armed;
        if (g_rel_t && g_rel_t != lt) { lt = g_rel_t; armed = 0; }
        if (lt && age_ms(now, lt) <= 1600u) {
            if (!armed) { x0 = pos[0]; y0 = pos[1]; armed = 1; }
            push_text("LUNGE +%ums moved=%.2f v=%.1f (hold=%ums)", age_ms(now, lt), sqrtf((pos[0]-x0)*(pos[0]-x0)+(pos[1]-y0)*(pos[1]-y0)), v, g_rel_hold);
        }
    }
    if (v > 60.0f) return;                                             /* teleport / loading */
    if (!moving && v >= 2.5f) { moving = 1; ep_t0 = now; peak = v; }
    else if (moving) {
        if (v > peak) peak = v;
        if (v < 1.5f) { moving = 0; if (peak >= 4.0f) push_text("PRINCE-MOVE peak=%.1f dur=%ums", peak, age_ms(now, ep_t0)); }
    }
    if (v >= g_cfg.roll_speed && !in_roll) {
        in_roll = 1; rx0 = have ? ppx : pos[0]; ry0 = have ? ppy : pos[1];
        if (last_roll && age_ms(now, last_roll) <= (unsigned)g_cfg.roll_window) rchain++; else rchain = 1;
        last_roll = now | 1u;
        g_fr_rolls++;
        push_text("ROLL? v=%.1f chain=%u", v, rchain);
        g_rchain_pub = rchain; g_rlast_pub = last_roll;
        if (g_cfg.roll_on && rchain >= (unsigned)g_cfg.roll_from) { g_roll_until = now + (unsigned)g_cfg.roll_ms; push_text("ROLL reaction for %d ms", g_cfg.roll_ms); }
    } else if (v < g_cfg.roll_speed * 0.6f) {
        if (in_roll) {
            in_roll = 0;
            { float dx = pos[0] - rx0, dy = pos[1] - ry0; if (dx * dx + dy * dy >= 2.25f) { float fa = 0.0f; hab_add(dx, dy); if (hab_dir(&fa)) push_text("ROLL-HABIT: the Prince likes to roll toward %.0f deg (sector weights %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f)", fa * 57.2958f, g_hab[0], g_hab[1], g_hab[2], g_hab[3], g_hab[4], g_hab[5], g_hab[6], g_hab[7]); } }
            if (rchain >= 2u) { g_pun_from = (now + 300u) | 1u; g_pun_until = now + 1400u; g_pun_budget = 1; g_pun_dir = 1; push_text("ROLL-TRAP: roll #%u ended, the nearest enemy may strike in 0.3 s", rchain); }
            if (g_rl_on && rchain >= 2u) {
                int ex = g_rl_step * (int)(rchain - 1u); if (ex > g_rl_max) ex = g_rl_max;
                if (ex > 0) { g_rb_until = (now + (unsigned)ex) | 1u; push_text("ROLL-LAG roll #%u ended: keys blocked %d ms (blocked so far %u, DI calls state=%u data=%u)", rchain, ex, g_di_blocked, g_di_state_calls, g_di_data_calls); }
            }
        }
    }
}

typedef struct { uint32_t ent; unsigned t, tlog; } DLog;
static DLog g_dlog[64];

static void dist_log(void *ent)
{
    unsigned now = now_ms();
    int i, slot = -1;
    for (i = 0; i < 64; i++) {
        if (g_dlog[i].ent == (uint32_t)(uintptr_t)ent) { slot = i; break; }
        if (slot < 0 && (g_dlog[i].ent == 0 || now - g_dlog[i].t > 10000u)) slot = i;
    }
    if (slot >= 0 && (g_dlog[slot].ent != (uint32_t)(uintptr_t)ent || now - g_dlog[slot].t >= 300u)) {
        float raw;
        g_dlog[slot].ent = (uint32_t)(uintptr_t)ent; g_dlog[slot].t = now;
        if (probe_dist(ent, &raw)) {
            float d = raw > 0 ? sqrtf(raw) : 0.0f;
            seen_set_dist((uint32_t)(uintptr_t)ent, d);
            if (g_phase != PH_IDLE || g_fs > 0.0f || age_ms(now, g_dlog[slot].tlog) >= 1000u) {
                g_dlog[slot].tlog = now;
                push_text("DIST ent=%08lX raw=%.2f sqrt=%.2f f=%.2f ph=%d v1c=%.3f", (unsigned long)(uintptr_t)ent, raw, d, g_f, g_phase, g_probe_v1c);
            }
        }
    }
}


/* v2.8: the chosen attacker must pass the game's own check, not only our 'yes'.
 * From the code dump: the attack decision for kind 0 is function 0x00768260 (edi = behavior object - 0x88). It remembers its answer for the current frame
 * at object+0x1d0/0x1d4 (frame numbers, copy of globals 0x11A2120/24) and object+0x1d8 (answer byte). It compares a distance from the enemy's component
 * with a reach read from the enemy data (ebx+0x13c, in another branch ebx+0x140), where ebx = component(vtable slot 1). Presumably the attack code later uses that remembered answer.
 * So for the chosen enemy we raise the reach for one call, let the game decide by itself (all its other checks stay), then put the reach back. */
static void tryreal_log(const char *what, void *ent, int a, int b, float r1, float r2)
{
    static int n; if (n++ >= 80) return;
    push_text("REAL %s ent=%08lX first=%d second=%d reach13c=%.2f reach140=%.2f", what, (unsigned long)(uintptr_t)ent, a, b, r1, r2);
}
static uint8_t chosen_real_attack(void *self, void *ent)
{
    HANDLE me = GetCurrentProcess();
    uint32_t obj = 0, outer, actor = 0, comp, vt = 0, getter = 0, eb = 0, fr0 = 0, fr1 = 0;
    float r1 = 0, r2 = 0, big = g_cfg.des_reach;
    uint8_t res = 0, zero4[4] = {0, 0, 0, 0};
    if (!ReadProcessMemory(me, (uint8_t *)ent + 0x34, &obj, 4, NULL) || !obj) return 0;
    outer = obj - 0x88u;
    if (!ReadProcessMemory(me, (void *)(uintptr_t)(outer + 0xF8), &actor, 4, NULL) || !actor) { tryreal_log("no-actor", ent, 0, 0, 0, 0); return 0; }
    comp = actor + 0x4FC;
    if (big > 1.0f && ReadProcessMemory(me, (void *)(uintptr_t)comp, &vt, 4, NULL) && vt && ReadProcessMemory(me, (void *)(uintptr_t)(vt + 4), &getter, 4, NULL) && getter >= 0x401000u && getter < 0x1000000u) {
        eb = (uint32_t)(uintptr_t)((uint32_t (THISCALL *)(void *))(uintptr_t)getter)((void *)(uintptr_t)comp);
    }
    if (eb > 0x10000u && ReadProcessMemory(me, (void *)(uintptr_t)(eb + 0x13C), &r1, 4, NULL) && ReadProcessMemory(me, (void *)(uintptr_t)(eb + 0x140), &r2, 4, NULL)) {
        float o1 = r1, o2 = r2;
        if (o1 < big) WriteProcessMemory(me, (void *)(uintptr_t)(eb + 0x13C), &big, 4, NULL);
        if (o2 < big) WriteProcessMemory(me, (void *)(uintptr_t)(eb + 0x140), &big, 4, NULL);
        WriteProcessMemory(me, (void *)(uintptr_t)(outer + 0x1D0), zero4, 4, NULL);   /* forget the remembered answer of this frame */
        res = o_CB(self, ent);
        WriteProcessMemory(me, (void *)(uintptr_t)(eb + 0x13C), &o1, 4, NULL);
        WriteProcessMemory(me, (void *)(uintptr_t)(eb + 0x140), &o2, 4, NULL);
        tryreal_log("reach", ent, 0, res, o1, o2);
    } else {
        tryreal_log("no-reach", ent, 0, 0, 0, 0);
    }
    if (!res && g_cfg.des_cache && ReadProcessMemory(me, (void *)0x11A2120, &fr0, 4, NULL) && ReadProcessMemory(me, (void *)0x11A2124, &fr1, 4, NULL)) {
        uint8_t one = 1;
        WriteProcessMemory(me, (void *)(uintptr_t)(outer + 0x1D0), &fr0, 4, NULL);
        WriteProcessMemory(me, (void *)(uintptr_t)(outer + 0x1D4), &fr1, 4, NULL);
        WriteProcessMemory(me, (void *)(uintptr_t)(outer + 0x1D8), &one, 1, NULL);
        res = 1;
        tryreal_log("cache", ent, 0, 1, r1, r2);
    }
    return res;
}


/* v2.29: normal fight (ring not wide): at most g_lim_max enemies may start an attack within grant_ms; an enemy that already has a grant may continue.
 * Among the askers the one who attacked longest ago goes first is not tracked here: first come, first served (the game asks every frame). */
static int lim_gate(uint32_t ent, unsigned now)
{
    int i, active = 0, freei = -1;
    for (i = 0; i < 8; i++) {
        if (g_lim[i].ent && age_ms(now, g_lim[i].t) < (unsigned)g_lim_ms) {
            if (g_lim[i].ent == ent) return 1;
            active++;
        } else if (freei < 0) freei = i;
    }
    if (active >= g_lim_max || freei < 0) return 0;
    g_lim[freei].ent = ent; g_lim[freei].t = now | 1u;
    return 1;
}
static uint8_t HOOKCALL h_CB(void *self, void *edx, void *ent)
{
    uint8_t r = o_CB(self, ent);
    uint32_t p = *(uint32_t *)((uint8_t *)self + 8);
    (void)edx;
    seen_touch((uint32_t)(uintptr_t)ent, now_ms());
    prince_tick(ent, now_ms());
    dist_log(ent);
    watch_tick(ent, now_ms());
    map_tick(now_ms());
    spd_tick(ent, now_ms()); ang_tick(now_ms());
    if (p == 5) watch_other(ent, now_ms());
    if (p == 5) {
        uint8_t nat = r; des_update(ent, now_ms(), &r);
        if (!nat && r && g_des_ent == (uint32_t)(uintptr_t)ent) r = chosen_real_attack(self, ent);   /* v2.8: not a bare 'yes' */
        beh_log(ent, nat, g_des_ent == (uint32_t)(uintptr_t)ent);
    }
    if (p == 5 && r) g_stat_asks++;
    if (g_wnd_end_t && (int)(now_ms() - g_wnd_end_t) >= 0) { g_wnd_end_t = 0; g_relayout = 1; }
    if (g_relayout && p == 5) relayout_now();
    if (p == 5 && r && wounded_now((uint32_t)(uintptr_t)ent)) {       /* v2.53: a wounded enemy does not attack, unless the Prince is right next to him (then he strikes first) */
        float dw2 = seen_get_dist((uint32_t)(uintptr_t)ent); static unsigned n_wd, t_wd;
        if (dw2 < 0 || dw2 > g_wnd_near) { r = 0; n_wd++; if (age_ms(now_ms(), t_wd) >= 2000u) { t_wd = now_ms() | 1u; push_text("WOUNDED attacks refused %u so far (last ent=%08lX dist=%.2f)", n_wd, (unsigned long)(uintptr_t)ent, dw2); } }
    }
    if (g_cfg.block_attacks && p == 5 && r) { unsigned t = now_ms(); if (g_wide && !hold_attack_gate((uint32_t)(uintptr_t)ent, t)) { r = 0; g_stat_blocked++; note(9, "HOLD", ent, 5, 0, 0, 0); } }
    if (p == 5 && !r && g_ml_on && g_des_ent != (uint32_t)(uintptr_t)ent && (!g_wide || (g_sr_sq && ent_kind((uint32_t)(uintptr_t)ent) == 2)) && !wounded_now((uint32_t)(uintptr_t)ent)) {      /* v2.38: an enemy that stands in sword range but the game still says no (it is in its pause): push him to swing, at most once per interval */
        float dm = seen_get_dist((uint32_t)(uintptr_t)ent);
        unsigned nowm = now_ms(); int k, fi = -1, ok = 1;
        if (dm > 0.0f && dm <= g_ml_dist) {
            for (k = 0; k < 16; k++) { if (g_ml[k].ent == (uint32_t)(uintptr_t)ent) { fi = k; if (age_ms(nowm, g_ml[k].t) < (unsigned)g_ml_ms) ok = 0; break; } if (fi < 0 && (!g_ml[k].ent || age_ms(nowm, g_ml[k].t) > 5000u)) fi = k; }
            if (ok && fi >= 0 && chosen_real_attack(self, ent)) { r = 1; g_ml[fi].ent = (uint32_t)(uintptr_t)ent; g_ml[fi].t = nowm | 1u; g_ml_forced++; }
        }
    }
    if (p == 5 && r && g_rg_on && g_des_ent != (uint32_t)(uintptr_t)ent) {      /* v2.32: no swing from too far: he walks in first */
        float dg = seen_get_dist((uint32_t)(uintptr_t)ent);
        if (dg > g_rg_dist) { static unsigned t_g; r = 0; g_rg_refused++; if (age_ms(now_ms(), t_g) >= 2000u) { t_g = now_ms() | 1u; push_text("REACH-GATE %u swings refused so far (last dist=%.1f)", g_rg_refused, dg); } }
    }
    if (p == 5 && r && g_lim_on && !g_wide && !kill_open() && g_des_ent != (uint32_t)(uintptr_t)ent && !lim_gate((uint32_t)(uintptr_t)ent, now_ms())) {
        static unsigned t_l;
        r = 0; g_lim_refused++;
        if (age_ms(now_ms(), t_l) >= 2000u) { t_l = now_ms() | 1u; push_text("LIMIT %u attacks refused so far (max %d at once)", g_lim_refused, g_lim_max); }
    }
    if (p == 5 && r && g_cfg.wave_on && g_rel_t && g_des_ent != (uint32_t)(uintptr_t)ent && age_ms(now_ms(), g_rel_t) < (unsigned)g_cfg.wave_ms) {
        float dw = seen_get_dist((uint32_t)(uintptr_t)ent);
        if (dw > g_cfg.wave_dist) {      /* v2.13: just after a charge only the close ones attack; the others keep their distance */
            static unsigned n_w, t_w;
            r = 0; n_w++;
            if (age_ms(now_ms(), t_w) >= 1000u) { t_w = now_ms() | 1u; push_text("WAVE-HOLD %u attacks refused so far (last ent=%08lX dist=%.1f)", n_w, (unsigned long)(uintptr_t)ent, dw); }
        }
    }
    if (p == 5 && r && g_sf_on && g_wide && g_des_ent != (uint32_t)(uintptr_t)ent && g_n_shield > 0) {      /* v2.98: shield-bearers first */
        static unsigned t_in, t_seen, t_log;
        unsigned nw = now_ms(); float ns = nearest_shield(nw);
        if (ns > 0.0f && ns <= g_sf_dist) {
            if (age_ms(nw, t_seen) > 800u) t_in = nw | 1u;
            t_seen = nw | 1u;
            if (age_ms(nw, t_in) < (unsigned)g_sf_win && ent_kind((uint32_t)(uintptr_t)ent) == 1) {
                float ds = seen_get_dist((uint32_t)(uintptr_t)ent);
                if (ds > g_sf_free) {
                    r = 0; g_sf_ref++;
                    if (age_ms(nw, t_log) >= 1000u) { t_log = nw | 1u; push_text("SHIELD-FIRST %u soldier attacks refused so far (soldier %.1f, nearest shield %.1f)", g_sf_ref, ds, ns); }
                }
            }
        }
    }
    if (p == 5 && !r && !g_wide && g_pun_budget > 0 && (int)(g_pun_until - now_ms()) > 0 && (!g_pun_from || (int)(now_ms() - g_pun_from) >= 0) && !wounded_now((uint32_t)(uintptr_t)ent)) {      /* v2.76: the answer is a real swing by a near enemy, not only a narrower ring */
        float dp = seen_get_dist((uint32_t)(uintptr_t)ent);
        int dir_ok = 1;
        if (g_pun_dir) {      /* v2.82: the roll trap: the enemy who stands on the side the Prince likes to roll to cuts off his way */
            float fa = 0.0f, P[3], Q[3];
            if (hab_dir(&fa) && prince_pos(ent, P) && enemy_pos(ent, Q)) {
                float b = atan2f(Q[1] - P[1], Q[0] - P[0]), df = b - fa;
                while (df > 3.14159265f) df -= 6.2831853f;
                while (df < -3.14159265f) df += 6.2831853f;
                dir_ok = fabsf(df) <= 1.22f;      /* 70 degrees */
            }
        }
        if (dir_ok && dp > 0.0f && dp <= 4.5f && chosen_real_attack(self, ent)) { r = 1; g_pun_budget--; g_fr_forced++; push_text("PUNISH: swing forced for ent=%08lX dist=%.2f (left %d)", (unsigned long)(uintptr_t)ent, dp, g_pun_budget); }
    }
    if (p == 5 && !r && !g_wide && g_wnd_on && wound_return_due((uint32_t)(uintptr_t)ent)) {      /* v2.81: a wounded enemy that comes back after his pause strikes first */
        float dr = seen_get_dist((uint32_t)(uintptr_t)ent);
        if (dr > 0.0f && dr <= 4.5f && chosen_real_attack(self, ent)) { r = 1; g_fr_forced++; wound_return_done((uint32_t)(uintptr_t)ent); push_text("WOUND-RETURN: ent=%08lX comes back and strikes first (dist=%.2f)", (unsigned long)(uintptr_t)ent, dr); }
    }
    if (p == 5 && r && g_sr_on && g_n_shield > 0 && g_wide && g_des_ent != (uint32_t)(uintptr_t)ent && ent_kind((uint32_t)(uintptr_t)ent) == 1) {      /* v2.99: with a shield-bearer in the crowd a soldier does not start an attack, unless the Prince is right in front of him */
        float dq = seen_get_dist((uint32_t)(uintptr_t)ent);
        if (!(dq >= 0.0f && dq <= g_sr_free)) {
            static unsigned t_q; r = 0; g_sr_ref++;
            if (age_ms(now_ms(), t_q) >= 1500u) { t_q = now_ms() | 1u; push_text("SHIELD-RESERVE %u soldier attacks refused so far (soldier %.1f, shields %d, soldiers %d)", g_sr_ref, dq, g_n_shield, g_n_other); }
        }
    }
    note(3, "CB", ent, p, 0, 0, r);
    return r;
}
static uint8_t HOOKCALL h_MM(void *self, void *edx, void *ent)
{
    uint8_t r = o_MM(self, ent); (void)edx;
    relayout_poll(); chosen_ring_push(ent);
    watch_tick(ent, now_ms()); watch_answer(0, ent, self, r); fq_note(0, self, ent, r);
    note(4, "MM", ent, *(uint32_t *)((uint8_t *)self + 8), 0, 0, r);
    return r;
}
static uint8_t HOOKCALL h_TQ(void *self, void *edx, void *ent)
{
    chosen_ring_push(ent);
    uint8_t r = o_TQ(self, ent); (void)edx;
    watch_tick(ent, now_ms()); watch_answer(3, ent, self, r); fq_note(3, self, ent, r);
    note(5, "TQ", ent, *(uint32_t *)((uint8_t *)self + 8), 0, 0, r);
    return r;
}
static uint8_t HOOKCALL h_TD(void *self, void *edx, void *ent)
{
    uint8_t r = o_TD(self, ent); (void)edx;
    watch_tick(ent, now_ms()); watch_answer(1, ent, self, r); fq_note(1, self, ent, r);
    note(6, "TD", ent, *(uint32_t *)((uint8_t *)self + 8), *(uint32_t *)((uint8_t *)self + 0xC),
         *(float *)((uint8_t *)self + 0x10), r);
    return r;
}

typedef struct { const char *name; uint32_t vtable; uint32_t expect; void *hook; EvalFn *orig; } Target;
static Target g_targets[] = {
    { "FightAttackCondition",         0x00FC9C20, 0x006E8280, (void *)h_FA, &o_FA },
    { "FightRingQueryCondition",      0x00FC9C70, 0x006E8670, (void *)h_FR, &o_FR },
    { "CombatBehaviorCondition",      0x00FC9BD0, 0x006E8020, (void *)h_CB, &o_CB },
    { "MoveModeCondition",            0x00FC9DB0, 0x006E9210, (void *)h_MM, &o_MM },
    { "FightTargetQueryCondition",    0x00FC9D10, 0x006E8E30, (void *)h_TQ, &o_TQ },
    { "FightTargetDistanceCondition", 0x00FC9CC0, 0x006E8B20, (void *)h_TD, &o_TD },
};


/* v2.50: EnemyDamageHandler (class hash B855849F, vtable 0x00FCF9D0). Slot 22 = 0x006FC6B0: thiscall, 1 stack arg (the damage event). ecx = handler, handler+0x60 = the actor,
 * event+0x9C = damage type (0x27/0x28/0x38 seen in the code), return = al.  We call the original and log every call: this is the exact "enemy receives a hit" moment. */
typedef uint8_t (THISCALL *DhFn)(void *self, void *ev);
typedef struct { uint32_t ent; unsigned n, t, last; } WHit;
static WHit g_wh[48];
static int wounded_now(uint32_t ent) { int j; if (!g_wnd_on) return 0; if (g_n_shield + g_n_other <= g_wnd_min) return 0; for (j = 0; j < 48; j++) if (g_wh[j].ent == ent) return g_wh[j].n >= (unsigned)g_wnd_hits && age_ms(now_ms(), g_wh[j].t) < (unsigned)g_wnd_ms; return 0; }
static int recent_hit_ent(uint32_t ent, unsigned within) { int j; for (j = 0; j < 48; j++) if (g_wh[j].ent == ent) return g_wh[j].t && age_ms(now_ms(), g_wh[j].t) <= within; return 0; }
static int wound_return_due(uint32_t ent)
{
    int j;
    for (j = 0; j < 48; j++) if (g_wh[j].ent == ent) {
        unsigned a = g_wh[j].t ? age_ms(now_ms(), g_wh[j].t) : 0u;
        return g_wh[j].n >= (unsigned)g_wnd_hits && a >= (unsigned)g_wnd_ms && a <= (unsigned)g_wnd_ms + 2500u && g_wh[j].last != g_wh[j].t && g_n_shield + g_n_other > g_wnd_min;
    }
    return 0;
}
static void wound_return_done(uint32_t ent) { int j; for (j = 0; j < 48; j++) if (g_wh[j].ent == ent) { g_wh[j].last = g_wh[j].t; return; } }
static DhFn o_DH;
static volatile unsigned g_dh_calls, g_dh_ok;
static uint8_t HOOKCALL h_DH(void *self, void *edx, void *ev)
{
    uint8_t r; uint32_t actor = 0, type = 0, st0 = 0, st1 = 0; int i; uint32_t ent = 0; HANDLE me = GetCurrentProcess();
    (void)edx;
    ReadProcessMemory(me, (uint8_t *)self + 0x60, &actor, 4, NULL);
    ReadProcessMemory(me, (uint8_t *)ev + 0x9C, &type, 4, NULL);
    if (actor) ReadProcessMemory(me, (uint8_t *)(uintptr_t)actor + 0xF8C, &st0, 4, NULL);
    r = o_DH(self, ev);
    if (actor) ReadProcessMemory(me, (uint8_t *)(uintptr_t)actor + 0xF8C, &st1, 4, NULL);
    g_dh_calls++;
    for (i = 0; i < NSEEN; i++) {
        uint32_t obj = 0;
        if (!g_seen[i].ent) continue;
        if (ReadProcessMemory(me, (void *)(uintptr_t)(g_seen[i].ent + 0x38), &obj, 4, NULL) && obj && obj - 0x4F8u == actor) { ent = g_seen[i].ent; break; }
    }
    if (ent && type == 11) {                       /* v2.51: sword hit (type 11 = hit confirmed by 'HIT n=' on shield-bearers) */
        int k = ent_kind(ent), j, slot = -1;
        g_last_hit_t = now_ms() | 1u; g_fr_hits++;
        if (k) {
            for (j = 0; j < 48; j++) { if (g_wh[j].ent == ent) { slot = j; break; } if (slot < 0 && (!g_wh[j].ent || age_ms(now_ms(), g_wh[j].t) > 60000u)) slot = j; }
            if (slot >= 0) {
                if (g_wh[slot].ent != ent) { g_wh[slot].ent = ent; g_wh[slot].n = 0; g_wh[slot].t = 0; }
                if (g_wh[slot].t && age_ms(now_ms(), g_wh[slot].t) > (unsigned)g_wnd_ms + 5000u) g_wh[slot].n = 0;          /* healed: 20 s without a hit */
                if (g_wh[slot].t && age_ms(now_ms(), g_wh[slot].t) < 250u) goto wnd_skip;                /* one swing = one hit (several events within 250 ms) */
                g_wh[slot].n++; g_wh[slot].t = now_ms() | 1u;
                if (wounded_now(ent) && g_wnd_ring) { g_relayout = 1; g_wnd_end_t = (now_ms() + (unsigned)g_wnd_ms + 150u) | 1u; }
                push_text("WND hit n=%u ent=%08lX kind=%s dist=%.2f%s", g_wh[slot].n, (unsigned long)ent, k == 2 ? "shield" : "soldier", seen_get_dist(ent), wounded_now(ent) ? " WOUNDED" : "");
            }
        }
        wnd_skip: ;
    }
    if (g_dh_ok < 1200) {
        uint32_t w[8]; unsigned dn = g_lmb_dn_t ? age_ms(now_ms(), g_lmb_dn_t) : 99999u;
        memset(w, 0, sizeof w); ReadProcessMemory(me, (uint8_t *)ev + 0x90, w, sizeof w, NULL);
        g_dh_ok++;
        push_text("DMG actor=%08lX ent=%08lX type=%lu ret=%u state %lu->%lu ev90=%08lX %08lX %08lX %08lX %08lX %08lX lmb_ago=%ums dist=%.2f", (unsigned long)actor, (unsigned long)ent, (unsigned long)type, r,
                  (unsigned long)st0, (unsigned long)st1, (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5], dn, ent ? seen_get_dist(ent) : -1.0f);
    }
    return r;
}

/* v3.07: observe-only wrapper of the Spawner method 0xA2B9F0 (thiscall, one argument, ret 4; it calls the spawn core 0xA2BBF0).  Logs who spawns what and when,
 * so a reinforcement call can later copy the game's own call.  It changes nothing. */
typedef uint32_t (THISCALL *SpFn)(void *self, uint32_t arg);
static SpFn o_SP; static volatile unsigned g_sp_calls;
static uint32_t HOOKCALL h_SP(void *self, void *edx, uint32_t arg)
{
    uint32_t r; unsigned n;
    (void)edx;
    n = ++g_sp_calls;
    r = o_SP(self, arg);
    if (n <= 80u) {
        uint32_t w[12]; memset(w, 0, sizeof w);
        if (!IsBadReadPtr(self, sizeof w)) memcpy(w, self, sizeof w);
        push_text("SPAWN#%u this=%p arg=%08lX -> %08lX vt=%08lX f4=%08lX f8=%08lX f0c=%08lX f10=%08lX enemies=%d/%d", n, self, (unsigned long)arg, (unsigned long)r,
                  (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[4], g_n_shield, g_n_other);
    }
    return r;
}
static void install_spawn_hook(void)
{
    static const uint32_t slots[3] = {0xFB3B94u, 0xFB45BCu, 0x1001A2Cu}; int i, ok = 0;
    for (i = 0; i < 3; i++) {
        uint32_t *slot = (uint32_t *)(uintptr_t)slots[i]; DWORD old;
        if (IsBadReadPtr(slot, 4) || *slot != 0x00A2B9F0u) { logf_("[observer] SKIP spawner slot %08lX (is %08lX)\n", (unsigned long)slots[i], IsBadReadPtr(slot, 4) ? 0UL : (unsigned long)*slot); continue; }
        if (!o_SP) o_SP = (SpFn)(uintptr_t)*slot;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) continue;
        *slot = (uint32_t)(uintptr_t)h_SP; VirtualProtect(slot, 4, old, &old); ok++;
    }
    logf_("[observer] spawner method wrapped in %d vtables\n", ok);
}
static void install_dmg_hook(void)
{
    uint32_t *slot = (uint32_t *)(uintptr_t)(0x00FCF9D0u + 22 * 4); DWORD old;
    if (IsBadReadPtr(slot, 4) || *slot != 0x006FC6B0u) { logf_("[observer] SKIP EnemyDamageHandler: slot is %08lX\n", IsBadReadPtr(slot, 4) ? 0UL : (unsigned long)*slot); return; }
    o_DH = (DhFn)(uintptr_t)*slot;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { logf_("[observer] VirtualProtect failed for EnemyDamageHandler\n"); return; }
    *slot = (uint32_t)(uintptr_t)h_DH; VirtualProtect(slot, 4, old, &old);
    logf_("[observer] wrapped EnemyDamageHandler slot 22 (006FC6B0)\n");
}

static int code_ready(void)
{
    size_t i;
    for (i = 0; i < sizeof g_targets / sizeof g_targets[0]; i++) {
        const uint8_t *c = (const uint8_t *)(uintptr_t)g_targets[i].expect;
        if (IsBadReadPtr(c, 4)) return 0;
        if (!(c[0] == 0x55 || c[0] == 0x53 || c[0] == 0x56 || c[0] == 0x57)) return 0;
        if (c[0] == 0x55 && c[1] != 0x8B) return 0;
    }
    return 1;
}


/* v2.29: the game gives an enemy a fight-ring slot only if the straight line to it passes no other enemy's slot closer than 1.5 units (function 0x59A8E0).
 * Enemies at the back of a crowd then get no slot and stand still although they are in "go into slot" mode. The patch makes the test say "clear". */
static void patch_line_test(void)
{
    uint8_t *p = (uint8_t *)0x59A8E0;
    static const uint8_t orig[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    DWORD old;
    if (memcmp(p, orig, 6) != 0) { logf_("[observer] line test patch skipped: unexpected bytes at 0059A8E0\n"); return; }
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) { logf_("[observer] line test patch: VirtualProtect failed\n"); return; }
    p[0] = 0xB0; p[1] = 0x01; p[2] = 0xC2; p[3] = 0x14; p[4] = 0x00;     /* mov al,1 ; ret 0x14 */
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf_("[observer] straight-line test of slot assignment switched off (0059A8E0)\n");
}
/* v2.35: flanking. The game picks for an enemy the free ring slot with the smallest squared distance to him (function 0x599810, call of 0x59B200 at 0x599958),
 * so a crowd forms on the side it comes from. Here every enemy gets a random favourite slot number; slots far from it (counted in slots around the ring) look farther away
 * by up to 'penalty' game units. So enemies walk round the Prince and also come from the side and from behind. */
uint32_t flank_orig = 0x59B200;
/* v2.54: who gets the inner ring. The game gives a place to whoever asks first (ring 0 is tried before 1 and 2), not to the nearest, so in a pulse an ordinary soldier takes the inner
 * place and the chosen shield-bearer is left outside. In a mixed crowd soldiers get cost = FLT_MAX for ring 0 slots: the game then sees "no slot" and tries ring 1. */
typedef struct { uint32_t key; int kind; unsigned t; uint32_t ent; } KeyKind;
static KeyKind g_kk[64];
static uint32_t g_kk_ent;     /* ent found by the last key_kind() call (0 = unknown) */
static int key_kind(uint32_t key)
{
    unsigned now = now_ms(); int i, free_i = -1; HANDLE me = GetCurrentProcess();
    for (i = 0; i < 64; i++) {
        if (g_kk[i].key == key && age_ms(now, g_kk[i].t) < 2000u) { g_kk_ent = g_kk[i].ent; return g_kk[i].kind; }
        if (free_i < 0 && (!g_kk[i].key || age_ms(now, g_kk[i].t) >= 2000u)) free_i = i;
    }
    {
        int kind = 0; uint32_t fent = 0;
        for (i = 0; i < NSEEN && !kind; i++) {
            uint32_t ent = g_seen[i].ent, obj = 0, w[0x70], k;
            if (!ent) continue;
            ReadProcessMemory(me, (void *)(uintptr_t)(ent + 0x38), &obj, 4, NULL);
            if (key == ent || (obj && (key == obj || key == obj - 0x4F8u))) { kind = ent_kind(ent); fent = ent; break; }
            memset(w, 0, sizeof w);
            ReadProcessMemory(me, (void *)(uintptr_t)ent, w, 0x1C0, NULL);            /* every pointer inside the entity block */
            for (k = 0; k < 0x70; k++) if (w[k] == key) { kind = ent_kind(ent); fent = ent; break; }
            if (!kind && obj) {
                memset(w, 0, sizeof w);
                ReadProcessMemory(me, (void *)(uintptr_t)obj, w, 0x100, NULL);
                for (k = 0; k < 0x40; k++) if (w[k] == key) { kind = ent_kind(ent); fent = ent; break; }
            }
        }
        g_kk_ent = fent;
        if (free_i >= 0) { g_kk[free_i].key = key; g_kk[free_i].kind = kind; g_kk[free_i].t = now | 1u; g_kk[free_i].ent = fent; }
        if (!kind) g_mx_key_miss++;
        return kind;
    }
}
volatile unsigned g_dbg_r1_any, g_dbg_r1_ch, g_dbg_r2_ch, g_dbg_r1_inf; volatile float g_dbg_r1_min = 1e30f;
uint32_t __attribute__((cdecl)) flank_adjust(uint32_t slot, uint32_t ent, uint32_t ring, uint32_t sbits, uint32_t xkey)
{
    union { uint32_t u; float f; } v;
    uint32_t count, idx, h, pref, d;
    float dn;
    v.u = sbits;
    uint32_t xk = (xkey >= 0x10000u) ? xkey : ent;      /* v3.04: xkey is the 'swap' flag 0/1 of 0x599810 ([ebp+0xC]), never a pointer: every branch that tested it was dead code; the enemy is the second argument */
    if (g_wnd_on && g_wnd_ring && slot >= 0x10000u && ring >= 0x10000u && xk >= 0x10000u) {      /* v2.58: a wounded enemy gets no place in the two inner rings: the game puts him into the outer ring */
        uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r0 = 0, r1 = 0;
        if (mgr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)(mgr + 0x40), 4)) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x40);
        if (arr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)arr, 8)) { r0 = *(volatile uint32_t *)(uintptr_t)arr; r1 = *(volatile uint32_t *)(uintptr_t)(arr + 4); }
        if (r0 && (ring == r0 || ring == r1)) { key_kind(xk); if (g_kk_ent && wounded_now(g_kk_ent)) { static unsigned t_wr; g_wnd_ring_block++; if (age_ms(now_ms(), t_wr) >= 1000u) { t_wr = now_ms() | 1u; push_text("WND-RING wounded ent=%08lX kept out of inner rings (%u so far, dist=%.2f)", (unsigned long)g_kk_ent, g_wnd_ring_block, seen_get_dist(g_kk_ent)); } v.u = 0x7F7FFFFFu; return v.u; } }
    }
    if (g_sd_on && g_wide && slot >= 0x10000u && ring >= 0x10000u && xk >= 0x10000u && v.f >= 0.0f && v.f < 1.0e8f) {      /* v2.92 */
        uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r0 = 0, r1 = 0;
        if (mgr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)(mgr + 0x40), 4)) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x40);
        if (arr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)arr, 8)) { r0 = *(volatile uint32_t *)(uintptr_t)arr; r1 = *(volatile uint32_t *)(uintptr_t)(arr + 4); }
        if (r0 && (ring == r0 || ring == r1) && key_kind(xk) == 2 && g_kk_ent) {
            float dd = seen_get_dist(g_kk_ent);
            if (dd >= 3.0f && dd <= 14.0f) {
                static unsigned t_sd; float was = v.f;
                v.f = v.f * (float)g_sd_pct / 100.0f; g_sd_n++;
                if (age_ms(now_ms(), t_sd) >= 700u) { t_sd = now_ms() | 1u; push_text("SHIELD-DISCOUNT ent=%08lX dist=%.2f ring=%d cost %.1f -> %.1f (%u so far)", (unsigned long)g_kk_ent, dd, ring == r0 ? 0 : 1, was, v.f, g_sd_n); }
                return v.u;
            }
        }
    }
    if (g_sr_on && g_n_shield > 0 && g_wide && slot >= 0x10000u && ring >= 0x10000u && xk >= 0x10000u) {      /* v2.99: ring 0 is reserved for shield-bearers */
        uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r0 = 0, r1x = 0;
        if (mgr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)(mgr + 0x40), 4)) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x40);
        if (arr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)arr, 8)) { r0 = *(volatile uint32_t *)(uintptr_t)arr; r1x = *(volatile uint32_t *)(uintptr_t)(arr + 4); }
        if (r0 && (ring == r0 || (g_sr_inner && ring == r1x)) && key_kind(xk) == 1) {
            static unsigned t_sr; g_sr_ring++;
            if (age_ms(now_ms(), t_sr) >= 1500u) { t_sr = now_ms() | 1u; push_text("SHIELD-RESERVE ring 0 refused for soldier ent=%08lX (%u so far)", (unsigned long)g_kk_ent, g_sr_ring); }
            v.u = 0x7F7FFFFFu; return v.u;
        }
    }
    if (g_ps_on && g_pulse_run && g_des_ent && slot >= 0x10000u && ring >= 0x10000u && xk >= 0x10000u) {      /* v2.96 */
        uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r0 = 0;
        if (mgr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)(mgr + 0x40), 4)) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x40);
        if (arr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)arr, 4)) r0 = *(volatile uint32_t *)(uintptr_t)arr;
        if (r0 && ring == r0 && key_kind(xk) == 1 && g_kk_ent && g_kk_ent != g_des_ent) {
            static unsigned t_ps; g_ps_ref++;
            if (age_ms(now_ms(), t_ps) >= 1000u) { t_ps = now_ms() | 1u; push_text("PSOLO ring 0 refused for soldier ent=%08lX (%u so far)", (unsigned long)g_kk_ent, g_ps_ref); }
            v.u = 0x7F7FFFFFu; return v.u;
        }
    }
    if (g_evt_on && g_evt_until && (int)(g_evt_until - now_ms()) > 0 && slot >= 0x10000u && ring >= 0x10000u && xkey >= 0x10000u && key_kind(xkey) == 1) { static unsigned n_ev; n_ev++; v.u = 0x7F7FFFFFu; return v.u; }
    if (g_mixed_on && g_wide && g_n_shield > 0 && g_n_other > 0 && slot >= 0x10000u && ring >= 0x10000u && ent >= 0x10000u) {
        uint32_t mgr = *(volatile uint32_t *)0x11A53DC, arr = 0, r0 = 0, r1 = 0, r2 = 0;
        if (mgr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)(mgr + 0x40), 4)) arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x40);
        if (arr >= 0x10000u && !IsBadReadPtr((void *)(uintptr_t)arr, 12)) { r0 = *(volatile uint32_t *)(uintptr_t)arr; r1 = *(volatile uint32_t *)(uintptr_t)(arr + 4); r2 = *(volatile uint32_t *)(uintptr_t)(arr + 8); }
        if (r1 && ring == r1) g_dbg_r1_any++;
        if (g_des_ent && r1 && (ring == r1 || ring == r2) && key_kind(xkey) == 2 && g_kk_ent == g_des_ent) {
            if (ring == r1) { g_dbg_r1_ch++; if (!(v.f < 1.0e8f)) g_dbg_r1_inf++; else if (v.f < g_dbg_r1_min) g_dbg_r1_min = v.f; } else g_dbg_r2_ch++;
        }
        if (r0 && (ring == r0 || (g_pulse_run && ring == r1)) && key_kind(xkey) == 1) { g_mx_ring_block++; v.u = 0x7F7FFFFFu; return v.u; }   /* v2.63: during a pulse soldiers get neither of the two inner rings */
        if (r0 && r2 && ring == r2 && g_des_ent && g_pulse_run) {      /* v2.63: the game moves an enemy one ring inwards at a time (2 -> 1 -> 0): only the OUTER ring is closed for the chosen one, not ring 1 */          /* v2.60: the chosen shield-bearer may only take a place in the inner ring: otherwise the game sends him to the outer ring and he walks away */
            key_kind(xkey);
            if (g_kk_ent && g_kk_ent == g_des_ent) { static unsigned t_co; g_mx_chosen_out++; if (age_ms(now_ms(), t_co) >= 1000u) { t_co = now_ms() | 1u; push_text("CHOSEN-RING outer ring refused for ent=%08lX (%u so far)", (unsigned long)g_kk_ent, g_mx_chosen_out); } v.u = 0x7F7FFFFFu; return v.u; }
        }
    }
    if (!g_fl_on || slot < 0x10000u || ring < 0x10000u || ent < 0x10000u) return sbits;
    count = *(volatile uint32_t *)(uintptr_t)(ring + 0x14) & 0x3fffu;
    idx = *(volatile uint32_t *)(uintptr_t)(slot + 8);
    if (count < 3u || count > 200u) return sbits;
    idx %= count;
    h = ((ent >> 4) * 2654435761u) >> 8;
    pref = h % count;
    d = idx > pref ? idx - pref : pref - idx;
    if (d > count - d) d = count - d;
    dn = (float)d / ((float)count * 0.5f);
    if (!(v.f >= 0.0f) || v.f > 1.0e8f) return sbits;
    { float dist = sqrtf(v.f) + g_fl_pen * dn; v.f = dist * dist; }
    g_fl_calls++;
    return v.u;
}
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _flank_stub\n"
    "_flank_stub:\n"
    "  mov edx, dword ptr [esp+0x10]\n"
    "  push edx\n"
    "  push ecx\n"
    "  call dword ptr [_flank_orig]\n"
    "  sub esp, 20\n"
    "  movss dword ptr [esp+12], xmm0\n"
    "  mov eax, dword ptr [esp+20]\n"
    "  mov dword ptr [esp], eax\n"
    "  mov eax, dword ptr [esp+24]\n"
    "  mov dword ptr [esp+4], eax\n"
    "  mov eax, dword ptr [ebp+8]\n"
    "  mov dword ptr [esp+8], eax\n"
    "  mov eax, dword ptr [esp+12]\n"
    "  mov dword ptr [esp+12], eax\n"
    "  mov eax, dword ptr [ebp+0xC]\n"
    "  mov dword ptr [esp+16], eax\n"
    "  call _flank_adjust\n"
    "  movd xmm0, eax\n"
    "  add esp, 28\n"
    "  ret\n"
    ".att_syntax\n"
);
extern void flank_stub(void);
static void patch_flank(void)
{
    uint8_t *p = (uint8_t *)0x599958;
    DWORD old;
    int32_t rel;
    if (!g_fl_on) return;
    if (p[0] != 0xE8 || *(int32_t *)(p + 1) != 0x18A3) { logf_("[observer] flank patch skipped: unexpected bytes at 00599958\n"); return; }
    rel = (int32_t)((uintptr_t)flank_stub - 0x59995Du);
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) { logf_("[observer] flank patch: VirtualProtect failed\n"); return; }
    *(int32_t *)(p + 1) = rel;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf_("[observer] flanking switched on (0059995 8 call), penalty %.1f\n", g_fl_pen);
}
/* v2.40 diagnostic (F4): find animation clip lengths in memory. The game data keeps  <name> ... 'A<4-6 digits>' 00 FE ... and a double (frames) 14 bytes after the 'A'.
 * Logs every clip whose name contains one of the words below, with its address and value. */
static void scan_clips(int verbose)
{
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *a = (uint8_t *)0x10000;
    static uint8_t *buf; static size_t bufsz;
    unsigned found = 0, total = 0;
    HANDLE me = GetCurrentProcess();
    if (verbose) push_text("CLIPSCAN start");
    while (a < (uint8_t *)0x7FFE0000 && VirtualQuery(a, &mbi, sizeof mbi)) {
        uint8_t *base = (uint8_t *)mbi.BaseAddress; size_t sz = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && sz >= 64 && sz < 0x20000000u) {
            size_t i; SIZE_T got = 0;
            if (sz > bufsz) { free(buf); buf = (uint8_t *)malloc(sz); bufsz = buf ? sz : 0; }
            if (buf && ReadProcessMemory(me, base, buf, sz, &got) && got > 40) {
                for (i = 2; i + 24 < got; i++) {
                    if (buf[i] == 'A' && buf[i + 1] >= '0' && buf[i + 1] <= '9' && buf[i + 2] >= '0' && buf[i + 2] <= '9') {
                        size_t k = i + 1, nd = 0;
                        while (k < got && buf[k] >= '0' && buf[k] <= '9') { k++; nd++; }
                        if (nd >= 4 && nd <= 6 && k + 1 < got && buf[k] == 0 && buf[k + 1] == 0xFE) {
                            size_t j = i, n0; char nm[96]; size_t nl; double val;
                            while (j > 0 && (buf[j - 1] == 0 || buf[j - 1] == 0xFE)) j--;
                            n0 = j;
                            while (n0 > 0 && buf[n0 - 1] >= 32 && buf[n0 - 1] < 127 && j - n0 < 90) n0--;
                            nl = j - n0;
                            if (nl >= 3 && i + 14 + 8 <= got) {
                                memcpy(nm, buf + n0, nl); nm[nl] = 0;
                                memcpy(&val, buf + i + 14, 8);
                                total++;
                                if (g_rf_on && g_rf_n < 64 && val >= 3.0 && val <= 80.0) {
                                    static const char *tg[] = { "Roll_To_Combat_Wait", "Roll_To_Wait", "Roll_Tr_Run", "Roll_Tr_Combat_Run", "Roll_Front_Intro", "Roll_Front_Start", "Roll_Front_Left_Start", "Roll_Front_Right_Start", "Roll_Back_Intro", "Roll_Left_Intro", "Roll_Right_Intro", "Run_Roll_Front_Intro", "Run_Roll_Back_Intro", "Run_Roll_Left_Intro", "Run_Roll_Right_Intro", "Run_Roll_Back_Right_Intro", "Roll_Back_Right_Intro", "Roll_Front", "Roll_Front_Left", "Roll_Front_Right", "Crouch_Ilde_To_Roll_Front" };      /* v2.83: 0-3 recovery clips, 4-16 the START-UP of the next roll (a spammed roll goes roll -> roll and never plays the recovery), 17+ the roll bodies (test only) */
                                    int q, nq = g_rf_test ? 21 : 17;
                                    for (q = 0; q < nq; q++) {
                                        size_t tl = strlen(tg[q]);
                                        if (nl >= tl && nl - tl <= 4 && strcmp(nm + nl - tl, tg[q]) == 0 && (nl == tl || !((nm[nl - tl - 1] >= 'A' && nm[nl - tl - 1] <= 'Z') || (nm[nl - tl - 1] >= 'a' && nm[nl - tl - 1] <= 'z') || nm[nl - tl - 1] == '_'))) {
                                            int z, dup = 0; volatile double *ap = (volatile double *)(uintptr_t)(base + i + 14);
                                            for (z = 0; z < g_rf_n; z++) if (g_rf[z].p == ap) dup = 1;
                                            if (!dup) { g_rf[g_rf_n].p = ap; g_rf[g_rf_n].base = val / (double)g_rf_cur; g_rf_n++; push_text("FATIGUE clip %s = %.1f registered", tg[q], val); }
                                        }
                                    }
                                }
                                if (strstr(nm, "Roll") || strstr(nm, "roll") || strstr(nm, "Crouch")) {
                                    if (found < 120) push_text("CLIP %p %s = %.2f", (void *)(base + i + 14), nm, val);
                                    found++;
                                }
                            }
                        }
                    }
                }
            }
        }
        a = base + sz; if (a <= base) break;
    }
    if (verbose) push_text("CLIPSCAN done: %u clips seen, %u roll/crouch matches", total, found);
}
static int install_hooks(void)
{
    size_t i;
    int n = 0;
    for (i = 0; i < sizeof g_targets / sizeof g_targets[0]; i++) {
        Target *t = &g_targets[i];
        uint32_t *slot = (uint32_t *)(uintptr_t)(t->vtable + 17 * 4);
        DWORD old;
        if (IsBadReadPtr(slot, 4) || *slot != t->expect) {
            logf_("[observer] SKIP %s: vtable slot is %08lX, expected %08lX (different game build?)\n",
                  t->name, IsBadReadPtr(slot, 4) ? 0UL : (unsigned long)*slot, (unsigned long)t->expect);
            continue;
        }
        *t->orig = (EvalFn)(uintptr_t)*slot;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { logf_("[observer] VirtualProtect failed for %s\n", t->name); continue; }
        *slot = (uint32_t)(uintptr_t)t->hook;
        VirtualProtect(slot, 4, old, &old);
        logf_("[observer] wrapped %s.Evaluate (%08lX)\n", t->name, (unsigned long)t->expect);
        n++;
    }
    return n;
}

static void flush_trace(void)
{
    LONG t;
    if (!g_trace) { while (g_tail != g_head) g_tail = (g_tail + 1) % RING; return; }   /* logging off: just drain the buffer */
    while (g_tail != g_head) {
        Event e;
        t = g_tail;
        e = g_ring[t];
        g_tail = (t + 1) % RING;
        if (e.kind[0] == 'T' && e.kind[1] == 'X')
            fprintf(g_trace, "%u %s\n", e.t, e.txt);
        else if (e.kind[0] == 'T' && e.kind[1] == 'D')
            fprintf(g_trace, "%u TD ent=%08lX vt=%08lX el=%lu op=%lu dist=%.2f -> %u\n", e.t,
                    (unsigned long)e.ent, (unsigned long)e.entvt, (unsigned long)e.p1, (unsigned long)e.p2, e.f, e.res);
        else
            fprintf(g_trace, "%u %s ent=%08lX vt=%08lX param=%lu -> %u\n", e.t, e.kind,
                    (unsigned long)e.ent, (unsigned long)e.entvt, (unsigned long)e.p1, e.res);
    }
    fflush(g_trace);
}

/* ---- input log: mouse buttons + whitelist of gameplay keys, only while the game window has focus ---- */
typedef struct { int vk; const char *name; } KeyDef;
static const KeyDef g_keys[] = {
    { VK_LBUTTON, "MouseLeft" }, { VK_RBUTTON, "MouseRight" }, { VK_MBUTTON, "MouseMiddle" },
    { VK_XBUTTON1, "MouseX1" }, { VK_XBUTTON2, "MouseX2" },
    { VK_SPACE, "Space" }, { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" }, { VK_TAB, "Tab" },
    { 'E', "E" }, { 'Q', "Q" }, { 'R', "R" }, { 'F', "F" }, { 'C', "C" }, { 'V', "V" }, { 'X', "X" }, { 'Z', "Z" },
};
#define NKEYS ((int)(sizeof g_keys / sizeof g_keys[0]))
static uint8_t g_keystate[NKEYS];

static int game_focused(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void poll_input(void)
{
    int i, focus = game_focused();
    {
        int lmb = focus && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        unsigned t = now_ms();
        if (lmb && !g_lmb_since) {
            g_lmb_since = t | 1u;
            if (g_last_press && age_ms(t, g_last_press) <= (unsigned)g_cfg.spam_gap) g_chain++; else g_chain = 1;
            if (g_rel_t) push_text("PRESS %ums after the last charge release", age_ms(t, g_rel_t));
            g_last_press = t | 1u;
            if (g_chain >= 2) push_text("CLICK chain=%u", g_chain);
        } else if (!lmb) {
            if (g_lmb_since && age_ms(t, g_lmb_since) >= (unsigned)g_cfg.hold_ms) { g_chain = 0; g_rel_hold = age_ms(t, g_lmb_since); g_rel_t = t | 1u; g_fr_holds++; push_text("RELEASE hold=%ums", g_rel_hold); }   /* a charge is not a click */
            g_lmb_since = 0;
        }
    }
    for (i = 0; i < NKEYS; i++) {
        uint8_t down = focus && ((GetAsyncKeyState(g_keys[i].vk) & 0x8000) != 0);
        if (down != g_keystate[i]) {
            g_keystate[i] = down;
            push_text("KEY %s %s", g_keys[i].name, down ? "down" : "up");
            if (g_keys[i].vk == VK_LBUTTON) { if (down) g_lmb_dn_t = now_ms() | 1u; else g_lmb_up_t = now_ms() | 1u; }
        }
    }
}

/* ---- snapshot of enemy memory (F6/F7/F8): the entity of every enemy that asked the AI in the last 4 s, and the object it points to ---- */
static void rd_hex(FILE *f, const char *label, uint8_t *addr, size_t n)
{
    uint8_t buf[0x800];
    SIZE_T got = 0;
    size_t i;
    if (n > sizeof buf) n = sizeof buf;
    while (n >= 0x40 && !(ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got) && got == n)) { got = 0; n /= 2; }
    if (!got) { fprintf(f, "%s %p unreadable\n", label, (void *)addr); return; }
    fprintf(f, "%s %p (%u bytes)\n", label, (void *)addr, (unsigned)got);
    for (i = 0; i < got; i += 16) {
        size_t j;
        fprintf(f, "+%04X:", (unsigned)i);
        for (j = i; j < i + 16 && j < got; j++) fprintf(f, " %02X", buf[j]);
        fprintf(f, "\n");
    }
}

static void entity_snapshot(const char *key)
{
    static int serial;
    char p[MAX_PATH];
    FILE *f;
    unsigned now = now_ms();
    int i, n = 0, tgt_done = 0;
    path_in_dir(p, sizeof p, "revived_entities.txt");
    f = fopen(p, "a");
    if (!f) return;
    serial++;
    fprintf(f, "=== SNAP %d key=%s t=%u enemies=%d phase=%d f=%.2f chain=%u scale=%.2f\n", serial, key, now, enemy_count(now), g_phase, g_f, g_chain, g_cnt_k);
    rmgr_log(key);
    setup_log();
    for (i = 0; i < NSEEN && n < 16; i++) {
        void *obj = NULL;
        if (!g_seen[i].ent || now - g_seen[i].t > SEEN_MS) continue;
        fprintf(f, "--- ENT %08lX last_seen=%ums ago\n", (unsigned long)g_seen[i].ent, now - g_seen[i].t);
        fprintf(f, "dist=%.2f\n", g_seen[i].d);
        rd_hex(f, "ENT", (uint8_t *)(uintptr_t)g_seen[i].ent, 0x200);
        if (ReadProcessMemory(GetCurrentProcess(), (uint8_t *)(uintptr_t)g_seen[i].ent + 0x38, &obj, 4, NULL) && obj) {
            uint32_t sref = 0, tgt = 0, fl = 0;
            rd_hex(f, "ACT", (uint8_t *)obj - 0x4F8, 0x4F8);        /* the actor that contains the object (the distance function uses this = obj, actor = obj - 0x4F8) */
            rd_hex(f, "OBJ", (uint8_t *)obj, 0x200);
            if (ReadProcessMemory(GetCurrentProcess(), (uint8_t *)obj + 0xC, &sref, 4, NULL) && sref &&
                ReadProcessMemory(GetCurrentProcess(), (uint8_t *)(uintptr_t)sref + 8, &fl, 4, NULL) &&
                ReadProcessMemory(GetCurrentProcess(), (uint8_t *)(uintptr_t)sref, &tgt, 4, NULL) && tgt && !tgt_done) {
                fprintf(f, "TARGETREF %08lX flags=%08lX target=%08lX\n", (unsigned long)sref, (unsigned long)fl, (unsigned long)tgt);
                rd_hex(f, "TGT", (uint8_t *)(uintptr_t)tgt, 0x600);
                tgt_done = 1;
            }
        }
        slot_log((void *)(uintptr_t)g_seen[i].ent, key);
        poscand((void *)(uintptr_t)g_seen[i].ent, 0);
        n++;
    }
    fprintf(f, "=== END SNAP %d (%d entities)\n", serial, n);
    fclose(f);
    push_text("SNAPSHOT %d key=%s entities=%d", serial, key, n);
    logf_("[observer] snapshot %d (%s): %d entities -> revived_entities.txt\n", serial, key, n);
}


/* v2.44 F6/F7: compact "before / after" snapshot of the enemy nearest to the Prince.
 * F6 = baseline (hex of the three regions + every text string they point to). F7 = only the 32-bit words that changed since the last F6/F7. */
static uint8_t g_sn_e[0x200], g_sn_a[0x4F8], g_sn_o[0x400];
static uint32_t g_sn_ent; static unsigned g_sn_t; static int g_sn_have;
static int sn_pick(void)
{
    unsigned now = now_ms(); int i, best = -1; float bd = 1e9f;
    for (i = 0; i < NSEEN; i++) {
        float d; if (!g_seen[i].ent || now - g_seen[i].t > SEEN_MS) continue;
        d = g_seen[i].d < 0 ? 500.0f : g_seen[i].d;
        if (d < bd) { bd = d; best = i; }
    }
    return best >= 0 ? (int)g_seen[best].ent : 0;
}
static int sn_str(uint32_t ptr, char *out, int n)
{
    uint8_t t[40]; int i, k = 0;
    if (ptr < 0x10000u || ptr > 0x7FFE0000u) return 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)ptr, t, sizeof t, NULL)) return 0;
    for (i = 0; i < 39 && t[i] >= 32 && t[i] < 127 && k < n - 1; i++) out[k++] = (char)t[i];
    out[k] = 0; return k >= 5;
}
static int sn_read(uint32_t ent, uint32_t *obj)
{
    HANDLE me = GetCurrentProcess();
    memset(g_sn_e, 0, sizeof g_sn_e); memset(g_sn_a, 0, sizeof g_sn_a); memset(g_sn_o, 0, sizeof g_sn_o);
    if (!ReadProcessMemory(me, (void *)(uintptr_t)ent, g_sn_e, sizeof g_sn_e, NULL)) return 0;
    *obj = *(uint32_t *)(g_sn_e + 0x38);
    if (*obj < 0x100000u) return 0;
    ReadProcessMemory(me, (void *)(uintptr_t)(*obj - 0x4F8u), g_sn_a, sizeof g_sn_a, NULL);
    ReadProcessMemory(me, (void *)(uintptr_t)*obj, g_sn_o, sizeof g_sn_o, NULL);
    return 1;
}
static void sn_regions(uint8_t **r, size_t *n, const char **nm) { r[0] = g_sn_e; n[0] = sizeof g_sn_e; nm[0] = "ENT"; r[1] = g_sn_a; n[1] = sizeof g_sn_a; nm[1] = "ACT"; r[2] = g_sn_o; n[2] = sizeof g_sn_o; nm[2] = "OBJ"; }
static void sn_compact(const char *key)
{
    char p[MAX_PATH]; FILE *f; uint32_t ent = sn_pick(), obj = 0; unsigned now = now_ms();
    uint8_t *R[3]; size_t N[3]; const char *NM[3]; int k;
    uint8_t *oe = NULL, *oa = NULL, *oo = NULL;
    static uint8_t pe[0x200], pa[0x4F8], po[0x400];
    if (!ent) { push_text("SNAP %s: no enemy seen in the last 2 s", key); logf_("[observer] %s: no enemy seen\n", key); return; }
    sn_regions(R, N, NM);
    if (!sn_read(ent, &obj)) { push_text("SNAP %s: cannot read enemy %08lX", key, (unsigned long)ent); return; }
    path_in_dir(p, sizeof p, "revived_entities.txt");
    f = fopen(p, "a"); if (!f) return;
    if (key[1] == '6' || !g_sn_have || ent != g_sn_ent) {
        fprintf(f, "=== COMPACT %s t=%u ent=%08lX obj=%08lX act=%08lX dist=%.2f\n", key, now, (unsigned long)ent, (unsigned long)obj, (unsigned long)(obj - 0x4F8u), seen_get_dist(ent));
        for (k = 0; k < 3; k++) {
            size_t i, j;
            fprintf(f, "-- %s vtable=%08lX\n", NM[k], (unsigned long)*(uint32_t *)R[k]);
            for (i = 0; i < N[k]; i += 16) { fprintf(f, "%s+%04X:", NM[k], (unsigned)i); for (j = i; j < i + 16 && j < N[k]; j++) fprintf(f, " %02X", R[k][j]); fprintf(f, "\n"); }
            for (i = 0; i + 4 <= N[k]; i += 4) { char s1[48]; uint32_t w = *(uint32_t *)(R[k] + i); if (sn_str(w, s1, sizeof s1)) fprintf(f, "STR %s+%04X -> \"%s\"\n", NM[k], (unsigned)i, s1); }
            {   /* v2.46: second level - follow every pointer, look inside the target (first 0x100 bytes) for text, direct or one more pointer away */
                int n2 = 0; size_t i2;
                for (i2 = 0; i2 + 4 <= N[k] && n2 < 160; i2 += 4) {
                    uint32_t w = *(uint32_t *)(R[k] + i2); uint8_t t[0x100]; size_t o;
                    if (w < 0x10000u || w > 0x7FFE0000u || (w & 3)) continue;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)w, t, sizeof t, NULL)) continue;
                    for (o = 0; o + 4 <= sizeof t && n2 < 160; o += 4) {
                        uint32_t w2 = *(uint32_t *)(t + o); char s2[48];
                        if (sn_str(w2, s2, sizeof s2)) { fprintf(f, "STR2 %s+%04X -> [+%02X] -> \"%s\"\n", NM[k], (unsigned)i2, (unsigned)o, s2); n2++; }
                        else if (o + 8 <= sizeof t) {
                            size_t q = 0; while (o + q < sizeof t && t[o + q] >= 32 && t[o + q] < 127) q++;
                            if (q >= 6) { char s3[48]; size_t m = q < 47 ? q : 47; memcpy(s3, t + o, m); s3[m] = 0; fprintf(f, "STR2I %s+%04X -> [+%02X] inline \"%s\"\n", NM[k], (unsigned)i2, (unsigned)o, s3); n2++; o += (q & ~3u); }
                        }
                    }
                }
            }
        }
        memcpy(pe, g_sn_e, sizeof pe); memcpy(pa, g_sn_a, sizeof pa); memcpy(po, g_sn_o, sizeof po);
        g_sn_ent = ent; g_sn_have = 1; g_sn_t = now;
        push_text("SNAP %s baseline ent=%08lX", key, (unsigned long)ent);
    } else {
        int ch = 0; oe = pe; oa = pa; oo = po;
        fprintf(f, "=== COMPACT %s t=%u (+%u ms) ent=%08lX changes vs previous:\n", key, now, now - g_sn_t, (unsigned long)ent);
        for (k = 0; k < 3; k++) {
            uint8_t *old = k == 0 ? oe : k == 1 ? oa : oo; size_t i;
            for (i = 0; i + 4 <= N[k]; i += 4) {
                uint32_t a = *(uint32_t *)(old + i), b = *(uint32_t *)(R[k] + i);
                if (a != b) { float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); fprintf(f, "%s+%04X: %08lX -> %08lX   (float %g -> %g, int %ld -> %ld)\n", NM[k], (unsigned)i, (unsigned long)a, (unsigned long)b, fa, fb, (long)a, (long)b); ch++; }
            }
        }
        fprintf(f, "(%d words changed)\n", ch);
        memcpy(pe, g_sn_e, sizeof pe); memcpy(pa, g_sn_a, sizeof pa); memcpy(po, g_sn_o, sizeof po);
        g_sn_t = now;
        push_text("SNAP %s diff ent=%08lX changed=%d", key, (unsigned long)ent, ch);
    }
    fclose(f);
}


/* v2.45: watch fields of every enemy we have seen. OBJ+0x2F8 held 23 / 15 / 100 in the F6 snapshots (looks like hit points), OBJ+0x11C and OBJ+0x3C4 were state-like.
 * Every change is logged as  HPW ent=.. type#=.. off=.. old -> new  so that a hit shows up without any key press. */
typedef struct { uint32_t ent; uint32_t v[3]; int have; uint32_t first; } HpW;
static HpW g_hpw[NSEEN];
static const unsigned g_hpw_off[3] = { 0x2F8, 0x11C, 0x3C4 };
static void hp_watch(unsigned now)
{
    static unsigned next, nlog; int i;
    if (!g_hs_on || (int)(now - next) < 0) return;
    next = now + 60u;
    for (i = 0; i < NSEEN; i++) {
        uint32_t ent = g_seen[i].ent, obj = 0; int k, slot = -1, j;
        if (!ent || now - g_seen[i].t > SEEN_MS) continue;
        for (j = 0; j < NSEEN; j++) if (g_hpw[j].ent == ent) { slot = j; break; }
        if (slot < 0) { for (j = 0; j < NSEEN; j++) if (!g_hpw[j].ent) { slot = j; break; } if (slot < 0) continue; memset(&g_hpw[slot], 0, sizeof g_hpw[slot]); g_hpw[slot].ent = ent; }
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(ent + 0x38), &obj, 4, NULL) || obj < 0x100000u) continue;
        for (k = 0; k < 3; k++) {
            uint32_t v = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(obj + g_hpw_off[k]), &v, 4, NULL)) continue;
            if (!g_hpw[slot].have) g_hpw[slot].v[k] = v;
            else if (v != g_hpw[slot].v[k]) {
                if (nlog < 1500) { nlog++; push_text("HPW ent=%08lX off=%03X %u -> %u (first hp=%u) dist=%.2f", (unsigned long)ent, g_hpw_off[k], g_hpw[slot].v[k], v, g_hpw[slot].first, seen_get_dist(ent)); }
                g_hpw[slot].v[k] = v;
            }
            if (k == 0 && !g_hpw[slot].have) g_hpw[slot].first = v;
        }
        if (!g_hpw[slot].have) { g_hpw[slot].have = 1; if (nlog < 1500) { nlog++; push_text("HPW ent=%08lX new: hp=%u s11C=%u s3C4=%u", (unsigned long)ent, g_hpw[slot].v[0], g_hpw[slot].v[1], g_hpw[slot].v[2]); } }
    }
}


/* v2.47: the HPW fields changed when the ENEMY attacked, not when the Prince hit (no LMB in the log) -> they are not hit points.
 * Now: watch the whole enemy object (first 0x400 bytes). Every small whole-number field (old and new < 0x10000) that changes is logged as
 *   SIW ent=.. +off old -> new     together with "PRINCE-HIT-WINDOW" info (ms since last left click down/up) so a real hit can be found.
 * Offsets that change more than 25 times in total are silenced (they are noise). */
static uint8_t g_sia_buf[NSEEN][0x4F8]; static uint8_t g_sia_have[NSEEN]; static unsigned g_sia_cnt[0x140], g_sia_lines;
static unsigned g_open_t[NSEEN], g_hcnt[NSEEN]; static float g_open_dist[NSEEN];
static uint8_t g_siw_buf[NSEEN][0x400]; static uint32_t g_siw_ent[NSEEN]; static unsigned g_siw_cnt[0x100]; static unsigned g_siw_lines;
static void siw_watch(unsigned now)
{
    static unsigned next; int i;
    if (!g_hs_on || (int)(now - next) < 0) return;
    next = now + 50u;
    for (i = 0; i < NSEEN; i++) {
        uint32_t ent = g_seen[i].ent, obj = 0; uint8_t cur[0x400]; int j, slot = -1; size_t o;
        if (!ent || now - g_seen[i].t > SEEN_MS) continue;
        for (j = 0; j < NSEEN; j++) if (g_siw_ent[j] == ent) { slot = j; break; }
        if (slot < 0) { for (j = 0; j < NSEEN; j++) if (!g_siw_ent[j]) { slot = j; break; } if (slot < 0) continue; g_siw_ent[slot] = ent; memset(g_siw_buf[slot], 0xEE, 0x400); g_hcnt[slot] = 0; g_open_t[slot] = 0; g_sia_have[slot] = 0; }
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(ent + 0x38), &obj, 4, NULL) || obj < 0x100000u) continue;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)obj, cur, sizeof cur, NULL)) continue;
        if (g_siw_buf[slot][0] == 0xEE && g_siw_buf[slot][1] == 0xEE && g_siw_buf[slot][2] == 0xEE) { memcpy(g_siw_buf[slot], cur, sizeof cur); continue; }
        for (o = 0; o + 4 <= sizeof cur; o += 4) {
            uint32_t a = *(uint32_t *)(g_siw_buf[slot] + o), b = *(uint32_t *)(cur + o);
            if (a == b) continue;
            if (o == 0x18) {   /* v2.48 hit counter: +018 = 25 means 'open' */
                if (a == 0 && b == 25) { g_open_t[slot] = now | 1u; g_open_dist[slot] = seen_get_dist(ent); }
                else if (a == 25 && b == 0) {
                    unsigned dn = g_lmb_dn_t, ot = g_open_t[slot], as = as_last(ent);
                    if (dn && ot && (int)(now - dn) >= 0 && now - dn <= 800u && (int)(dn - ot) >= 0 && seen_get_dist(ent) <= 2.6f + 0.0f && (!as || (int)(as - dn) < 0)) {
                        g_hcnt[slot]++;
                        push_text("HIT n=%u ent=%08lX type=%s dist=%.2f reset %ums after click, open for %ums", g_hcnt[slot], (unsigned long)ent, (g_siw_buf[slot][0x2F8] == 23) ? "shield?" : (g_siw_buf[slot][0x2F8] == 15 ? "soldier?" : "unknown"), seen_get_dist(ent), now - dn, now - ot);
                    } else {
                        push_text("NOHIT ent=%08lX +018 25->0 click_ago=%ums open_before_click=%d attack_after_click=%d dist=%.2f", (unsigned long)ent, dn ? now - dn : 99999u, dn && ot && (int)(dn - ot) >= 0, (as && (int)(as - dn) >= 0), seen_get_dist(ent));
                    }
                }
            }
            if (a < 0x10000u && b < 0x10000u && g_siw_lines < 3000u && g_siw_cnt[o >> 2] < 25u) {
                unsigned dn = g_lmb_dn_t ? age_ms(now, g_lmb_dn_t) : 99999u, up = g_lmb_up_t ? age_ms(now, g_lmb_up_t) : 99999u;
                g_siw_cnt[o >> 2]++; g_siw_lines++;
                push_text("SIW ent=%08lX +%03X %u -> %u  dist=%.2f lmb_down_ago=%ums lmb_up_ago=%ums", (unsigned long)ent, (unsigned)o, a, b, seen_get_dist(ent), dn, up);
            }
        }
        memcpy(g_siw_buf[slot], cur, sizeof cur);
        {   /* v2.49: the actor in front of the object (obj - 0x4F8, 0x4F8 bytes): small whole-number changes -> SIA lines (soldiers showed nothing in the object itself) */
            uint8_t act[0x4F8]; size_t q;
            if (ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(obj - 0x4F8u), act, sizeof act, NULL)) {
                if (g_sia_have[slot]) {
                    for (q = 0; q + 4 <= sizeof act; q += 4) {
                        uint32_t a = *(uint32_t *)(g_sia_buf[slot] + q), b = *(uint32_t *)(act + q);
                        if (a != b && a < 0x10000u && b < 0x10000u && g_sia_lines < 2500u && g_sia_cnt[q >> 2] < 25u) {
                            unsigned dn = g_lmb_dn_t ? age_ms(now, g_lmb_dn_t) : 99999u;
                            g_sia_cnt[q >> 2]++; g_sia_lines++;
                            push_text("SIA ent=%08lX +%03X %u -> %u  dist=%.2f lmb_down_ago=%ums", (unsigned long)ent, (unsigned)q, a, b, seen_get_dist(ent), dn);
                        }
                    }
                }
                memcpy(g_sia_buf[slot], act, sizeof act); g_sia_have[slot] = 1;
            }
        }
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    int was4 = 0, was9 = 0, was6 = 0, was7 = 0, was8 = 0, was5 = 0, was3 = 0, was2 = 0;
    unsigned last_scan = 0;
    int hooked = 0;
    unsigned waited = 0, last_snap = 0;
    char p[MAX_PATH];
    (void)arg;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
    path_in_dir(p, sizeof p, "revived.ini");
    g_logging = GetPrivateProfileIntA("log", "enabled", LOG_DEFAULT, p);
    open_log();
    if (g_logging) { path_in_dir(p, sizeof p, "revived_trace.txt"); g_trace = fopen(p, "a"); }
    load_config(1);
    logf_("[observer] v3.11 started, pid=%lu, base=%p\n", (unsigned long)GetCurrentProcessId(), (void *)GetModuleHandleA(NULL));
    logf_("[observer] patterns=%d hold_ms=%d up=%d full=%d down=%d soldier=%.1f slots=%d/%d by_count=%d(%d..%d,%d%%..%d%%) min_enemies=%d last_pct=%d spam=%d(from %d, %d%%+%d%%) (F3 patterns, F2 rescan rings, F5 reload, F6/F7/F8 snapshots, F9 dump)\n",
          g_cfg.patterns, g_cfg.hold_ms, g_cfg.ramp_up_ms, g_cfg.full_ms, g_cfg.ramp_down_ms, g_cfg.dist[0], g_cfg.slots_inner, g_cfg.slots_outer,
          g_cfg.by_count, g_cfg.cnt_lo, g_cfg.cnt_hi, g_cfg.pct_lo, g_cfg.pct_hi, g_cfg.min_enemies, g_cfg.last_pct, g_cfg.spam_on, g_cfg.spam_from, g_cfg.spam_pct, g_cfg.spam_step);
    if (g_trace) fprintf(g_trace, "# trace start v2.29, pid=%lu\n", (unsigned long)GetCurrentProcessId());
    for (;;) {
        {   /* v2.67 AUTOTEST: F11 presses and holds the left mouse button for 8 s by itself (SendInput) and logs what the mod saw */
            static int was11, at_ph, at_seen; static unsigned at_t0;
            int d11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            unsigned tn = now_ms();
            if (d11 && !was11 && !at_t0) {
                INPUT in; UINT n;
                memset(&in, 0, sizeof in); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                n = SendInput(1, &in, sizeof in);
                at_t0 = tn | 1u; at_ph = 0; at_seen = 0;
                push_text("AUTOTEST start: SendInput returned %u (err %lu), enemies=%d", n, (unsigned long)GetLastError(), g_nenemies);
            }
            was11 = d11;
            if (at_t0) {
                if ((int)g_phase > at_ph) at_ph = (int)g_phase;
                if (g_lmb_since) at_seen = 1;
                if (age_ms(tn, at_t0) >= 8000u) {
                    INPUT in; UINT n;
                    memset(&in, 0, sizeof in); in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    n = SendInput(1, &in, sizeof in);
                    push_text("AUTOTEST end: release SendInput=%u, mod saw LMB=%d, highest phase=%d (2 = full), f=%.2f enemies=%d", n, at_seen, at_ph, g_f, g_nenemies);
                    at_t0 = 0;
                }
            }
        }
        int d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        int d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        int d7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        int d8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        int d2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        int d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        int d3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        int d4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (!g_logging) { d9 = d6 = d7 = d8 = d4 = 0; }   /* developer hotkeys (dumps, snapshots, scans) only with logging on */
        if (d4 && !was4) scan_clips(1);
        was4 = d4;
        unsigned now;
        if (!hooked) {
            if (code_ready()) { int n = install_hooks(); install_dmg_hook(); install_spawn_hook(); hooked = 1; logf_("[observer] %d hooks installed\n", n); if (g_nolt) patch_line_test(); patch_flank(); }
            else if (waited > 300000) { logf_("[observer] gave up waiting for decrypted code\n"); hooked = 1; }
        }
        if (d9 && !was9) { logf_("[observer] F9 pressed, dumping...\n"); do_dump(); logf_("[observer] dump finished\n"); }
        if (d6 && !was6) { push_text("MARK F6"); sn_compact("F6"); }
        if (d7 && !was7) { push_text("MARK F7"); sn_compact("F7"); }
        if (d8 && !was8) { push_text("MARK F8"); entity_snapshot("F8"); }
        if (d2 && !was2) { scan_rings(); }
        if (d3 && !was3) { g_cfg.patterns = !g_cfg.patterns; push_text("CONFIG patterns enabled=%d (F3)", g_cfg.patterns); logf_("[observer] F3: patterns enabled=%d\n", g_cfg.patterns); }
        if (d5 && !was5) {
            { int i2; g_ft = 0; g_fpr = 0; g_press_until = 0; g_kill_until = 0; g_retreat_until = 0; g_roll_until = 0; for (i2 = 0; i2 < g_nhits; i2++) write_hit(&g_hits[i2], 0.0f); g_f = 0; g_fc = 0; g_fs = 0; g_chain = 0; g_f_written = -1.0f; g_phase = PH_IDLE; g_wide = 0; g_relayout = 1; }
            load_config(0);
            push_text("CONFIG reload patterns=%d hold=%d soldier=%.1f", g_cfg.patterns, g_cfg.hold_ms, g_cfg.dist[0]);
            logf_("[observer] F5: reloaded revived.ini\n");
        }
        was9 = d9; was6 = d6; was7 = d7; was8 = d8; was5 = d5; was3 = d3; was2 = d2;
        poll_input();
        now = now_ms();
        { static unsigned nd; static unsigned ls, ld; if (g_rl_on && now > nd) { nd = now + 5000u; if (g_di_state_calls != ls || g_di_data_calls != ld) { ls = g_di_state_calls; ld = g_di_data_calls; push_text("DI keyboard: GetDeviceState calls=%u GetDeviceData calls=%u keys blocked=%u", ls, ld, g_di_blocked); } } }
        { static unsigned next_rf; if (hooked && g_rf_on && g_rf_n < 4 && now > 25000u && (int)(now - next_rf) >= 0) { next_rf = now + 15000u; scan_clips(0); } }
        if (hooked && !g_nhits && now - last_scan > 15000u && now > 10000u) { last_scan = now; scan_rings(); }
        surround_tick(now);
        siw_watch(now);
        (void)last_snap;
        flush_trace();
        if (g_dropped) { logf_("[observer] trace buffer overflow, dropped %ld events\n", (long)g_dropped); g_dropped = 0; }
        Sleep(10);
        waited += 10;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        char *slash;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0; else g_dir[0] = 0;
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
