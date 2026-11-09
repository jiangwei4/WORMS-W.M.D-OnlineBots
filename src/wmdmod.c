// WMD-Mod v74 — DLL proxy "dinput8.dll" pour Worms W.M.D (Steam)
// Ajoute des équipes IA (CPU) dans les parties EN LIGNE, avec les éléments du lobby local du jeu :
// bouton « Ajouter l'I.A. », sélecteur de difficulté, croix de retrait, et menu contextuel d'une IA
// (Exclure = retirer, Couper le son = niveau +, Profil = niveau -). Seul l'hôte gère les IA.
// L'exe n'est pas modifié : tout se fait en mémoire. À installer chez l'hôte ET chez chaque invité.
// Compilation : voir build.bat (-DWMD_NOLOG = version sans fichier wmdmod_log.txt).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#define REF_BASE 0x7F0000u
static uintptr_t g_base;
#define VA(x) (g_base + ((uintptr_t)(x) - REF_BASE))

// ---- adresses (base de référence 0x7F0000, build Steam 2026-08) ----
#define A_NETSETUP_PTR   0xE4C626C  // NetSetup* global
#define A_ADDCPUSLOT     0xB7C3A0   // int  NetSetup::AddCpuSlot()                 thiscall
#define A_REBUILD        0xB88800   // void NetSetup::RebuildPlayerList()          thiscall
#define A_FINDTRACK      0xB7F130   // int  NetSetup::FindOptionsTracking(id64)    thiscall
#define A_CREATETRACK    0xB7CAC0   // void NetSetup::CreateOptionsTracking(id64)  thiscall
#define A_DELETETRACK    0xB8B0A0   // void NetSetup::DeleteOptionsTracking(id64)  thiscall, ret 8
#define A_ONLINE         0xB833B0   // bool IsOnlineSessionRunning()
#define A_ISHOST         0xB832D0   // bool IsOnlineHost()
#define A_ISREADY        0xB83810   // bool IsPlayerReady(listIndex)            (ret 4)
#define A_STRFMT         0xDD8C90   // void XString::Format(XString*, fmt, ...)    cdecl
#define A_OPTSELECT      0xD70730   // void Option::Select(index) thiscall, ret 4 (marque l'option modifiée :
                                    //   NetSetup::Update 0xB851F0 l'envoie ensuite aux autres via 0xB7D5C0)
#define OPT_DIFFICULTY   1          // 2e objet d'options du joueur (id 0x...36) = difficulté IA, 5 niveaux

// Structure NetSetup (offsets)
#define NS_LIST      0xD30   // liste visible : 6 x 0x18
#define NS_LOCAL     0xDC0   // liste locale  : 6 x 0x18
#define E_ACTIVE 0x0
#define E_LOCAL  0x1
#define E_AI     0x2
#define E_B3     0x3
#define E_B4     0x4
#define E_IDX    0x8
#define E_NAME   0xC
#define E_ID     0x10

// Suivi des options par joueur : 16 x 0x20 à partir de +0xE48
#define NS_TRACK 0xE48
#define T_ACTIVE 0x08
#define T_ID     0x10
#define T_OBJ    0x18   // 4 pointeurs d'objets d'options (+0x18..+0x24)
// Objet d'options : +0 id ; 256 valeurs x 0x28 à partir de +0x10
#define O_VALS   0x10
#define V_ACTIVE 0x00
#define V_VAL    0x08
#define V_SEL    0x14
#define V_NAME   0x18

#define MAGIC_HI   0x00000000u
#define MAGIC_LO   0x57D00000u
#define IS_MAGIC(lo,hi) ((hi) == MAGIC_HI && ((lo) & 0xFFFFFF00u) == MAGIC_LO)

typedef HRESULT (WINAPI *PFN_DI8C)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DI8C g_real = NULL;
static char g_dir[MAX_PATH];
static CRITICAL_SECTION g_logcs;

static void logf_(const char* fmt, ...) {
#ifdef WMD_NOLOG
    (void)fmt; return;   // version « sans log » : aucun fichier wmdmod_log.txt
#endif
    EnterCriticalSection(&g_logcs);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%swmdmod_log.txt", g_dir);
    FILE* f = fopen(p, "a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_logcs);
}

// ---------- fichier de réglages wmdmod.ini (créé avec les valeurs par défaut s'il manque) ----------
typedef struct {
    int verbose;                     // 0 = log minimal, 1 = diagnostic (développement)
    unsigned long long hostSteamId;  // 0 = détection automatique de l'hôte
    int forcePadAI;                  // tour IA en ligne : forcer le ver à appliquer les boutons de l'IA
    int swapMapper;                  // mode hôte seul : l'hôte utilise le traducteur d'entrées de l'IA
    int aiShared;                    // 1 = l'IA tourne sur CHAQUE PC (comme en local), 0 = sur l'hôte seul
} cfg_t;
static cfg_t g_cfg = { 1, 0, 1, 1, 1 };
static void load_ini(void) {
    char path[MAX_PATH]; snprintf(path, sizeof path, "%swmdmod.ini", g_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(path, "w");
        if (f) {
            fputs("; Reglages de WMD-Mod (IA en ligne pour Worms W.M.D)\n"
                  "; Ce fichier est cree automatiquement ; supprimez-le pour revenir aux valeurs par defaut.\n\n"
                  "[Log]\n"
                  "; 0 = journal minimal, 1 = journal de diagnostic detaille (version avec log uniquement)\n"
                  "Detail=1\n\n"
                  "[Reseau]\n"
                  "; Laisser vide : l'hote est detecte automatiquement (proprietaire du lobby Steam).\n"
                  "; En secours : Steam ID 64 bits de l'hote, en decimal (ex. 76561198000000000).\n"
                  "HostSteamID=\n\n"
                  "[Experimental]\n"
                  "; 1 = pendant le tour de l'IA, le ver applique les boutons de l'IA\n"
                  "ForcerBoutonsIA=1\n"
                  "; 1 = en mode hote seul, l'hote enregistre et envoie les entrees de la manette virtuelle de l'IA\n"
                  "TraducteurIA=1\n"
                  "; 1 = l'IA tourne sur CHAQUE PC en meme temps (comme en local, rien n'est envoye) ;\n"
                  ";     0 = seul l'hote fait jouer l'IA et envoie ses actions. Les deux PC doivent avoir la meme valeur.\n"
                  "IAPartagee=1\n", f);
            fclose(f);
        }
    }
    char v[64];
    g_cfg.verbose = GetPrivateProfileIntA("Log", "Detail", 1, path);
#ifdef WMD_NOLOG
    g_cfg.verbose = 0;   // version sans log : pas de diagnostic détaillé
#endif
    GetPrivateProfileStringA("Reseau", "HostSteamID", "", v, sizeof v, path);
    g_cfg.hostSteamId = 0;
    for (const char* c = v; *c >= '0' && *c <= '9'; c++) g_cfg.hostSteamId = g_cfg.hostSteamId * 10 + (unsigned)(*c - '0');
    g_cfg.forcePadAI = GetPrivateProfileIntA("Experimental", "ForcerBoutonsIA", 1, path);
    g_cfg.swapMapper = GetPrivateProfileIntA("Experimental", "TraducteurIA", 1, path);
    g_cfg.aiShared = GetPrivateProfileIntA("Experimental", "IAPartagee", 1, path);
}

static void load_real(void) {
    if (g_real) return;
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH); strcat(sys, "\\dinput8.dll");
    HMODULE h = LoadLibraryA(sys);
    if (h) g_real = (PFN_DI8C)GetProcAddress(h, "DirectInput8Create");
}
__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE i, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    load_real(); return g_real ? g_real(i, v, r, o, u) : E_FAIL;
}

// ---------- hooks ----------
static void* make_hook(uintptr_t target, void* detour, int stolen) {
    uint8_t* tr = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, (void*)target, stolen);
    tr[stolen] = 0xE9; *(int32_t*)(tr + stolen + 1) = (int32_t)((target + stolen) - ((uintptr_t)tr + stolen + 5));
    DWORD old; VirtualProtect((void*)target, stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* t = (uint8_t*)target;
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((uintptr_t)detour - (target + 5));
    for (int k = 5; k < stolen; k++) t[k] = 0x90;
    VirtualProtect((void*)target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, stolen);
    return tr;
}
static void patch_iat_slot(uintptr_t slot, void* fn, void** orig) {
    DWORD old; VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
    *orig = *(void**)slot; *(void**)slot = fn;
    VirtualProtect((void*)slot, 4, old, &old);
}
static uintptr_t find_iat(const char* dll, const char* fn) {
    uint8_t* b = (uint8_t*)g_base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* id = (IMAGE_IMPORT_DESCRIPTOR*)(b + d.VirtualAddress); id->Name; id++) {
        if (_stricmp((char*)b + id->Name, dll)) continue;
        IMAGE_THUNK_DATA* on = (IMAGE_THUNK_DATA*)(b + id->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(b + id->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++)
            if (!(on->u1.Ordinal & IMAGE_ORDINAL_FLAG) &&
                !strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(b + on->u1.AddressOfData))->Name, fn))
                return (uintptr_t)&ft->u1.Function;
    }
    return 0;
}

// ---------- fonctions du jeu ----------
typedef int  (__fastcall *PFN_THIS0)(void*, void*);
typedef int  (__fastcall *PFN_THIS2)(void*, void*, uint32_t, uint32_t);
typedef char (__cdecl   *PFN_BOOL)(void);
typedef void (__cdecl   *PFN_FMT)(void*, const char*, ...);
static PFN_THIS0 o_rebuild, f_addcpu;
static PFN_THIS2 o_findtrack, f_createtrack, f_deletetrack;
static PFN_BOOL  f_online, f_ishost;
static PFN_FMT   f_fmt;

static uint8_t* ns(void) {
    uint8_t* n = *(uint8_t**)VA(A_NETSETUP_PTR);
    return (n && !IsBadReadPtr(n, 0x1200)) ? n : NULL;
}

// ---------- état du mod ----------
typedef struct { uint32_t lo; char name[24]; } cpu_t;
static cpu_t g_remote[6]; static int g_nremote = 0;   // IA connues côté client
static volatile LONG g_reqAddCpu = 0, g_reqRebuild = 0;   // ajout d'IA (bouton), reconstruction de liste (invité)
typedef void (__fastcall *PFN_SELECT)(void*, void*, int);
static PFN_SELECT f_select;
static DWORD g_mainTid = 0;
static int g_cpuCounter = 0;

// rand() forcé pour donner un identifiant unique à l'IA créée
typedef int (__cdecl *PFN_RAND)(void);
static PFN_RAND o_rand; static volatile int g_forceRand = -1;
static int __cdecl h_rand(void) {
    if (g_forceRand >= 0 && GetCurrentThreadId() == g_mainTid) { int v = g_forceRand; g_forceRand = -1; return v; }
    return o_rand();
}

static void dump_lists(const char* why) {
    uint8_t* n = ns();
    logf_("--- etat lobby (%s) NetSetup=%p online=%d host=%d", why, n, n ? f_online() : -1, n ? f_ishost() : -1);
    if (!n) return;
    for (int L = 0; L < 2; L++) {
        uint8_t* base = n + (L ? NS_LOCAL : NS_LIST);
        for (int i = 0; i < 6; i++) {
            uint8_t* e = base + i * 0x18;
            if (!e[E_ACTIVE]) continue;
            const char* nm = *(const char**)(e + E_NAME);
            logf_("  %s[%d] local=%d ia=%d b3=%d b4=%d idx=%d nom='%.20s' id=%08X%08X",
                  L ? "LOCALE " : "VISIBLE", i, e[E_LOCAL], e[E_AI], e[E_B3], e[E_B4], *(int*)(e + E_IDX),
                  (nm && !IsBadReadPtr(nm, 1)) ? nm : "?", *(uint32_t*)(e + E_ID + 4), *(uint32_t*)(e + E_ID));
        }
    }
}

static uint8_t* list_find(uint8_t* n, uint32_t lo, uint32_t hi) {
    for (int i = 0; i < 6; i++) {
        uint8_t* e = n + NS_LIST + i * 0x18;
        if (e[E_ACTIVE] && *(uint32_t*)(e + E_ID) == lo && *(uint32_t*)(e + E_ID + 4) == hi) return e;
    }
    return NULL;
}
static uint8_t* list_free(uint8_t* n) {
    for (int i = 0; i < 6; i++) { uint8_t* e = n + NS_LIST + i * 0x18; if (!e[E_ACTIVE]) return e; }
    return NULL;
}

// ---------- options par joueur ----------
static const char* safe_str(const char* p) { return (p && !IsBadReadPtr(p, 1)) ? p : "?"; }
static const char* player_name(uint8_t* n, uint32_t lo, uint32_t hi) {
    for (int L = 0; L < 2; L++)
        for (int i = 0; i < 6; i++) {
            uint8_t* e = n + (L ? NS_LOCAL : NS_LIST) + i * 0x18;
            if (e[E_ACTIVE] && *(uint32_t*)(e + E_ID) == lo && *(uint32_t*)(e + E_ID + 4) == hi)
                return safe_str(*(const char**)(e + E_NAME));
        }
    return "?";
}
// index de la valeur choisie (même logique que le jeu en 0xD701A0), -1 sinon
static int opt_sel(uint8_t* o, int* count) {
    int sel = -1, c = 0;
    for (int k = 0; k < 256; k++) {
        uint8_t* v = o + O_VALS + k * 0x28;
        if (!v[V_ACTIVE]) continue;
        c++;
        if (sel < 0 && v[V_SEL]) sel = k;
    }
    if (count) *count = c;
    return sel;
}
static const char* opt_name(uint8_t* o, int k) {
    return k < 0 ? "-" : safe_str(*(const char**)(o + O_VALS + k * 0x28 + V_NAME));
}
static uint8_t* track_obj(uint8_t* n, int i, int j) {
    uint8_t* t = n + NS_TRACK + i * 0x20;
    if (!t[T_ACTIVE]) return NULL;
    uint8_t* o = *(uint8_t**)(t + T_OBJ + j * 4);
    return (o && !IsBadReadPtr(o, O_VALS + 256 * 0x28)) ? o : NULL;
}
// Diagnostic : note tout changement d'option par joueur (2 fois par seconde)
static uint32_t g_wId[16]; static int g_wSel[16][4]; static int g_wInit = 0; static DWORD g_wLast = 0;
static void watch_options(void) {
    DWORD now = GetTickCount();
    if (now - g_wLast < 500) return;
    g_wLast = now;
    uint8_t* n = ns(); if (!n) { g_wInit = 0; return; }
    for (int i = 0; i < 16; i++) {
        uint8_t* t = n + NS_TRACK + i * 0x20;
        uint32_t lo = t[T_ACTIVE] ? *(uint32_t*)(t + T_ID) : 0, hi = *(uint32_t*)(t + T_ID + 4);
        for (int j = 0; j < 4; j++) {
            uint8_t* o = track_obj(n, i, j);
            int s = o ? opt_sel(o, NULL) : -2;
            if (g_wInit && o && lo && g_wId[i] == lo && g_wSel[i][j] != s)
                logf_("[options] '%.20s' (id %08X) option %08X : %d -> %d '%.24s'",
                      player_name(n, lo, hi), lo, *(uint32_t*)o, g_wSel[i][j], s, opt_name(o, s));
            g_wSel[i][j] = s;
        }
        g_wId[i] = lo;
    }
    g_wInit = 1;
}

// Prévient l'écran du lobby qu'il doit se redessiner (même chose que le jeu en 0xB8B086 / 0xB883F0) :
//   +0xB4 liste modifiée (acquittée par l'UI via +0xC8), +0xB5+i options du joueur i (acquit +0xBB+i)
#define NS_UI_LIST      0xB4
#define NS_UI_LIST_ACK  0xC8
#define NS_UI_PLAYER    0xB5
#define NS_UI_PLAYER_ACK 0xBB
// NetSetup::Update (0xB851F0) remet les indicateurs « liste/joueur modifiés » à 0 à chaque passage : on les
// maintient levés pendant 1,5 s après chaque changement (ajout, niveau, retrait, IA annoncée chez l'invité).
static volatile DWORD g_uiPulseUntil = 0;
// Une ligne existe en deux variantes (MNU_LocalPlayer_N / MNU_NetworkPlayer_N), chacune avec son
// TXT_PlayerName_N : basculer brièvement une ligne IA en variante distante fait relire son nom (voir h_islocal).
static volatile DWORD g_flipFrom = 0, g_flipUntil = 0;
static void ui_pulse(void) {
    DWORD now = GetTickCount();
    g_uiPulseUntil = now + 1500;
    g_flipFrom = now + 150; g_flipUntil = now + 450;   // laisse le temps au nom/à la fiche d'être mis à jour
}
static void ui_pulse_pump(uint8_t* n) {
    if (!n || !g_uiPulseUntil) return;
    if ((LONG)(GetTickCount() - g_uiPulseUntil) > 0) { g_uiPulseUntil = 0; return; }
    n[NS_UI_LIST] = 1; n[NS_UI_LIST_ACK] = 0;
    for (int i = 0; i < 6; i++) { n[NS_UI_PLAYER + i] = 1; n[NS_UI_PLAYER_ACK + i] = 0; }
    // moniteur d'interface « Player List Changed » (0x2AFA000B, 0x8BDA10 -> 0xB809F0) : indicateur +0x13C4,
    // acquitté par l'écran en +0x13C5, effacé par NetSetup::Update (0xB85248) ; il fait reconstruire les lignes.
    *(uint16_t*)(n + 0x13C4) = 1;
    // la liste des lignes SB_OnlinePlayers (donnée 0x2EE200AD = 0x8D9C20, nombre de joueurs) surveille « Has
    // Lobby Match Data Changed » (0x2AFA0003 -> 0x8BD860) : vrai si [0xE4C6E6C]+0x2E (consommé) ou si
    // NetSetup+0xCF (0xB82920(0)). C'est ce signal qui fait recompter/recréer les lignes (indispensable chez l'invité).
    n[0xCF] = 1; n[0xD4] = 0;   // comme le jeu (0xB84F7E) ; effacé par Update (0xB85492) une fois acquitté
    static DWORD lastG = 0;
    if (GetTickCount() - lastG > 200) {
        lastG = GetTickCount();
        uint8_t* g = *(uint8_t**)VA(0xE4C6E6C);
        if (g && !IsBadWritePtr(g + 0x2E, 1)) g[0x2E] = 1;
    }
}
static void ui_notify(uint8_t* n, uint32_t lo, uint32_t hi) {
    n[NS_UI_LIST] = 1; n[NS_UI_LIST_ACK] = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t* e = n + NS_LIST + i * 0x18;
        if (e[E_ACTIVE] && *(uint32_t*)(e + E_ID) == lo && *(uint32_t*)(e + E_ID + 4) == hi) {
            n[NS_UI_PLAYER + i] = 1; n[NS_UI_PLAYER_ACK + i] = 0;
        }
    }
}

// Comme le lobby local (0x8E8DB0) : après AddCpuSlot, l'IA reçoit une équipe CPU prédéfinie (gestionnaire
// [0xE4C6AFC] via 0xBA77B0, nombre 0xBA7850, i-ème fiche de 0xC8 octets 0xBA7990) : copie 0xBDA1E0, vidage
// des récompenses 0xB93D30(+0x90), SetLobbyOptionTeam 0xB8C150(slot, 0, &fiche), destruction 0xBDA8C0.
typedef void* (__cdecl *PFN_CPUMGR)(void);
typedef int   (__fastcall *PFN_THISINT)(void*, void*);
typedef void* (__fastcall *PFN_CPUTEAM)(void*, void*, int);
typedef void  (__fastcall *PFN_TCOPY)(void*, void*, void*);
typedef void  (__fastcall *PFN_THISVOID)(void*, void*);
typedef void  (__fastcall *PFN_SETTEAM)(void*, void*, int, int, void*);
static void assign_cpu_team(uint8_t* n, int slot, int ordinal, const char* label) {
    static const uint8_t s1[] = {0x55,0x8b,0xec,0x6a,0xff,0x68}, s2[] = {0x8b,0x41,0x2c,0x85,0xc0},
                         s3[] = {0x55,0x8b,0xec,0x56,0x8b,0x71,0x2c}, s4[] = {0x55,0x8b,0xec,0x6a,0xff,0x68};
    if (memcmp((void*)VA(0xBA77B0), s1, 6) || memcmp((void*)VA(0xBA7850), s2, 5) || memcmp((void*)VA(0xBA7990), s3, 7) ||
        memcmp((void*)VA(0xB8C150), s4, 6)) { logf_("equipe CPU : signature inattendue"); return; }
    void* mgr = ((PFN_CPUMGR)VA(0xBA77B0))();
    if (!mgr) { logf_("equipe CPU : pas de gestionnaire"); return; }
    int count = ((PFN_THISINT)VA(0xBA7850))(mgr, 0);
    if (count <= 0) { logf_("equipe CPU : liste vide"); return; }
    int idx = ordinal % count;
    void* rec = ((PFN_CPUTEAM)VA(0xBA7990))(mgr, 0, idx);
    if (!rec) { logf_("equipe CPU : fiche %d introuvable", idx); return; }
    static uint8_t buf[0x200];
    memset(buf, 0, sizeof buf);
    ((PFN_TCOPY)VA(0xBDA1E0))(buf, 0, rec);
    ((PFN_THISVOID)VA(0xB93D30))(buf + 0x90, 0);
    if (label) {   // nom de l'équipe = « [IA1] Niv 3 » (+0 nom, +8 nom affiché s'il existe)
        f_fmt(buf, "%s", label);
        const char* alt; memcpy(&alt, buf + 8, sizeof alt);
        if (alt && !IsBadReadPtr(alt, 1) && alt[0]) f_fmt(buf + 8, "%s", label);
    }
    const char* title; memcpy(&title, buf, sizeof title);   // XString : nom de l'équipe à +0
    ((PFN_SETTEAM)VA(0xB8C150))(n, 0, slot, 0, buf);
    logf_("equipe CPU %d/%d '%s' donnee a l'IA du slot %d", idx, count, (title && !IsBadReadPtr(title, 1)) ? title : "?", slot);
    ((PFN_THISVOID)VA(0xBDA8C0))(buf, 0);
}

// Hôte : retire une IA du mod (target = son id, 0 = la dernière ajoutée)
static void remove_cpu(uint8_t* n, uint32_t target) {
    uint8_t* best = NULL;
    for (int i = 0; i < 6; i++) {
        uint8_t* s = n + NS_LOCAL + i * 0x18;
        uint32_t lo = *(uint32_t*)(s + E_ID), hi = *(uint32_t*)(s + E_ID + 4);
        if (!s[E_ACTIVE] || !s[E_AI] || !IS_MAGIC(lo, hi)) continue;
        if (target ? lo == target : (!best || lo > *(uint32_t*)(best + E_ID))) best = s;
    }
    if (!best) { logf_("[retrait IA] aucune IA du mod a retirer"); return; }
    uint32_t lo = *(uint32_t*)(best + E_ID);
    logf_("[retrait IA] '%.20s' (id %08X)", safe_str(*(const char**)(best + E_NAME)), lo);
    best[E_ACTIVE] = 0;                       // nom laissé en place : le jeu le libère à la réutilisation
    uint8_t* v = list_find(n, lo, MAGIC_HI);
    if (v) v[E_ACTIVE] = 0;
    f_deletetrack(n, 0, lo, MAGIC_HI);
    o_rebuild(n, 0);
    ui_notify(n, 0, 0);
    for (int i = 0; i < 6; i++) { n[NS_UI_PLAYER + i] = 1; n[NS_UI_PLAYER_ACK + i] = 0; }  // les lignes ont bougé
    dump_lists("retrait IA");
}

// ---------- difficulté ----------
static uint8_t* cpu_diff_obj(uint8_t* n, uint32_t lo) {
    for (int i = 0; i < 16; i++) {
        uint8_t* t = n + NS_TRACK + i * 0x20;
        if (t[T_ACTIVE] && *(uint32_t*)(t + T_ID) == lo && *(uint32_t*)(t + T_ID + 4) == MAGIC_HI)
            return track_obj(n, i, OPT_DIFFICULTY);
    }
    return NULL;
}
static int cpu_level(uint8_t* n, uint32_t lo) {   // 1..5, 0 si inconnu
    uint8_t* o = cpu_diff_obj(n, lo);
    return o ? opt_sel(o, NULL) + 1 : 0;
}

static void cpu_label(char* out, size_t sz, uint32_t lo, int lvl);   // défini plus bas
// Hôte : niveau d'une IA +1 / -1, borné à 1..5 (Couper le son = Niv +, Profil = Niv -)
static void step_cpu_level(uint8_t* n, uint32_t lo, int delta) {
    uint8_t* o = cpu_diff_obj(n, lo);
    if (!o) { logf_("[niveau IA] options de l'IA %08X introuvables", lo); return; }
    int c, s = opt_sel(o, &c);
    if (c <= 0) return;
    int ns_ = s + delta;
    if (ns_ < 0) ns_ = 0;
    if (ns_ > c - 1) ns_ = c - 1;
    if (ns_ == s) return;
    f_select(o, 0, ns_);
    ui_notify(n, lo, MAGIC_HI);
    logf_("[niveau IA] IA %08X difficulte %d -> %d", lo, s + 1, ns_ + 1);
}

// Nom des IA : « [IA1] Niv 3 » (numéro = rang parmi les IA actives, 1..6)
static int cpu_rank(uint8_t* n, uint32_t lo) {
    uint32_t ids[16]; int c = 0;
    for (int L = 0; L < 2 && n; L++)
        for (int i = 0; i < 6; i++) {
            uint8_t* e = n + (L ? NS_LOCAL : NS_LIST) + i * 0x18;
            uint32_t l2 = *(uint32_t*)(e + E_ID), h2 = *(uint32_t*)(e + E_ID + 4);
            if (!e[E_ACTIVE] || !e[E_AI] || !IS_MAGIC(l2, h2)) continue;
            int dup = 0; for (int k = 0; k < c; k++) if (ids[k] == l2) dup = 1;
            if (!dup && c < 16) ids[c++] = l2;
        }
    for (int k = 0; k < g_nremote; k++) {
        int dup = 0; for (int j = 0; j < c; j++) if (ids[j] == g_remote[k].lo) dup = 1;
        if (!dup && c < 16) ids[c++] = g_remote[k].lo;
    }
    int r = 1; for (int k = 0; k < c; k++) if (ids[k] < lo) r++;
    return r;
}
static uint8_t* g_labelNs = NULL;
static void cpu_label(char* out, size_t sz, uint32_t lo, int lvl) {      // nom d'équipe (se rafraîchit)
    snprintf(out, sz, "[IA%d] Niv %d", cpu_rank(g_labelNs, lo), lvl);
}
static int cpu_level(uint8_t* n, uint32_t lo);
static void cpu_rowname(char* out, size_t sz, uint32_t lo) {             // « [IA1] Niv 3 » (lu au 1er affichage)
    int lv = g_labelNs ? cpu_level(g_labelNs, lo) : 0;
    snprintf(out, sz, "[IA%d] Niv %d", cpu_rank(g_labelNs, lo), lv ? lv : 3);
}
// SetLobbyOptionTeam (0xB8C150) compte dans la LISTE VISIBLE (+0xD30), pas dans la liste locale renvoyée par
// AddCpuSlot : on attend que l'IA soit dans la liste visible, puis on lui donne sa fiche CPU.
static int vis_index(uint8_t* n, uint32_t lo) {
    for (int i = 0; i < 6; i++) {
        uint8_t* e = n + NS_LIST + i * 0x18;
        if (e[E_ACTIVE] && *(uint32_t*)(e + E_ID) == lo && *(uint32_t*)(e + E_ID + 4) == MAGIC_HI) return i;
    }
    return -1;
}
static uint32_t g_pendTeam[8];   // ids d'IA en attente de leur fiche
static void apply_cpu_team(uint8_t* n, uint32_t lo, const char* why) {
    int vi = vis_index(n, lo);
    if (vi < 0) return;
    logf_("IA %08X : fiche CPU sur la ligne visible %d (%s)", lo, vi, why);
    assign_cpu_team(n, vi, (int)(lo & 0xFF) - 1, NULL);   // vrai nom d'équipe ; le nom en partie vient de h_settitle
}
static void pending_cpu_teams(uint8_t* n) {
    for (int k = 0; k < 8; k++) {
        if (!g_pendTeam[k]) continue;
        if (vis_index(n, g_pendTeam[k]) < 0) continue;
        uint32_t lo = g_pendTeam[k]; g_pendTeam[k] = 0;
        apply_cpu_team(n, lo, "nouvelle IA");
    }
}
// Met le niveau dans le nom des IA, chez l'hôte comme chez les clients
static uint32_t g_nmId[8]; static int g_nmLvl[8];
static void refresh_cpu_names(uint8_t* n) {
    g_labelNs = n;
    for (int L = 0; L < 2; L++)
        for (int i = 0; i < 6; i++) {
            uint8_t* e = n + (L ? NS_LOCAL : NS_LIST) + i * 0x18;
            uint32_t lo = *(uint32_t*)(e + E_ID), hi = *(uint32_t*)(e + E_ID + 4);
            if (!e[E_ACTIVE] || !e[E_AI] || !IS_MAGIC(lo, hi)) continue;
            int lvl = cpu_level(n, lo); if (!lvl) continue;
            int k = (lo & 0xFF) % 8;
            char want[32]; cpu_rowname(want, sizeof want, lo);
            const char* cur = *(const char**)(e + E_NAME);
            if (cur && !IsBadReadPtr(cur, 1) && !strcmp(cur, want)) continue;
            f_fmt(e + E_NAME, "%s", want);
            ui_notify(n, lo, hi);
            ui_pulse();   // nom changé (niveau chez l'hôte, options reçues chez l'invité) -> écran prévenu
            if (g_nmId[k] != lo || g_nmLvl[k] != lvl) {
                g_nmId[k] = lo; g_nmLvl[k] = lvl;
                logf_("IA %08X : nom -> '%s'", lo, want);
            }
        }
    for (int k = 0; k < g_nremote; k++) {   // nom utilisé par inject_cpus côté client
        int lvl = cpu_level(n, g_remote[k].lo);
        if (lvl) cpu_label(g_remote[k].name, sizeof g_remote[0].name, g_remote[k].lo, lvl);
    }
}

// Après la reconstruction en ligne de la liste : on réinjecte les IA
static void inject_cpus(uint8_t* n) {
    if (!f_online()) return;   // hors ligne, le jeu le fait déjà
    // hôte : IA de la liste locale
    for (int i = 0; i < 6; i++) {
        uint8_t* s = n + NS_LOCAL + i * 0x18;
        uint32_t lo = *(uint32_t*)(s + E_ID), hi = *(uint32_t*)(s + E_ID + 4);
        if (!s[E_ACTIVE] || !s[E_AI] || !IS_MAGIC(lo, hi) || list_find(n, lo, hi)) continue;
        uint8_t* e = list_free(n); if (!e) break;
        e[E_ACTIVE] = 1; e[E_LOCAL] = 1; e[E_AI] = 1; e[E_B3] = 1; e[E_B4] = 0;
        *(int*)(e + E_IDX) = 0;
        char* src = *(char**)(s + E_NAME);
        if (src) { (*(int*)(src - 0xC))++; *(char**)(e + E_NAME) = src; }   // copie refcomptée
        *(uint32_t*)(e + E_ID) = lo; *(uint32_t*)(e + E_ID + 4) = hi;
    }
    // client : IA annoncées par l'hôte
    for (int k = 0; k < g_nremote; k++) {
        if (list_find(n, g_remote[k].lo, MAGIC_HI)) continue;
        uint8_t* e = list_free(n); if (!e) break;
        e[E_ACTIVE] = 1; e[E_LOCAL] = 0; e[E_AI] = 1; e[E_B3] = 1; e[E_B4] = 0;
        *(int*)(e + E_IDX) = 0;
        f_fmt(e + E_NAME, "%s", g_remote[k].name);
        *(uint32_t*)(e + E_ID) = g_remote[k].lo; *(uint32_t*)(e + E_ID + 4) = MAGIC_HI;
    }
}

// Les IA sont toujours "prêtes"
typedef char (__fastcall *PFN_READY)(void*, void*, uint32_t);
static PFN_READY o_ready;
static char __fastcall h_ready(void* ecx, void* edx, uint32_t i) {
    uint8_t* n = ns();
    if (n && i < 6) {
        uint32_t k = 0;
        for (int j = 0; j < 6; j++) {
            uint8_t* e = n + NS_LIST + j * 0x18;
            if (!e[E_ACTIVE]) continue;
            if (k == i) {
                if (e[E_AI] && IS_MAGIC(*(uint32_t*)(e + E_ID), *(uint32_t*)(e + E_ID + 4))) return 1;
                break;
            }
            k++;
        }
    }
    return o_ready(ecx, edx, i);
}

static int __fastcall h_rebuild(void* self, void* edx) {
    g_mainTid = g_mainTid; // (fixé juste après)
    int r = o_rebuild(self, edx);
    DWORD tid = GetCurrentThreadId();
    if (tid != g_mainTid) { g_mainTid = tid; logf_("thread logique du jeu = %lu", tid); }
    inject_cpus((uint8_t*)self);
    return r;
}

// Client : quand l'hôte envoie les options d'une IA inconnue, on crée son suivi et on l'ajoute
static int g_inCreate = 0;
static int __fastcall h_findtrack(void* self, void* edx, uint32_t lo, uint32_t hi) {
    int r = o_findtrack(self, edx, lo, hi);
    if (r == -1 && !g_inCreate && IS_MAGIC(lo, hi) && GetCurrentThreadId() == g_mainTid && f_online() && !f_ishost()) {
        int known = 0;
        for (int k = 0; k < g_nremote; k++) if (g_remote[k].lo == lo) known = 1;
        if (!known && g_nremote < 6) {
            g_remote[g_nremote].lo = lo;
            snprintf(g_remote[g_nremote].name, sizeof g_remote[0].name, "CPU%d", (int)(lo & 0xFF) + 1);
            g_nremote++;
            logf_("Client : IA %08X annoncee par l'hote -> ajout", lo);
        }
        g_inCreate = 1; f_createtrack(self, edx, lo, hi); g_inCreate = 0;
        r = o_findtrack(self, edx, lo, hi);
        InterlockedExchange(&g_reqRebuild, 1);
    }
    return r;
}

// ---------- messages de debug de l'IA ----------
// 0xCCC750 est le printf de debug de l'IA (45 appels : "My CPU character is level #%d...",
// "AI turn ended"...), vidé en version commerciale (un simple RET suivi de 15 int3).
// On le redirige vers notre log. Formateur maison : un %s invalide ne fait pas planter.
#define A_AILOG 0xCCC750
static volatile LONG g_aiLines = 0;
static void __cdecl h_ailog(const char* fmt, ...) {
    if (!fmt || IsBadReadPtr(fmt, 1) || InterlockedIncrement(&g_aiLines) > 3000) return;
    char out[512]; int o = 0;
    va_list a; va_start(a, fmt);
    for (const char* p = fmt; *p && o < (int)sizeof out - 64; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[16]; int k = 0; spec[k++] = '%'; p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && k < 12) spec[k++] = *p++;
        if (!*p) break;
        char c = *p; spec[k++] = c; spec[k] = 0;
        int room = (int)sizeof out - o - 1;
        if (c == '%') out[o++] = '%';
        else if (c == 's') { const char* s = va_arg(a, const char*); o += snprintf(out + o, room, "%.80s", (s && !IsBadReadPtr(s, 1)) ? s : "(?)"); }
        else if (c == 'f' || c == 'g' || c == 'e') o += snprintf(out + o, room, spec, va_arg(a, double));
        else if (strchr("diuxXcp", c)) o += snprintf(out + o, room, spec, va_arg(a, int));
        else { out[o++] = '?'; }
        if (o > (int)sizeof out - 64) o = (int)sizeof out - 64;
    }
    va_end(a);
    while (o > 0 && (out[o-1] == '\n' || out[o-1] == '\r')) o--;
    out[o] = 0;
    logf_("[IA jeu] %s", out);
}
static void patch_ailog(void) {
    uint8_t* t = (uint8_t*)VA(A_AILOG);
    if (t[0] != 0xC3 || t[1] != 0xCC || t[4] != 0xCC) { logf_("log IA : signature inattendue, pas de redirection"); return; }
    DWORD old; VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((uintptr_t)h_ailog - ((uintptr_t)t + 5));
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
}

// ---------- espion (diagnostic du blocage au tour de l'IA) ----------
// Note chaque combinaison (fonction, appelant, argument, résultat) une seule fois, avec l'heure :
// montre quelles parties du jeu interrogent la liste des joueurs pendant la partie.
#define A_GETCOUNT    0xB80100   // int  GetPlayerCount()           thiscall
#define A_GETID       0xB7FFF0   // u64  GetPlayerId(idx)           thiscall, ret 4
#define A_GETLOCALID  0xB80800   // u64  GetLocalPlayerId()         thiscall
#define A_ISLOCAL     0xB83440   // bool IsLocal(idx)               thiscall, ret 4
#define A_ISAI        0xB82F00   // bool IsAI(idx)                  thiscall, ret 4
#define A_FINDIDX     0xB7EFD0   // int  FindPlayerIndex(id64)      thiscall, ret 8
#define A_ISTEAMCPU   0xACF290   // bool IsTeamCPU(team)            cdecl (profil IA de l'équipe non vide)
typedef struct { uint8_t fn; uint32_t ret, arg, res; } spy_t;
static spy_t g_spy[1500]; static int g_nspy = 0;
static CRITICAL_SECTION g_spycs;
static void spy(int fn, void* ret, uint32_t arg, uint32_t res) {
    if (!g_cfg.verbose) return;
    static const char* names[] = { "GetPlayerCount", "GetPlayerId", "GetLocalPlayerId", "IsLocal", "IsAI", "FindPlayerIndex", "IsTeamCPU", "TeamPlayer" };
    uint32_t r = (uint32_t)((uintptr_t)ret - g_base + REF_BASE);
    // table de hachage (le jeu appelle certaines de ces fonctions des centaines de fois par image)
    static int16_t slot[4096];   // 0 = vide, sinon index+1 dans g_spy
    uint32_t h = (r * 2654435761u) ^ (arg * 40503u) ^ (res * 2246822519u) ^ (uint32_t)fn;
    EnterCriticalSection(&g_spycs);
    for (uint32_t k = 0; k < 4096; k++) {
        int16_t* sl = &slot[(h + k) & 4095];
        if (*sl) {
            spy_t* e = &g_spy[*sl - 1];
            if (e->fn == fn && e->ret == r && e->arg == arg && e->res == res) break;   // déjà vu
            continue;
        }
        if (g_nspy < (int)(sizeof g_spy / sizeof g_spy[0])) {
            g_spy[g_nspy].fn = fn; g_spy[g_nspy].ret = r; g_spy[g_nspy].arg = arg; g_spy[g_nspy].res = res;
            *sl = (int16_t)(++g_nspy);
            logf_("[espion] %s(%08X) <- %08X = %08X (thread %lu)", names[fn], arg, r, res, GetCurrentThreadId());
        }
        break;
    }
    LeaveCriticalSection(&g_spycs);
}
typedef int      (__fastcall *PFN_I0)(void*, void*);
typedef uint64_t (__fastcall *PFN_Q0)(void*, void*);
typedef uint64_t (__fastcall *PFN_Q1)(void*, void*, uint32_t);
typedef char     (__fastcall *PFN_B1)(void*, void*, uint32_t);
typedef int      (__fastcall *PFN_I2)(void*, void*, uint32_t, uint32_t);
static PFN_I0 o_getcount; static PFN_Q1 o_getid; static PFN_Q0 o_getlocalid;
static PFN_B1 o_islocal, o_isai; static PFN_I2 o_findidx;
typedef char (__cdecl *PFN_TEAMCPU)(uint32_t);
static PFN_TEAMCPU o_isteamcpu;
extern volatile LONG g_inGameOnline, g_isHostCached;
#define RET_AISTART 0xC5F46B   // 0xC5F2D0 (état 7) : si IsTeamCPU → démarre l'IA de CE PC
#define RET_TURNSTART 0xC60833 // 0xC607F0 (début de tour) : IsTeamCPU de l'équipe qui va jouer
volatile LONG g_turnIsCPU = 0;
volatile LONG g_aiTurnLog = 0;
static char __cdecl h_isteamcpu(uint32_t team) {
    char r = o_isteamcpu(team);
    void* ra = __builtin_return_address(0);
    uintptr_t rr = (uintptr_t)ra - g_base + REF_BASE;
    spy(6, ra, team, (uint8_t)r);
    if ((uintptr_t)ra == VA(RET_TURNSTART)) { g_turnIsCPU = r ? 1 : 0; g_aiTurnLog = r ? 400 : 0; }
    if (g_cfg.verbose && r && g_aiTurnLog > 0 && GetCurrentThreadId() == g_mainTid &&
        rr != 0xB4CB53 && rr != 0xB06ADF && rr != 0xB5E08F && rr != 0xB5B0CE && rr != 0xC9AC8F) {   // pendant un tour IA (hors affichage)
        InterlockedDecrement(&g_aiTurnLog);
        logf_("[tour] IsTeamCPU(%u) <- %08X", team, (unsigned)rr);
    }
    // mode hôte seul (IAPartagee=0) : chez l'invité, l'IA n'est pas lancée, l'hôte la joue
    if (!g_cfg.aiShared && r && g_inGameOnline && !g_isHostCached && (uintptr_t)ra == VA(RET_AISTART)) {
        logf_("[tour] client : IA de l'equipe %u NON lancee ici (l'hote la joue)", team);
        return 0;
    }
    return r;
}
static int __fastcall h_getcount(void* c, void* d) {
    int r = o_getcount(c, d); spy(0, __builtin_return_address(0), 0, r); return r;
}
static uint64_t __fastcall h_getid(void* c, void* d, uint32_t i) {
    uint64_t r = o_getid(c, d, i); spy(1, __builtin_return_address(0), i, (uint32_t)r); return r;
}
static uint64_t __fastcall h_getlocalid(void* c, void* d) {
    uint64_t r = o_getlocalid(c, d); spy(2, __builtin_return_address(0), 0, (uint32_t)r); return r;
}
static char __fastcall h_islocal(void* c, void* d, uint32_t i) {
    char r = o_islocal(c, d, i); spy(3, __builtin_return_address(0), i, (uint8_t)r);
    // Activateur « Lobby Mute Button » (0x8BFE70 -> IsLocal en 0x8BFE98) : une ligne IA compte comme
    // distante pour que « Couper le son » (= niveau +) apparaisse dans son menu.
    if (r && i < 6 && (uintptr_t)__builtin_return_address(0) == VA(0x8BFE9D)) {
        uint8_t* e = (uint8_t*)c + NS_LIST + i * 0x18;
        if (e[E_AI] && IS_MAGIC(*(uint32_t*)(e + E_ID), *(uint32_t*)(e + E_ID + 4))) return 0;
    }
    // Activateur « I am the local player » (0x8BE7C0, appel 0xB83440 en 0x8BE822) : pendant 0,3 s après un
    // changement (ui_pulse), une ligne IA passe en variante « joueur distant » puis revient, ce qui fait
    // relire son nom (TXT_PlayerName_N n'a pas de moniteur).
    DWORD now_ = GetTickCount();
    int flip = g_flipUntil && (LONG)(now_ - g_flipFrom) >= 0 && (LONG)(now_ - g_flipUntil) < 0;
    if (flip && r && i < 6 && (uintptr_t)__builtin_return_address(0) == VA(0x8BE827)) {
        uint8_t* e = (uint8_t*)c + NS_LIST + i * 0x18;
        if (e[E_AI] && IS_MAGIC(*(uint32_t*)(e + E_ID), *(uint32_t*)(e + E_ID + 4))) return 0;
    }
    return r;
}
static char __fastcall h_isai(void* c, void* d, uint32_t i) {
    char r = o_isai(c, d, i); spy(4, __builtin_return_address(0), i, (uint8_t)r); return r;
}
static int __fastcall h_findidx(void* c, void* d, uint32_t lo, uint32_t hi) {
    int r = o_findidx(c, d, lo, hi); spy(5, __builtin_return_address(0), lo, r); return r;
}
// Au lancement (hôte), le constructeur des équipes de la partie (0xBF8F90 / 0xBF7500) écrit le titre de
// l'équipe du joueur i avec 0xAE2D10(i, drapeau, XString* nom) (cdecl). Pour un joueur IA du mod, on réécrit
// ce titre en « [IAk] Niv n », quelle que soit l'équipe choisie dans le lobby (apparence et vers conservés).
// Les équipes sont ensuite envoyées telles quelles aux invités.
typedef void (__cdecl *PFN_SETTITLE)(uint32_t, uint32_t, void*);
static PFN_SETTITLE o_settitle;
static void __cdecl h_settitle(uint32_t team, uint32_t flag, void* name) {
    o_settitle(team, flag, name);
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    if (ra != VA(0xBF9171) && ra != VA(0xBF76F3)) return;
    uint8_t* n = ns();
    if (!n || !o_getid || team >= 6) return;
    uint64_t id = o_getid(n, 0, team);
    uint32_t lo = (uint32_t)id, hi = (uint32_t)(id >> 32);
    {   // diagnostic : qui reçoit quelle fiche au lancement
        const char* nm = name ? *(const char**)name : NULL;
        if (team == 0) dump_lists("construction des equipes");
        logf_("[equipes] lancement : equipe %u <- joueur id %08X%08X '%.24s', fiche '%.24s'", team, hi, lo,
              player_name(n, lo, hi), (nm && !IsBadReadPtr(nm, 1)) ? nm : "?");
    }
    if (!IS_MAGIC(lo, hi)) return;
    g_labelNs = n;
    int lv = cpu_level(n, lo); if (!lv) lv = 3;
    char lb[32]; cpu_label(lb, sizeof lb, lo, lv);
    f_fmt((void*)(VA(0x58BA4E8) + team * 0xD98), "%s", lb);
    logf_("[equipes] joueur %u (IA %08X) : titre d'equipe -> '%s'", team, lo, lb);
}
// Menu contextuel d'un joueur du lobby en ligne (BTN_Player_N), détourné pour les lignes IA :
//  « Expulser » (champ Kick 0x2EE2020E -> 0x8E2CD0 -> NetSetup::Kick 0xB83990(ligne)) = retirer CETTE IA ;
//    le bouton n'apparaît que si hôte && 0xB83200(ligne) == 0 (activateur 0x8BFE00) -> forcé à 0 pour une IA.
//  « Couper le son » (champ Mute 0x2EE2020F -> 0x8E2D20 -> 0xB8C7E0(ligne, état)) = niveau +.
//  « Profil » (champ 0x2EE20232 -> 0x8D2430) = niveau -.
// Les actions sont mises en file et exécutées par pump() dans le thread logique.
static volatile LONG g_reqKickLo = 0, g_reqUpLo = 0, g_reqDownLo = 0;
static uint32_t ai_at_row(void* ns_, uint32_t i) {
    if (i >= 6) return 0;
    uint8_t* e = (uint8_t*)ns_ + NS_LIST + i * 0x18;
    uint32_t lo = *(uint32_t*)(e + E_ID), hi = *(uint32_t*)(e + E_ID + 4);
    return (e[E_ACTIVE] && e[E_AI] && IS_MAGIC(lo, hi)) ? lo : 0;
}
typedef void (__fastcall *PFN_KICK)(void*, void*, uint32_t);
typedef char (__fastcall *PFN_ISSELF)(void*, void*, uint32_t);
typedef void (__fastcall *PFN_SETMUTE)(void*, void*, uint32_t, uint32_t);
static PFN_KICK o_kick; static PFN_ISSELF o_isself; static PFN_SETMUTE o_setmute;
static void __fastcall h_kick(void* c, void* d, uint32_t i) {
    uint32_t lo = ai_at_row(c, i);
    if (lo) { logf_("[menu] Expulser sur l'IA %08X (ligne %u) -> retrait", lo, i); InterlockedExchange(&g_reqKickLo, (LONG)lo); return; }
    o_kick(c, d, i);
}
static char __fastcall h_isself(void* c, void* d, uint32_t i) {
    if (ai_at_row(c, i)) return 0;   // bouton Expulser visible sur une ligne IA
    return o_isself(c, d, i);
}
static void __fastcall h_setmute(void* c, void* d, uint32_t i, uint32_t st) {
    uint32_t lo = ai_at_row(c, i);
    if (lo) {
        if (!f_ishost()) return;   // seul l'hôte change le niveau
        logf_("[menu] Muet sur l'IA %08X (ligne %u) -> niveau +", lo, i);
        InterlockedExchange(&g_reqUpLo, (LONG)lo); return;
    }
    o_setmute(c, d, i, st);
}
// « Profil » (champ Open Platform Profile 0x2EE20232 -> 0x8D2430, cdecl(widget, ?), ligne = mot
// widget+0x81C - 1, clic = widget+0x2888 & 1) sur une IA = niveau -.
typedef int (__cdecl *PFN_FIELDCB)(void*, void*);
static PFN_FIELDCB o_profile;
static int __cdecl h_profile(void* w, void* x) {
    uint8_t* n = ns();
    if (w && n && !IsBadReadPtr(w, 0x2890)) {
        uint32_t row = *(uint16_t*)((uint8_t*)w + 0x81C);
        uint32_t lo = row ? ai_at_row(n, row - 1) : 0;
        if (lo) {
            if (*((uint8_t*)w + 0x2888) & 1) {
                if (f_ishost()) { logf_("[menu] Profil sur l'IA %08X (ligne %u) -> niveau -", lo, row - 1); InterlockedExchange(&g_reqDownLo, (LONG)lo); }
            }
            return 1;
        }
    }
    return o_profile(w, x);
}

// ---------- bandeau IA du lobby local dans le lobby en ligne ----------
// Les écrans sont des JSON (wads/uidata_0.wad) analysés au chargement : 0x830FA0 lit le fichier, copie le
// texte puis appelle le lecteur JSON 0x7F6CC0(lecteur, texte) (thiscall ret 4, retour 0x8310CD). Pour
// WMD_OnlineLobby.bkm on insère au début de "list" les éléments du lobby local (sélecteur de niveau MNU_AI_0N,
// croix BTN_RemovePlayer, bouton MNU_OfflineAddAI_0N), générés hors ligne par mkbanner.py -> banner_frag.h,
// et on relève loopMax. Les ids injectés ont un mot bas unique < 0x270F : la scène indexe ses éléments par
// ce mot bas dans une table de 9999 entrées (0x84EB20) ; hors table, ni clic ni DataParent.
#include "banner_frag.h"
typedef int (__fastcall *PFN_JSONPARSE)(void*, void*, const char*);
static PFN_JSONPARSE o_jsonparse;
static int __fastcall h_jsonparse(void* rd, void* edx, const char* txt) {
    if ((uintptr_t)__builtin_return_address(0) != VA(0x8310CD) || !txt) return o_jsonparse(rd, edx, txt);
    if (!strstr(txt, "WMD_OnlineLobby.bkm")) return o_jsonparse(rd, edx, txt);
    const char* lst = strstr(txt, "\"list\"");
    const char* br = lst ? strchr(lst, '{') : NULL;
    const char* lm = strstr(txt, "\"loopMax\"");
    if (!br || !lm) { logf_("[bandeau] structure inattendue, ecran non modifie"); return o_jsonparse(rd, edx, txt); }
    size_t n = strlen(txt), f = sizeof BANNER_FRAG - 1;
    char* out = (char*)malloc(n + f + 64);
    if (!out) return o_jsonparse(rd, edx, txt);
    size_t a = (size_t)(br + 1 - txt);
    memcpy(out, txt, a); size_t o = a;
    memcpy(out + o, BANNER_FRAG, f); o += f; out[o++] = ',';
    memcpy(out + o, txt + a, n - a); o += n - a; out[o] = 0;
    // loopMax : "loopMax" : NNNN -> BANNER_LOOPMAX (même longueur ou plus courte, on complète par des espaces)
    // le loopMax de l'écran est le DERNIER "loopMax" du texte (les éléments en ont aussi, valeur 1)
    char* lm2 = NULL;
    for (char* q = out + o - 9; q > out; q--) if (*q == '"' && !memcmp(q, "\"loopMax\"", 9)) { lm2 = q; break; }
    if (lm2) {
        char* c = strchr(lm2 + 9, ':');
        if (c) {
            c++; while (*c == ' ') c++;
            char* e = c; while (*e >= '0' && *e <= '9') e++;
            int old = atoi(c); char num[16]; int len = snprintf(num, sizeof num, "%d", BANNER_LOOPMAX);
            if (BANNER_LOOPMAX > old && len <= e - c) { memset(c, ' ', e - c); memcpy(c, num, len); }
            else if (BANNER_LOOPMAX > old) logf_("[bandeau] loopMax %d trop court pour %d", old, BANNER_LOOPMAX);
        }
    }
    logf_("[bandeau] WMD_OnlineLobby : %u octets de bandeau IA inseres (loopMax %d)", (unsigned)f, BANNER_LOOPMAX);
    int r = o_jsonparse(rd, edx, out);
    free(out);
    return r;
}
// Champs du bandeau local : verrouillés en ligne (IsOnlineSessionRunning) ; on les relaie pour une ligne IA.
//  0x8EB380 = croix BTN_RemovePlayer (champ 0x2EE2018D) ; 0x8EB550 = liste des niveaux (0x2EE201AF) ;
//  0x8EB680 = niveau courant / choix (0x2EE201B0). Contexte du widget : mot +0x81C = ligne+1, +0x81E = type.
static PFN_FIELDCB o_fremove, o_flevels, o_flevel;
static volatile LONG g_fieldLog = 0;
static void field_log(const char* what, void* w) {
    if (!w || IsBadReadPtr(w, 0x2890) || InterlockedIncrement(&g_fieldLog) > 300) return;
    logf_("[bandeau] %s : ligne+1=%u type=%04X clic=%d", what, *(uint16_t*)((uint8_t*)w + 0x81C),
          *(uint16_t*)((uint8_t*)w + 0x81E), *((uint8_t*)w + 0x2888) & 1);
}
static int __cdecl h_fremove(void* w, void* x) {
    uint8_t* n = ns();
    if (w && n && f_online() && !IsBadReadPtr(w, 0x2890)) {
        uint32_t row = *(uint16_t*)((uint8_t*)w + 0x81C);
        uint32_t lo = row ? ai_at_row(n, row - 1) : 0;
        field_log("croix", w);
        if (lo) {
            if ((*((uint8_t*)w + 0x2888) & 1)) {
                if (f_ishost()) { logf_("[bandeau] croix sur l'IA %08X -> retrait", lo); InterlockedExchange(&g_reqKickLo, (LONG)lo); }
            }
            return 1;
        }
    }
    return o_fremove(w, x);
}
// Les champs du sélecteur local sortent si IsOnlineSessionRunning (0xB833B0). Pour une ligne IA en ligne :
//  - liste (0x8EB550) et choix d'un élément (0x8EB680, type élément [0xF8F5D7C]) : appel de l'original avec
//    « hors ligne » simulé (h_online) et banque d'options NetSetup+0x124 sauvegardée/restaurée (l'original la
//    met à 1 via 0xB8C690) ;
//  - icône du niveau courant sur la ligne (type ligne en ligne [0xF8F5D76]) : contexte {index 0x134 + niveau,
//    type [0xF8F5D0C]} posé avec 0x8A5CD0(widget, &ctx) (thiscall ret 4), comme l'original pour une ligne locale.
typedef char (__fastcall *PFN_ONLINE)(void*, void*);
static PFN_ONLINE o_online; static volatile LONG g_spoofOffline = 0;
static char __fastcall h_online(void* c, void* d) { if (g_spoofOffline) return 0; return o_online(c, d); }
typedef void (__fastcall *PFN_SETCTX)(void*, void*, void*);
static int call_offline(PFN_FIELDCB f, void* w, void* x) {
    uint8_t* n = ns();
    int32_t bank = n ? *(int32_t*)(n + 0x124) : 0;
    InterlockedExchange(&g_spoofOffline, 1);
    int r = f(w, x);
    InterlockedExchange(&g_spoofOffline, 0);
    if (n) *(int32_t*)(n + 0x124) = bank;
    return r;
}
static int __cdecl h_flevels(void* w, void* x) {
    field_log("liste des niveaux", w);
    if (f_online() && o_online) return call_offline(o_flevels, w, x);
    return o_flevels(w, x);
}
static int __cdecl h_flevel(void* w, void* x) {
    field_log("niveau courant", w);
    uint8_t* n = ns();
    if (!w || !n || !f_online() || !o_online || IsBadReadPtr(w, 0x2890)) return o_flevel(w, x);
    uint16_t type = *(uint16_t*)((uint8_t*)w + 0x81E), row1 = *(uint16_t*)((uint8_t*)w + 0x81C);
    if (type == *(uint16_t*)VA(0xF8F5D7C)) {           // élément de la liste déroulante
        int clic = *((uint8_t*)w + 0x2888) & 1;
        int r = call_offline(o_flevel, w, x);
        if (clic && f_ishost()) { refresh_cpu_names(n); ui_pulse(); logf_("[bandeau] niveau choisi dans la liste (element %u)", row1); }
        return r;
    }
    int r = o_flevel(w, x);
    {   static volatile LONG k = 0;
        if (InterlockedIncrement(&k) <= 60)
            logf_("[bandeau] niveau courant : type %04X (ligne en ligne %04X, ligne locale %04X, element %04X) ligne+1 %u IA %08X",
                  type, *(uint16_t*)VA(0xF8F5D76), *(uint16_t*)VA(0xF8F5D7A), *(uint16_t*)VA(0xF8F5D7C), row1, row1 ? ai_at_row(n, row1 - 1) : 0);
    }
    if (type == *(uint16_t*)VA(0xF8F5D76) && row1) {     // ligne du lobby en ligne
        uint32_t lo = ai_at_row(n, row1 - 1);
        if (lo) {
            logf_("[bandeau] icone de niveau posee pour l'IA %08X", lo);
            int lv = cpu_level(n, lo); if (!lv) lv = 3;
            uint16_t ctx[2]; ctx[0] = (uint16_t)(0x134 + lv - 1); ctx[1] = *(uint16_t*)VA(0xF8F5D0C);
            ((PFN_SETCTX)VA(0x8A5CD0))(w, 0, ctx);
        }
    }
    return r;
}
// Bouton « Ajouter l'IA » (BTN_OfflineAddAI copié du lobby local, champ 0x2EE200F2 = 0x8E8DB0). En ligne
// l'original sort sans rien faire (IsOnlineSessionRunning) : chez l'hôte, le clic demande un ajout (traité par
// pump) ; chez l'invité, il est ignoré. Hors ligne : original inchangé.
static PFN_FIELDCB o_faddai;
static int __cdecl h_faddai(void* w, void* x) {
    if (!w || !f_online() || IsBadReadPtr(w, 0x2890)) return o_faddai(w, x);
    if (*((uint8_t*)w + 0x2888) & 1) {
        if (!f_ishost()) logf_("[bandeau] Ajouter l'IA : invite -> ignore");
        else { logf_("[bandeau] Ajouter l'IA -> ajout"); InterlockedExchange(&g_reqAddCpu, 1); }
    }
    return 1;
}
static void install_banner(void) {
    static const uint8_t j1[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x08};
    if (!memcmp((void*)VA(0x7F6CC0), j1, 9)) o_jsonparse = (PFN_JSONPARSE)make_hook(VA(0x7F6CC0), h_jsonparse, 6);
    else logf_("bandeau : signature 0x7F6CC0 inattendue");
    static const uint8_t r1[] = {0x55,0x8b,0xec,0x6a,0xff,0x68}, l1[] = {0x55,0x8b,0xec,0x83,0xec,0x10}, c1[] = {0x55,0x8b,0xec,0x56,0x8b,0x75,0x08};
    if (!memcmp((void*)VA(0x8EB380), r1, 6)) o_fremove = (PFN_FIELDCB)make_hook(VA(0x8EB380), h_fremove, 5); else logf_("bandeau : signature croix inattendue");
    if (!memcmp((void*)VA(0x8EB550), l1, 6)) o_flevels = (PFN_FIELDCB)make_hook(VA(0x8EB550), h_flevels, 6); else logf_("bandeau : signature liste inattendue");
    if (!memcmp((void*)VA(0x8EB680), c1, 7)) o_flevel = (PFN_FIELDCB)make_hook(VA(0x8EB680), h_flevel, 7); else logf_("bandeau : signature niveau inattendue");
    static const uint8_t o1[] = {0x55,0x8b,0xec,0x6a,0xff,0x68};
    if (!memcmp((void*)VA(0xB833B0), o1, 6)) o_online = (PFN_ONLINE)make_hook(VA(0xB833B0), h_online, 5); else logf_("bandeau : signature 0xB833B0 inattendue");
    static const uint8_t a1[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x6a,0xff,0x68};
    if (!memcmp((void*)VA(0x8E8DB0), a1, 9)) o_faddai = (PFN_FIELDCB)make_hook(VA(0x8E8DB0), h_faddai, 6); else logf_("bandeau : signature 0x8E8DB0 inattendue");
    logf_("bandeau IA installe");
}
static void install_spies(void) {
    static const struct { uint32_t a; uint8_t sig[8]; int n; } S[] = {
        { A_GETCOUNT,   {0x33,0xc0,0x81,0xc1,0x30,0x0d,0x00,0x00}, 8 },
        { A_GETID,      {0x55,0x8b,0xec,0x33,0xd2}, 5 },
        { A_GETLOCALID, {0x33,0xd2,0x8d,0x81,0x31,0x0d,0x00,0x00}, 8 },
        { A_ISLOCAL,    {0x55,0x8b,0xec,0x33,0xd2}, 5 },
        { A_ISAI,       {0x55,0x8b,0xec,0x33,0xd2}, 5 },
        { A_FINDIDX,    {0x55,0x8b,0xec,0x56,0x8b,0x75,0x08}, 7 },
        { A_ISTEAMCPU,  {0x55,0x8b,0xec,0x83,0x7d,0x08,0x06}, 7 },
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++)
        if (memcmp((void*)VA(S[i].a), S[i].sig, S[i].n)) { logf_("espion : signature %08X inattendue, espion desactive", S[i].a); return; }
    InitializeCriticalSection(&g_spycs);
    o_getcount   = (PFN_I0)make_hook(VA(A_GETCOUNT),   h_getcount,   8);
    o_getid      = (PFN_Q1)make_hook(VA(A_GETID),      h_getid,      5);
    o_getlocalid = (PFN_Q0)make_hook(VA(A_GETLOCALID), h_getlocalid, 8);
    o_islocal    = (PFN_B1)make_hook(VA(A_ISLOCAL),    h_islocal,    5);
    o_isai       = (PFN_B1)make_hook(VA(A_ISAI),       h_isai,       5);
    o_findidx    = (PFN_I2)make_hook(VA(A_FINDIDX),    h_findidx,    7);
    o_isteamcpu  = (PFN_TEAMCPU)make_hook(VA(A_ISTEAMCPU), h_isteamcpu, 7);
    static const uint8_t k1[] = {0x55,0x8b,0xec,0x83,0xec,0x0c}, k2[] = {0x55,0x8b,0xec,0x6a,0xff,0x68};
    if (!memcmp((void*)VA(0xB83990), k1, 6)) o_kick = (PFN_KICK)make_hook(VA(0xB83990), h_kick, 6); else logf_("menu : signature Kick inattendue");
    if (!memcmp((void*)VA(0xB83200), k2, 6)) o_isself = (PFN_ISSELF)make_hook(VA(0xB83200), h_isself, 5); else logf_("menu : signature 0xB83200 inattendue");
    if (!memcmp((void*)VA(0xB8C7E0), k2, 6)) o_setmute = (PFN_SETMUTE)make_hook(VA(0xB8C7E0), h_setmute, 5); else logf_("menu : signature Mute inattendue");
    static const uint8_t pf[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x6a,0xff};
    if (!memcmp((void*)VA(0x8D2430), pf, 8)) o_profile = (PFN_FIELDCB)make_hook(VA(0x8D2430), h_profile, 6); else logf_("menu : signature Profil inattendue");
    static const uint8_t st[] = {0x55,0x8b,0xec,0x56,0x57,0x68};
    if (!memcmp((void*)VA(0xAE2D10), st, 6)) o_settitle = (PFN_SETTITLE)make_hook(VA(0xAE2D10), h_settitle, 5);
    else logf_("titre d'equipe : signature inattendue");
    logf_("espion installe (7 fonctions)");
}

// ---------- logique de tour ----------
// 0xC5E130 = TurnLogic::Update(obj, dt) cdecl, appelée à chaque image ; état courant à obj+0x48
// (0..23, table de saut 0xC5E31C ; état 7 = 0xC5F2D0 qui lance l'IA). Le stub mémorise obj.
// 0xC14620(k, masque) cdecl = masques « qui contrôle / doit répondre » posés au début de tour.
// 0xB2CA90(team) cdecl = entrée de la table des joueurs de partie pour cette équipe.
#define A_TURNUPDATE 0xC5E130
#define A_SETMASK    0xC14620
#define A_TEAMPLAYER 0xB2CA90
volatile uint32_t g_turnObj = 0;
void* g_turnTramp = 0;
__asm__(".globl _turn_stub\n"
        "_turn_stub:\n"
        "  movl 4(%esp), %eax\n"
        "  movl %eax, _g_turnObj\n"
        "  jmp *_g_turnTramp\n");
extern void turn_stub(void);
typedef void (__cdecl *PFN_MASK)(int, uint32_t);
static PFN_MASK o_setmask;
static volatile LONG g_maskLines = 0;
extern volatile LONG g_turnIsCPU, g_inGameOnline, g_hostIdent;
static void __cdecl h_setmask(int k, uint32_t m) {
    // Tour d'une IA en ligne. IA partagée : le contrôleur du tour (k = 7) devient « tous » (-1), chaque PC
    // joue l'IA. Mode hôte seul : contrôleur (k = 7) et joueur actif (k = 1) = machine de l'hôte.
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    (void)ra;
    if (g_cfg.aiShared && k == 7 && g_turnIsCPU && g_inGameOnline && m != 0xFFFFFFFFu) {
        logf_("[tour] IA partagee : masque 7 %08X -> FFFFFFFF", m); m = 0xFFFFFFFFu;
    }
    if (!g_cfg.aiShared && (k == 7 || k == 1) && g_turnIsCPU && g_inGameOnline && g_hostIdent >= 0 && m != 0xFFFFFFFFu) {
        uint32_t nm = 1u << g_hostIdent;
        if (nm != m) { logf_("[tour] tour IA en ligne : masque %d %08X -> %08X (machine de l'hote)", k, m, nm); m = nm; }
    }
    o_setmask(k, m);
    if (g_cfg.verbose && InterlockedIncrement(&g_maskLines) <= 3000)
        logf_("[tour] masque %d = %08X (appelant %08X)", k, m,
              (unsigned)((uintptr_t)__builtin_return_address(0) - g_base + REF_BASE));
}
// Plusieurs IA : en ligne, la table des joueurs de partie n'a qu'UNE entrée IA (les équipes sont données aux
// joueurs de la session, 0xB7E300 -> 0xB2EAB0 ; les IA suivantes n'en ont pas). Pour
// une équipe IA sans entrée, les recherches par équipe (0xB2CA90 identité, 0xB2CAD0 connexion,
// 0xB2CB60 profil, 0xB2CC20 index) utilisent l'entrée de la première équipe IA qui en a une. Le jeu
// n'a de toute façon qu'une manette virtuelle pour toutes les IA (0xDB0B40 la branche sur chaque
// équipe IA), donc les IA partagent naturellement ce traducteur.
static uint32_t team_alias(uint32_t team) {
    // pas de condition « en ligne » : 0xDB0B40 s'exécute pendant le chargement ; sans entrée, 0xB2CC20
    // renverrait 100 et 0xB2EB30/0xB206B0 écriraient hors de la table. En local chaque IA a son entrée.
    if (team > 64 || !o_isteamcpu) return team;
    uint8_t* pt = (uint8_t*)VA(0xE4C3AF0);
    int aiTeam = -1;
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if (w[5] == team) return team;
        if (aiTeam < 0 && (w[0] & 3) == 3 && (int)w[5] >= 0 && w[1] == 0xFFFFFFFFu && w[9] == 0) aiTeam = (int)w[5];
    }
    if (aiTeam < 0 || !o_isteamcpu(team)) return team;
    static volatile LONG n = 0; static uint32_t lastTeam = 0xFFFFFFFFu;
    if (team != lastTeam && InterlockedIncrement(&n) <= 30) {
        lastTeam = team;
        logf_("[multi-IA] equipe IA %u sans entree : utilise l'entree de l'equipe IA %d", team, aiTeam);
    }
    return (uint32_t)aiTeam;
}
typedef int (__cdecl *PFN_TEAMPL)(uint32_t);
static PFN_TEAMPL o_teamplayer;
static int __cdecl h_teamplayer(uint32_t team) {
    int r = o_teamplayer(team_alias(team));
    // mode hôte seul : l'équipe IA est « contrôlée » par la machine de l'hôte (la table reste intacte)
    if (!g_cfg.aiShared && g_inGameOnline && g_hostIdent >= 0 && r != g_hostIdent && o_isteamcpu && o_isteamcpu(team)) {
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 50) logf_("[tour] equipe IA %u : source %d -> %d (machine de l'hote)", team, r, (int)g_hostIdent);
        r = (int)g_hostIdent;
    }
    spy(7, __builtin_return_address(0), team, r); return r;
}
// Diagnostic : 0xB55BB0 = TurnEnder::SetState(v) thiscall ret 4 (ignoré si état actuel == 4). +0x38 vaut
// 0 au début d'un tour (0xC5FAF2) ; 0xB55AE0(3) dans l'état 10 (0xC61078) termine le tour si +0x38 ∈ {1,3,4}.
typedef void (__fastcall *PFN_TESET)(void*, void*, int);
static PFN_TESET o_teset;
static volatile LONG g_teLines = 0;
static void __fastcall h_teset(void* te, void* edx, int v) {
    int old = (te && !IsBadReadPtr(te, 0x40)) ? *(int*)((uint8_t*)te + 0x38) : -1;
    o_teset(te, edx, v);
    if (InterlockedIncrement(&g_teLines) <= 2000)
        logf_("[fin-de-tour] objet %p : %d -> %d demande (appelant %08X)", te, old, v,
              (unsigned)((uintptr_t)__builtin_return_address(0) - g_base + REF_BASE));
}
// 0xB1B3C0 = Team::Forfeit(team, raison) thiscall ret 8. Raisons : UI_Surrender,
// UI_Complete_Level, RoundTimedOut, NoAllyWormsLeft, AllyVitalWormDied, NetRivalDropped (0xB06A03),
// LocalDropped (0xB06A52). En ligne, au début de chaque tour, le jeu vérifie que l'équipe active a
// un joueur réseau valide (0xB2CC20 → 0xB2BE50) ; l'IA n'en a pas → « LocalDropped » → fin de tour
// (fin-de-tour = 3) et équipe considérée comme partie. On ignore ces deux raisons pour une équipe IA.
typedef void (__fastcall *PFN_FORFEIT)(void*, void*, uint32_t, const char*);
static PFN_FORFEIT o_forfeit;
static void __fastcall h_forfeit(void* self, void* edx, uint32_t team, const char* why) {
    const char* w = (why && !IsBadReadPtr(why, 1)) ? why : "?";
    int dropped = !strcmp(w, "LocalDropped") || !strcmp(w, "NetRivalDropped");
    int cpu = o_isteamcpu ? o_isteamcpu(team) : 0;
    static volatile LONG nf = 0; LONG k = InterlockedIncrement(&nf);
    if (k <= 20 || k % 500 == 0)
    logf_("[forfait] (#%ld) equipe %u raison '%s' (IA=%d, en ligne=%d) appelant %08X%s", k, team, w, cpu, (int)g_inGameOnline,
          (unsigned)((uintptr_t)__builtin_return_address(0) - g_base + REF_BASE),
          (dropped && cpu && g_inGameOnline) ? " -> IGNORE (l'IA n'est pas un joueur deconnecte)" : "");
    if (dropped && cpu && g_inGameOnline) return;
    o_forfeit(self, edx, team, why);
}
// 0xB2CC20(out, team) / 0xB2CAD0(out, team) (cdecl) renvoient le « joueur réseau » d'une équipe
// (cherché dans la table des joueurs de partie par +0x14 = équipe). Le jeu vérifie à chaque image
// (0xB068E0) que le joueur de l'équipe active est connecté (0xB2BE50 / 0x1059AB0) ; pour l'IA il ne
// l'est pas → « NetRivalDropped » en boucle et la synchro de fin de tour ne démarre jamais.
// En ligne, pour une équipe IA, on renvoie le joueur réseau de l'équipe de l'hôte.
static int host_team(void) {
    if (g_hostIdent < 0) return -1;
    uint8_t* pt = (uint8_t*)VA(0xE4C3AF0);   // table des joueurs de partie
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if ((w[0] & 3) == 3 && (int)w[4] == g_hostIdent && (int)w[5] >= 0 && !(w[1] == 0xFFFFFFFFu && w[9] == 0)) return (int)w[5];
    }
    return -1;
}
typedef void* (__cdecl *PFN_TEAMUSER)(void*, uint32_t);
static PFN_TEAMUSER o_teamuser1, o_teamuser2;
static uint32_t redirect_team(uint32_t team, int which) {
    if (!g_inGameOnline || !o_isteamcpu || !o_isteamcpu(team)) return team;
    int ht = host_team();
    if (ht < 0) return team;
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) <= 20) logf_("[joueur-reseau] equipe IA %u -> joueur reseau de l'equipe de l'hote %d (%s)", team, ht, which ? "0xB2CAD0" : "0xB2CC20");
    return (uint32_t)ht;
}
// PAS de redirection pour le code de la manette IA (0xDB0B40..0xDB10F0) :
//  - 0xDB0B40 (chargement) : 0xB2CC20(IA) -> 0xB2EB30 vide le traducteur du joueur puis 0xB206B0 y branche
//    WormLeft… sur la manette IA. Redirigé, ce serait le traducteur de l'HÔTE qui serait rebranché.
//  - 0xDB0F10 (après chaque appui IA) et 0xDB0FB0 (direction analogique, jetpack) : remise à jour des
//    entrées du joueur de l'équipe active (0xDB10AF -> 0xB2D870).
static void* __cdecl h_teamuser1(void* out, uint32_t team) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    if (ra >= VA(0xDB0B40) && ra < VA(0xDB10F0)) {
        void* r = o_teamuser1(out, team_alias(team));
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 12) {
            uint32_t pl = (out && !IsBadReadPtr(out, 4)) ? *(uint32_t*)out : 0;
            logf_("[manette IA] 0xB2CC20(equipe %u) depuis %08X : non redirige, joueur %08X", team, (unsigned)(ra - g_base + REF_BASE), pl);
        }
        return r;
    }
    return o_teamuser1(out, redirect_team(team_alias(team), 0));
}
// Pas de redirection vers l'hôte pour l'AFFICHAGE, sinon les équipes IA prendraient le nom Steam de l'hôte.
static void* __cdecl h_teamuser2(void* out, uint32_t team) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    // affichages de nom suivant le schéma « joueur réseau connecté ? nom Steam (0x1059A00) : nom d'équipe
    // (0xACF590) » (adresses de retour des appels 0xB2CAD0) : listes, tour, abandon, fabrication…
    static const uint32_t NAME_RA[] = { 0xACF69D, 0xD57207, 0x8D16B8, 0x8D1F90, 0x8D216C, 0xB15924,
                                        0xB1B5E4, 0xB4E1BE, 0xC5F6B1, 0xCE361D };
    for (unsigned k = 0; k < sizeof NAME_RA / sizeof NAME_RA[0]; k++)
        if (ra == VA(NAME_RA[k])) return o_teamuser2(out, team_alias(team));
    return o_teamuser2(out, redirect_team(team_alias(team), 1));
}
static PFN_TEAMUSER o_teamprof;   // 0xB2CB60(out, équipe) = profil de l'entrée (+0x1C)
static void* __cdecl h_teamprof(void* out, uint32_t team) { return o_teamprof(out, team_alias(team)); }
static void install_turn_trace(void) {
    static const uint8_t s1[] = {0x55,0x8b,0xec,0xf3,0x0f,0x10,0x45,0x0c};
    static const uint8_t s2[] = {0x55,0x8b,0xec,0x6a,0xff};
    static const uint8_t s3[] = {0x55,0x8b,0xec,0x8b,0x55,0x08};
    if (memcmp((void*)VA(A_TURNUPDATE), s1, 8) || memcmp((void*)VA(A_SETMASK), s2, 5) || memcmp((void*)VA(A_TEAMPLAYER), s3, 6)) {
        logf_("traceur de tour : signature inattendue, desactive"); return;
    }
    g_turnTramp  = make_hook(VA(A_TURNUPDATE), (void*)turn_stub, 8);
    o_setmask    = (PFN_MASK)make_hook(VA(A_SETMASK), h_setmask, 5);
    o_teamplayer = (PFN_TEAMPL)make_hook(VA(A_TEAMPLAYER), h_teamplayer, 6);
    static const uint8_t s4[] = {0x55,0x8b,0xec,0x83,0x79,0x38,0x04};
    if (!memcmp((void*)VA(0xB55BB0), s4, 7)) o_teset = (PFN_TESET)make_hook(VA(0xB55BB0), h_teset, 7);
    else logf_("fin-de-tour : signature inattendue");
    static const uint8_t s5[] = {0x55,0x8b,0xec,0x6a,0xff};
    if (!memcmp((void*)VA(0xB1B3C0), s5, 5)) o_forfeit = (PFN_FORFEIT)make_hook(VA(0xB1B3C0), h_forfeit, 5);
    else logf_("forfait : signature inattendue");
    static const uint8_t s6[] = {0x55,0x8b,0xec,0x8b,0x55,0x0c};
    static const uint8_t s7[] = {0x55,0x8b,0xec,0x6a,0xff};
    if (!memcmp((void*)VA(0xB2CC20), s6, 6) && !memcmp((void*)VA(0xB2CAD0), s7, 5)) {
        o_teamuser1 = (PFN_TEAMUSER)make_hook(VA(0xB2CC20), h_teamuser1, 6);
        o_teamuser2 = (PFN_TEAMUSER)make_hook(VA(0xB2CAD0), h_teamuser2, 5);
    } else logf_("joueur-reseau : signature inattendue");
    static const uint8_t s8[] = {0x55,0x8b,0xec,0x6a,0xff,0x68};
    if (!memcmp((void*)VA(0xB2CB60), s8, 6)) o_teamprof = (PFN_TEAMUSER)make_hook(VA(0xB2CB60), h_teamprof, 5);
    else logf_("multi-IA : signature 0xB2CB60 inattendue");
    logf_("traceur de tour installe");
}
// Appelée depuis pump() : note chaque changement d'état et les champs de l'objet qui ont bougé
static uint32_t g_tlSnap[0x80]; static uint32_t g_tlObj = 0; static int g_tlState = -1; static DWORD g_tlSince = 0, g_tlStuckLog = 0;
static void net_watch(void) {
    if (!g_cfg.verbose) return;
    // synchro de fin de tour : 0x58BA008 attente, 0x58BA00A/00B masques, 0xE510D48 phase, 0xE510E60 sous-état
    static uint32_t last[6] = {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF};
    uint32_t cur[6];
    cur[0] = *(uint8_t*)VA(0x58BA008); cur[1] = *(uint8_t*)VA(0x58BA00A); cur[2] = *(uint8_t*)VA(0x58BA00B);
    cur[3] = *(uint32_t*)VA(0xE510D48);
    uint32_t f = *(uint32_t*)VA(0xE510E60); cur[4] = f ? (uint32_t)(f - g_base + REF_BASE) : 0;
    uint32_t te = g_turnObj ? *(uint32_t*)(uintptr_t)(g_turnObj + 0x40) : 0;
    cur[5] = (te && !IsBadReadPtr((void*)(uintptr_t)te, 0x40)) ? *(uint32_t*)(uintptr_t)(te + 0x38) : 0xEEEE;
    if (memcmp(cur, last, sizeof cur)) {
        logf_("[reseau] attente=%u masqueA=%02X masqueB=%02X phase=%u sous-etat=%08X fin-de-tour=%u",
              cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
        memcpy(last, cur, sizeof cur);
    }
}
static void turn_watch(void) {
    net_watch();
    uint32_t obj = g_turnObj;
    if (!obj || IsBadReadPtr((void*)(uintptr_t)obj, 0x200)) return;
    uint32_t cur[0x80]; memcpy(cur, (void*)(uintptr_t)obj, sizeof cur);
    int st = (int)cur[0x48 / 4];
    DWORD now = GetTickCount();
    if (obj != g_tlObj) {
        logf_("[tour] nouvel objet TurnLogic %08X, etat %d", obj, st);
        g_tlObj = obj; g_tlState = st; g_tlSince = now; memcpy(g_tlSnap, cur, sizeof cur); return;
    }
    if (st != g_tlState) {
        char buf[900]; int o = 0;
        for (int i = 0; i < 0x80 && o < (int)sizeof buf - 40; i++)
            if (cur[i] != g_tlSnap[i] && i != 0x48 / 4)
                o += snprintf(buf + o, sizeof buf - o, " +%02X:%X>%X", i * 4, g_tlSnap[i], cur[i]);
        buf[o] = 0;
        logf_("[tour] etat %d -> %d (apres %lu ms) |%s", g_tlState, st, now - g_tlSince, buf);
        g_tlState = st; g_tlSince = now; memcpy(g_tlSnap, cur, sizeof cur);
    } else if (now - g_tlSince > 5000 && now - g_tlStuckLog > 5000) {
        g_tlStuckLog = now;
        char buf[900]; int o = 0;
        for (int i = 0; i < 0x80 && o < (int)sizeof buf - 40; i++)
            if (cur[i] != g_tlSnap[i])
                o += snprintf(buf + o, sizeof buf - o, " +%02X:%X>%X", i * 4, g_tlSnap[i], cur[i]);
        buf[o] = 0;
        logf_("[tour] toujours etat %d depuis %lu ms |%s", st, now - g_tlSince, buf);
        memcpy(g_tlSnap, cur, sizeof cur);
    }
}


static LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    static int n = 0;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC000001D || c == 0xC0000096) {
        if (n++ < 10) logf_("EXCEPTION %08X a EIP=%08X (rva %08X)", c, (unsigned)e->ContextRecord->Eip, (unsigned)(e->ContextRecord->Eip - g_base + REF_BASE));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------- table des joueurs de partie (0xE4C3AF0, 12 x 0x50) ----------
// Entrée : +0 drapeaux (3 = actif, 0x800 = ce PC), +4/+8/+C manettes (-1 = aucune),
// +0x10 identité, +0x14 rang, +0x24 connexion réseau (0 pour une IA).
#define A_PLAYERS 0xE4C3AF0
static uint32_t g_ptSnap[24]; static int g_ptInit = 0;
// On retient l'identité de la machine de l'hôte dans la table de CE PC (hôte : son entrée locale avec
// manettes ; invité : l'entrée distante avec connexion), utilisée en mode hôte seul (h_setmask, h_teamplayer).
volatile LONG g_inGameOnline = 0, g_isHostCached = 0, g_hostIdent = -1;
static void players_watch(uint8_t* n) {
    uint8_t* pt = (uint8_t*)VA(A_PLAYERS);
    int online = n && f_online(), host = online && f_ishost();
    // valeurs « collantes » pendant une partie (le drapeau 0x800 de l'entrée locale et
    // f_online/f_ishost clignotent parfois en cours de partie → identité perdue au début du tour IA)
    int inGame = 0;
    for (int i = 0; i < 12; i++) { uint32_t* w = (uint32_t*)(pt + i * 0x50); if ((w[0] & 3) == 3 && (int)w[5] >= 0) inGame = 1; }
    if (online) { g_inGameOnline = 1; g_isHostCached = host; }
    else if (!inGame) { g_inGameOnline = 0; g_isHostCached = 0; }
    host = g_isHostCached;
    int changed = !g_ptInit, hostId = -1;
    // Hôte : son entrée = celle qui a des périphériques d'entrée locaux (+4 != -1 ; clavier compris),
    // vérifié dans tous nos logs. Sécurité : s'il n'y en a pas exactement une, repli sur le drapeau 0x800.
    // Client : l'hôte = entrée distante (sans entrée locale, avec connexion) de l'équipe 0.
    int nLocal = 0, idLocal = -1, id800 = -1, nRemote = 0, idRemote = -1;
    for (int i = 0; i < 12 && g_inGameOnline; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if ((w[0] & 3) != 3 || (int)w[5] < 0) continue;
        if (w[1] != 0xFFFFFFFFu) { nLocal++; idLocal = (int)w[4]; }
        if ((w[0] & 0x800) && w[9] != 0) id800 = (int)w[4];
        if (w[1] == 0xFFFFFFFFu && w[9] != 0) { nRemote++; idRemote = (int)w[4]; }
    }
    // Client : l'hôte = l'UNIQUE humain distant (connexion réseau, pas d'entrée locale). Les numéros
    // d'équipe sont redistribués au lancement (l'hôte n'est pas forcément l'équipe 0). À 3 humains ou
    // plus, il faudrait identifier l'hôte autrement (Steam ID) : avertissement et pas de redirection.
    if (!host && inGame) {
        if (nRemote == 1) hostId = idRemote;
        else {
            static int warned2 = 0;
            if (!warned2++) logf_("[joueurs] AVERTISSEMENT : %d humains distants (attendu 1) -> hote non identifie, IA non redirigee", nRemote);
        }
    }
    if (host && inGame) {
        if (nLocal == 1) hostId = idLocal;
        else {
            static int warned = 0;
            if (!warned++) logf_("[joueurs] AVERTISSEMENT : %d entrees avec entrees locales (attendu 1) -> repli sur le drapeau 'ce PC' (%d)", nLocal, id800);
            hostId = id800;
        }
    }
    // identification par Steam ID (*(entrée.+0x24 connexion)+0x18), si fourni dans wmdmod.ini
    if (inGame && g_cfg.hostSteamId) {
        for (int i = 0; i < 12; i++) {
            uint32_t* w = (uint32_t*)(pt + i * 0x50);
            if ((w[0] & 3) != 3 || (int)w[5] < 0 || w[9] < 0x10000 || IsBadReadPtr((void*)(uintptr_t)w[9], 0x20)) continue;
            if (*(unsigned long long*)(uintptr_t)(w[9] + 0x18) == g_cfg.hostSteamId) { hostId = (int)w[4]; break; }
        }
    }
    // IA partagée : en ligne, le jeu ne lit les entrées (traducteur +0x28) que des joueurs
    // marqués « ce PC » (0x800) ; les autres arrivent par le réseau. L'IA tourne sur chaque PC : on
    // marque donc son entrée « ce PC » sur CHAQUE PC pour que sa manette virtuelle soit lue localement.
    if (inGame && g_inGameOnline && g_cfg.aiShared) {
        for (int i = 0; i < 12; i++) {
            uint32_t* w = (uint32_t*)(pt + i * 0x50);
            if ((w[0] & 3) == 3 && w[1] == 0xFFFFFFFFu && w[9] == 0 && (int)w[5] >= 0 && !(w[0] & 0x800)) {
                w[0] |= 0x800;
                logf_("[joueurs] IA partagee : entree %d (equipe %d, identite %u) marquee 'ce PC'", i, (int)w[5], w[4]);
            }
        }
    }
    if (!inGame) hostId = -1;
    else if (hostId < 0) hostId = g_hostIdent;   // garder la dernière valeur connue pendant la partie
    if (hostId != g_hostIdent) { g_hostIdent = hostId; logf_("[joueurs] identite de la machine de l'hote sur ce PC = %d", hostId); }
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if (w[0] != g_ptSnap[i] || w[9] != g_ptSnap[12 + i]) changed = 1;
    }
    if (!changed) return;
    // diagnostic : emplacement des Steam ID du lobby (liste visible +0xD30, id 64 bits à +0x10) dans l'entrée
    // et dans les objets pointés par l'entrée (0x400 premiers octets).
    if (n && inGame) {
        static int scans = 0;
        if (scans < 3) {
            scans++;
            for (int L = 0; L < 6; L++) {
                uint8_t* e = n + NS_LIST + L * 0x18;
                if (!e[E_ACTIVE]) continue;
                uint32_t lo = *(uint32_t*)(e + E_ID), hi = *(uint32_t*)(e + E_ID + 4);
                if (hi == 0) continue;   // IA (id du mod)
                for (int i = 0; i < 12; i++) {
                    uint32_t* w = (uint32_t*)(pt + i * 0x50);
                    if ((w[0] & 3) != 3 || (int)w[5] < 0) continue;
                    for (int k = 0; k < 19; k++)
                        if (w[k] == lo && w[k + 1] == hi) logf_("[steamid] %08X%08X trouve dans l'entree %d, mot %d", hi, lo, i, k);
                    for (int k = 1; k < 20; k++) {
                        uint8_t* o = (uint8_t*)(uintptr_t)w[k];
                        if (w[k] < 0x10000 || IsBadReadPtr(o, 0x400)) continue;
                        for (int off = 0; off + 8 <= 0x400; off += 4)
                            if (*(uint32_t*)(o + off) == lo && *(uint32_t*)(o + off + 4) == hi)
                                logf_("[steamid] %08X%08X trouve dans l'entree %d (equipe %d, identite %u) : *(mot %d)+0x%X",
                                      hi, lo, i, (int)w[5], w[4], k, off);
                    }
                }
            }
        }
    }
    g_ptInit = 1;
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        g_ptSnap[i] = w[0]; g_ptSnap[12 + i] = w[9];
        if (w[0]) logf_("[joueurs] %2d : drapeaux %08X manettes %d/%d/%d identite %u equipe %d connexion %08X",
                        i, w[0], (int)w[1], (int)w[2], (int)w[3], w[4], (int)w[5], w[9]);
    }
}

// ---------- entrées de l'IA en ligne ----------
// Les boutons virtuels de l'IA sont écrits directement dans le ver (+0x816 via 0xB47E40) et ne circulent
// pas sur le réseau. Mode IA partagée (par défaut) : l'IA tourne sur chaque PC, rien n'est envoyé.
// Mode hôte seul (IAPartagee=0) : le choix d'arme de l'IA (0xB47E20 -> Worm::RequestCurrentEquip
// 0xB4EC60(arme, 0), 0 = « ne pas envoyer ») est rediffusé par 0xB4EC60(arme, 1) (envoi 0xAC8170).
static int ai_turn_online_host(void) {
    if (!(g_inGameOnline && (g_isHostCached || g_cfg.aiShared) && g_turnIsCPU && g_turnObj)) return 0;
    int st = *(int*)(uintptr_t)(g_turnObj + 0x48);
    return st >= 6 && st <= 13;
}
typedef void (__fastcall *PFN_PADSET)(void*, void*, uint32_t, int);
static PFN_PADSET o_padset;
static volatile LONG g_padSetLog = 0;
static void __fastcall h_padset(void* worm, void* edx, uint32_t bit, int on) {   // diagnostic
    if (g_cfg.verbose && InterlockedIncrement(&g_padSetLog) <= 200)
        logf_("[IA-entrees] 0xB47E40 bit %u etat %d (tour IA en ligne=%d)", bit, on, ai_turn_online_host());
    o_padset(worm, edx, bit, on);
}
typedef void (__fastcall *PFN_REQEQUIP)(void*, void*, uint32_t, int);
typedef void (__fastcall *PFN_THIS0V)(void*, void*);
typedef void (__fastcall *PFN_AISELECT)(void*, void*, uint32_t);
static PFN_REQEQUIP f_reqequip; static PFN_THIS0V f_equipdone; static PFN_AISELECT o_aiselect;
// Pendant sa réflexion l'IA passe en revue des centaines d'armes (0xB47E20 en boucle) : en mode hôte seul,
// on ne diffuse que le choix stable depuis 1,2 s (et juste avant un tir), depuis ai_equip_pump().
static void* volatile g_aiEquipWorm = 0; static volatile LONG g_aiEquip = -1, g_aiEquipSent = -1;
static volatile DWORD g_aiEquipTime = 0;
static volatile LONG g_aiLastEquip = -1;   // dernière arme demandée par l'IA (tous modes)
static void __fastcall h_aiselect(void* worm, void* edx, uint32_t equip) {
    o_aiselect(worm, edx, equip);          // local, sans réseau (comme l'original)
    g_aiLastEquip = (LONG)equip;
    if (ai_turn_online_host()) {
        if ((LONG)equip != g_aiEquip) { g_aiEquip = (LONG)equip; g_aiEquipTime = GetTickCount(); }
        g_aiEquipWorm = worm;
    }
}
static void ai_equip_flush(const char* why) {
    if (!g_aiEquipWorm || g_aiEquip < 0 || g_aiEquip == g_aiEquipSent) return;
    f_reqequip((void*)g_aiEquipWorm, 0, (uint32_t)g_aiEquip, 1);   // 1 = diffuser (0xAC8170)
    f_equipdone((void*)g_aiEquipWorm, 0);
    g_aiEquipSent = g_aiEquip;
    logf_("[IA-entrees] arme %ld envoyee sur le reseau (%s)", g_aiEquip, why);
}
static void ai_equip_pump(void) {
    if (!ai_turn_online_host()) { g_aiEquip = -1; g_aiEquipSent = -1; g_aiEquipWorm = 0; return; }
    if (!g_cfg.aiShared && g_aiEquip >= 0 && GetTickCount() - g_aiEquipTime > 1200) ai_equip_flush("choix stable");
}
// Boutons de l'IA lus là où le ver les consomme : 0xB4C7C0 (thiscall, out*) lit ver+0x816 (bits 0..14).
// Diagnostic, et en mode hôte seul : envoi de l'arme juste avant un tir.
typedef char (__fastcall *PFN_PADREAD)(void*, void*, uint32_t*);
static PFN_PADREAD o_padread;
static volatile LONG g_padReadLog = 0, g_heldLog = 0;
static char __fastcall h_padread(void* worm, void* edx, uint32_t* out) {
    if (ai_turn_online_host() && worm) {
        uint16_t v = *(uint16_t*)((uint8_t*)worm + 0x816);
        if (v) {
            if (g_cfg.verbose && InterlockedIncrement(&g_padReadLog) <= 300) logf_("[IA-entrees] ver %p boutons %04X", worm, v);
            if ((v & 0x0040) && !g_cfg.aiShared) ai_equip_flush("avant tir");
        }
    }
    return o_padread(worm, edx, out);
}
// Diagnostic : boutons « tenus » par l'IA (objet [0xE520788]+0x48)
typedef void (__fastcall *PFN_HELD)(void*, void*, uint32_t);
static PFN_HELD o_heldbit, o_heldmask;
static void __fastcall h_heldbit(void* o, void* edx, uint32_t bit) {
    if (g_cfg.verbose && ai_turn_online_host() && InterlockedIncrement(&g_heldLog) <= 300) logf_("[IA-entrees] tenu bit %u", bit);
    o_heldbit(o, edx, bit);
}
static void __fastcall h_heldmask(void* o, void* edx, uint32_t m) {
    if (g_cfg.verbose && ai_turn_online_host() && InterlockedIncrement(&g_heldLog) <= 300) logf_("[IA-entrees] tenu masque %08X", m);
    o_heldmask(o, edx, m);
}
// Mode hôte seul (TraducteurIA) : l'IA joue avec une manette virtuelle (IA+0x4E450, 0xDB10F0(emplacement, valeur)), qui est
// un vrai périphérique d'entrée lu par le traducteur d'entrées du joueur IA (entrée IA +0x28). En
// ligne, le tour de l'IA est piloté par la machine de l'hôte, qui enregistre et envoie les actions
// de SON traducteur (entrée locale +0x28). Pendant le tour IA, on donne donc à l'entrée locale de
// l'hôte le traducteur de l'IA ; on rend l'original dès la fin du tour.
static uint32_t* g_swapHost = NULL; static uint32_t g_swapOrig = 0, g_swapAI = 0;
typedef int (__fastcall *PFN_CURTEAM)(void*, void*);
static void mapper_swap_pump(void) {
    int want = g_cfg.swapMapper && !g_cfg.aiShared && ai_turn_online_host();
    if (!want) {
        if (g_swapHost) {
            if (g_swapHost[10] == g_swapAI) g_swapHost[10] = g_swapOrig;
            logf_("[traducteur] fin du tour IA : traducteur de l'hote restaure (%08X)", g_swapOrig);
            g_swapHost = NULL;
        }
        return;
    }
    if (g_swapHost) return;
    uint8_t* tm = *(uint8_t**)VA(0xE4C506C);
    if (!tm) return;
    int team = ((PFN_CURTEAM)VA(0xB3E5B0))(tm, 0);          // équipe dont c'est le tour
    uint8_t* pt = (uint8_t*)VA(0xE4C3AF0);
    uint32_t *host = NULL, *ai = NULL;
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if ((w[0] & 3) != 3 || (int)w[5] < 0) continue;
        if ((int)w[4] == g_hostIdent && w[1] != 0xFFFFFFFFu) host = w;
        if ((int)w[5] == (int)team_alias((uint32_t)team) && w[1] == 0xFFFFFFFFu && w[9] == 0) ai = w;
    }
    if (!host || !ai || !ai[10] || !host[10]) {
        static DWORD lw = 0;
        if (GetTickCount() - lw > 3000) { lw = GetTickCount(); logf_("[traducteur] entree hote/IA introuvable (equipe %d, hote %p, IA %p)", team, host, ai); }
        return;
    }
    g_swapHost = host; g_swapOrig = host[10]; g_swapAI = ai[10];
    host[10] = ai[10];
    logf_("[traducteur] tour IA (equipe %d) : l'hote utilise le traducteur de l'IA %08X (le sien : %08X)", team, g_swapAI, g_swapOrig);
}
static void ai_input_pump(void) {
    mapper_swap_pump();
    ai_equip_pump();
}
static void install_ai_input(void) {
    static const uint8_t a1[] = {0x55,0x8b,0xec,0x8d,0x91,0x16,0x08,0x00,0x00};
    static const uint8_t a2[] = {0x33,0xc0,0x66,0x89,0x81,0x16,0x08,0x00,0x00};
    static const uint8_t a3[] = {0x55,0x8b,0xec,0x56,0x6a,0x00};
    if (memcmp((void*)VA(0xB47E40), a1, 9) || memcmp((void*)VA(0xB48130), a2, 9) || memcmp((void*)VA(0xB47E20), a3, 6)) {
        logf_("IA-entrees : signature inattendue, desactive"); return;
    }
    o_padset    = (PFN_PADSET)make_hook(VA(0xB47E40), h_padset, 9);
    f_reqequip  = (PFN_REQEQUIP)VA(0xB4EC60);
    f_equipdone = (PFN_THIS0V)VA(0xB4E850);
    o_aiselect  = (PFN_AISELECT)make_hook(VA(0xB47E20), h_aiselect, 6);
    static const uint8_t a4[] = {0x55,0x8b,0xec,0x53,0x56,0x57,0x33,0xff};
    static const uint8_t a5[] = {0x55,0x8b,0xec,0x8b,0xd1,0x8b,0x4d,0x08};
    static const uint8_t a6[] = {0x55,0x8b,0xec,0x8b,0x45,0x08};
    if (!memcmp((void*)VA(0xB4C7C0), a4, 8)) o_padread = (PFN_PADREAD)make_hook(VA(0xB4C7C0), h_padread, 8);
    else logf_("IA-entrees : signature 0xB4C7C0 inattendue");
    if (!memcmp((void*)VA(0xCEF7C0), a5, 8)) o_heldbit = (PFN_HELD)make_hook(VA(0xCEF7C0), h_heldbit, 8);
    if (!memcmp((void*)VA(0xCEF7E0), a6, 6)) o_heldmask = (PFN_HELD)make_hook(VA(0xCEF7E0), h_heldmask, 6);
    logf_("IA-entrees installe");
}

// ---------- chaîne d'entrées de l'IA (diagnostic + rebranchement) ----------
// Maillons : (1) manette virtuelle de l'IA 0xDB10F0(emplacement, valeur) ; (2) traducteur d'entrées
// (entrée+0x28 : tableau de nœuds [+8, +0xC[ indexé par numéro d'action, nœud->vfunc[2]() = valeur) ;
// (3) événements d'action reçus par le ver 0xB4DB60(?, événement) (type 0xD640B040, action 0xD6B020).
typedef void  (__fastcall *PFN_VPAD)(void*, void*, uint32_t, float);
typedef char  (__fastcall *PFN_WORMEVT)(void*, void*, uint32_t, uint32_t);
typedef int   (__fastcall *PFN_EVTGET)(void*, void*);
typedef float (__fastcall *PFN_NODEVAL)(void*, void*);
typedef int   (__cdecl   *PFN_ACTIDX)(const char*);
static PFN_VPAD o_vpad; static PFN_WORMEVT o_wormevt;
static float g_vpadLast[16];
static volatile LONG g_vpadLog = 0, g_evtLogAI = 0, g_evtLogHuman = 0;
static const char* ACT_NAMES[] = { "WormLeft", "WormRight", "WormUp", "WormDown", "WormJumpPrimary",
    "WormJumpSecondary", "WormFireWeapon", "WormAltFireWeapon", "TargetSelect", "FuseNext", "WormEnterExitVehicle", 0 };
static int g_actIdx[16]; static int g_actInit = 0;
static void act_init(void) {
    if (g_actInit) return;
    for (int i = 0; ACT_NAMES[i]; i++) g_actIdx[i] = ((PFN_ACTIDX)VA(0xBB7360))(ACT_NAMES[i]);
    g_actInit = 1;
    char b[400]; int o = 0;
    for (int i = 0; ACT_NAMES[i]; i++) o += snprintf(b + o, sizeof b - o, " %s=%d", ACT_NAMES[i], g_actIdx[i]);
    logf_("[diag] numeros d'action :%s", b);
}
static const char* act_name(int idx) {
    for (int i = 0; ACT_NAMES[i]; i++) if (g_actIdx[i] == idx) return ACT_NAMES[i];
    return "?";
}
static volatile uint32_t g_vpadDev = 0, g_vpadObj = 0;
// File de commandes de l'IA : 0xDCBD80(id, nom) thiscall ret 8 (empile une commande ; le nom est
// la chaîne « kAICommand_... »). On logue chaque commande (sans les répétitions consécutives).
typedef void (__fastcall *PFN_AICMD)(void*, void*, uint32_t, const char*);
static PFN_AICMD o_aicmd;
static volatile LONG g_aiCmdLog = 0;
static void __fastcall h_aicmd(void* q, void* edx, uint32_t id, const char* name) {
    o_aicmd(q, edx, id, name);
    if (!g_cfg.verbose) return;
    static uint32_t lastId = 0xFFFFFFFFu; static LONG rep_ = 0;
    if (id == lastId) { rep_++; return; }
    if (rep_ && g_aiCmdLog < 1500) logf_("[IA cmd]   (precedente repetee %ld fois de plus)", rep_);
    if (InterlockedIncrement(&g_aiCmdLog) <= 1500)
        logf_("[IA cmd] %04X %s", id, (name && !IsBadReadPtr(name, 4)) ? name : "?");
    lastId = id; rep_ = 0;
}
static void __fastcall h_vpad(void* pad, void* edx, uint32_t slot, float v) {
    o_vpad(pad, edx, slot, v);
    if (pad && !IsBadReadPtr(pad, 4)) {
        uint32_t dev = *(uint32_t*)pad;
        g_vpadObj = (uint32_t)(uintptr_t)pad;
        if (dev != g_vpadDev) { g_vpadDev = dev; logf_("[diag] manette IA %p : peripherique %08X", pad, dev); }
    }
    if (g_cfg.verbose && slot < 16 && v != g_vpadLast[slot]) {
        if (slot == 4 && v > 0) logf_("[IA arme] appui tir/validation avec l'arme %ld", g_aiLastEquip);
        g_vpadLast[slot] = v;
        if (InterlockedIncrement(&g_vpadLog) <= 600) logf_("[diag] manette IA %p : emplacement %u = %.2f", pad, slot, v);
    }
}
static char __fastcall h_wormevt(void* worm, void* edx, uint32_t a, uint32_t evt) {
    uint32_t e0 = evt;
    if (g_cfg.verbose && ((PFN_EVTGET)VA(0xD6B040))(&e0, 0) != -1) {
        int ai = g_turnIsCPU ? 1 : 0;
        uint32_t e = evt;
        int type = ((PFN_EVTGET)VA(0xD6B040))(&e, 0);
        int act  = ((PFN_EVTGET)VA(0xD6B020))(&e, 0);
        // on ignore le bruit (type 13, actions 51-54 à chaque image) ; une ligne par nouvelle action
        static int lastAct = -1, lastType = -1; static DWORD lastT = 0; static LONG same = 0;
        if (!(type == 13 && act >= 51 && act <= 54)) {
            DWORD now = GetTickCount();
            if (act != lastAct || (type != lastType && type == 14) || now - lastT > 1000) {
                volatile LONG* cnt = ai ? &g_evtLogAI : &g_evtLogHuman;
                if (InterlockedIncrement(cnt) <= 1500) {
                    act_init();
                    logf_("[diag] ver %p recoit evenement type %d action %d (%s) [tour %s] (%ld identiques avant)", worm, type, act, act_name(act), ai ? "IA" : "humain", same);
                }
                same = 0; lastT = now;
            } else same++;
            lastAct = act; lastType = type;
        }
    }
    return o_wormevt(worm, edx, a, evt);
}
// valeur d'une action dans un traducteur (lecture seule)
static float mapper_value(uint32_t mapper, int idx) {
    if (!mapper || idx < 0 || IsBadReadPtr((void*)(uintptr_t)mapper, 0x10)) return -9;
    uint32_t* beg = *(uint32_t**)(uintptr_t)(mapper + 8); uint32_t* end = *(uint32_t**)(uintptr_t)(mapper + 0xC);
    if (!beg || beg + idx >= end || IsBadReadPtr(beg + idx, 4)) return -8;
    void* node = (void*)(uintptr_t)beg[idx];
    if (!node || IsBadReadPtr(node, 8)) return -7;
    void** vt = *(void***)node;
    if (!vt || IsBadReadPtr(vt, 12)) return -6;
    // en sortie de partie le traducteur peut être libéré (vfunc nulle -> crash EIP=0)
    if ((uintptr_t)vt[2] < VA(0x7F1000) || (uintptr_t)vt[2] >= VA(0x13C8000)) return -5;
    return ((PFN_NODEVAL)vt[2])(node, 0);
}
// En ligne, la manette virtuelle de l'IA reçoit bien les appuis mais le traducteur de l'IA reste à 0 :
// ses nœuds « touche » (vtable 0x164CB8C : source +8, index +0xC, valeur =
// source+0x20[index], 0xD53DA0) ne lisent pas le périphérique de la manette. On relève les nœuds
// du traducteur IA et, pendant le tour de l'IA, on fait lire leur valeur dans la manette de l'IA.
#define VT_KEYNODE 0x164CB8C
#define VT_ORNODE  0x164CBD4
#define VT_THRNODE 0x164CBB0
#define REFA(x) ((uint32_t)(x) - (uint32_t)VA(0x7F0000) + 0x7F0000u)
typedef float (__fastcall *PFN_KEYVAL)(void*, void*);
static PFN_KEYVAL o_keyval;
static uint32_t g_aiNodes[256]; static int g_aiNodeAct[256]; static volatile int g_aiNodeCount = 0;
// action (rang dans ACT_NAMES) -> emplacement de la manette IA (0 gauche, 1 droite, 2 haut, 3 bas, 4 tir,
// 5 saut arrière, 6 saut, 7 mèche suivante, 9 cible, 10 véhicule) ; -1 = pas d'équivalent
static const int ACT_SLOT[] = { 0, 1, 2, 3, 6, 5, 4, -1, 9, 7, 10 };
static int g_collectAct = -1;
static uint32_t g_aiNodesMapper = 0, g_aiNodesDev = 0;
static volatile LONG g_rebindLog = 0, g_rebindHits = 0;
static int ai_node_find(uint32_t n) {
    int c = g_aiNodeCount;
    for (int i = 0; i < c; i++) if (g_aiNodes[i] == n) return i;
    return -1;
}
static int is_ai_node(uint32_t n) { return ai_node_find(n) >= 0; }
// valeur actuelle d'un emplacement de la manette IA : périphérique(*pad)+0x20[pad[1+emplacement]]
static int vpad_slot_value(int slot, float* out) {
    uint32_t pad = g_vpadObj;
    if (!pad || slot < 0 || slot > 10 || IsBadReadPtr((void*)(uintptr_t)pad, 0x30)) return 0;
    uint32_t dev = *(uint32_t*)(uintptr_t)pad, idx = ((uint32_t*)(uintptr_t)pad)[1 + slot];
    if (!dev || idx >= 64 || IsBadReadPtr((void*)(uintptr_t)dev, 0x24)) return 0;
    float* st = *(float**)(uintptr_t)(dev + 0x20);
    if (!st || IsBadReadPtr(st + idx, 4)) return 0;
    *out = st[idx]; return 1;
}
static float __fastcall h_keyval(void* node, void* edx) {
    uint32_t dev = g_vpadDev;
    if (dev && g_turnIsCPU && g_inGameOnline && g_aiNodeCount) {
        int k = ai_node_find((uint32_t)(uintptr_t)node);
        uint32_t* w = (uint32_t*)node;
        if (k >= 0 && w[2] != dev) {
            float v = 0; int ok = 0;
            uint32_t svt = (w[2] && !IsBadReadPtr((void*)(uintptr_t)w[2], 4)) ? *(uint32_t*)(uintptr_t)w[2] : 0;
            if (svt && svt == *(uint32_t*)(uintptr_t)dev && w[3] < 64) {
                // même sorte de périphérique (une autre manette IA) : même numéro de touche
                float* st = *(float**)(uintptr_t)(dev + 0x20);
                if (st && !IsBadReadPtr(st + w[3], 4)) { v = st[w[3]]; ok = 1; }
            } else {
                // autre périphérique (clavier, manette…) : correspondance par action
                int a = g_aiNodeAct[k];
                if (a >= 0 && a < (int)(sizeof ACT_SLOT / sizeof ACT_SLOT[0])) ok = vpad_slot_value(ACT_SLOT[a], &v);
                else { v = 0; ok = 1; }   // action sans équivalent : rien d'appuyé
            }
            if (ok) {
                if (v != 0) {
                    InterlockedIncrement(&g_rebindHits);
                    if (InterlockedIncrement(&g_rebindLog) <= 60)
                        logf_("[rebranchement] noeud %p (source %08X, index %u, action %d) -> manette IA = %.2f", node, w[2], w[3], g_aiNodeAct[k], v);
                }
                return v;
            }
        }
    }
    return o_keyval(node, edx);
}
static void walk_node(uint32_t n, int depth, char* b, int* o, int sz, int collect) {
    if (*o > sz - 120) return;
    if (!n || IsBadReadPtr((void*)(uintptr_t)n, 0x18)) { *o += snprintf(b + *o, sz - *o, "nul"); return; }
    uint32_t* w = (uint32_t*)(uintptr_t)n; uint32_t vt = REFA(w[0]);
    if (vt == VT_KEYNODE) {
        uint32_t src = w[2], svt = (src && !IsBadReadPtr((void*)(uintptr_t)src, 4)) ? REFA(*(uint32_t*)(uintptr_t)src) : 0;
        *o += snprintf(b + *o, sz - *o, "touche(src %08X vt %08X idx %u%s)", src, svt, w[3], src == g_vpadDev ? " =MANETTE IA" : "");
        if (collect && g_collectAct >= 0 && g_aiNodeCount < 256 && !is_ai_node(n)) { g_aiNodeAct[g_aiNodeCount] = g_collectAct; g_aiNodes[g_aiNodeCount++] = n; }
    } else if (vt == VT_ORNODE && depth < 5) {
        *o += snprintf(b + *o, sz - *o, "OU("); walk_node(w[2], depth + 1, b, o, sz, collect);
        *o += snprintf(b + *o, sz - *o, ", "); walk_node(w[3], depth + 1, b, o, sz, collect);
        *o += snprintf(b + *o, sz - *o, ")");
    } else if (vt == VT_THRNODE && depth < 5) {
        *o += snprintf(b + *o, sz - *o, "seuil("); walk_node(w[2], depth + 1, b, o, sz, collect);
        *o += snprintf(b + *o, sz - *o, ")");
    } else {
        *o += snprintf(b + *o, sz - *o, "?(vt %08X : %08X %08X %08X %08X)", vt, w[1], w[2], w[3], w[4]);
    }
}
// relève (collect=1) et/ou décrit les nœuds d'un traducteur
static void mapper_scan(uint32_t mapper, const char* label, int collect) {
    if (!mapper || IsBadReadPtr((void*)(uintptr_t)mapper, 0x10)) { logf_("[rebranchement] traducteur %s invalide", label); return; }
    uint32_t* beg = *(uint32_t**)(uintptr_t)(mapper + 8); uint32_t* end = *(uint32_t**)(uintptr_t)(mapper + 0xC);
    if (!beg || end < beg || end - beg > 1000 || IsBadReadPtr(beg, (end - beg) * 4)) { logf_("[rebranchement] traducteur %s : tableau invalide", label); return; }
    char b[700]; int o;
    if (collect) {
        g_aiNodeCount = 0;
        for (uint32_t* q = beg; q < end; q++) {
            int ai = (int)(q - beg); g_collectAct = -1;
            for (int k = 0; ACT_NAMES[k]; k++) if (g_actIdx[k] == ai) g_collectAct = k;
            o = 0; walk_node(*q, 0, b, &o, sizeof b, 1);
        }
        g_collectAct = -1;
    }
    logf_("[rebranchement] traducteur %s %08X (vt %08X) : %d actions, manette IA %08X%s",
          label, mapper, REFA(*(uint32_t*)(uintptr_t)mapper), (int)(end - beg), g_vpadDev, collect ? "" : "");
    for (int k = 0; ACT_NAMES[k]; k++) {
        int idx = g_actIdx[k];
        if (idx < 0 || beg + idx >= end) continue;
        o = 0; walk_node(beg[idx], 0, b, &o, sizeof b, 0);
        logf_("[rebranchement]   %s %s : %s", label, ACT_NAMES[k], b);
    }
    if (collect) logf_("[rebranchement] %d noeuds 'touche' releves dans le traducteur IA", g_aiNodeCount);
}
static void rebind_pump(void) {
    if (!g_turnIsCPU || !g_turnObj) {
        g_vpadDev = 0; g_vpadObj = 0;
        if (g_aiNodesMapper) {
            logf_("[rebranchement] fin du tour IA : %ld lectures redirigees vers la manette IA", g_rebindHits);
            g_aiNodeCount = 0; g_aiNodesMapper = 0; g_aiNodesDev = 0; g_rebindHits = 0;
            g_rebindLog = 0; g_vpadLog = 0; g_evtLogAI = 0;   // plafonds de log par tour IA
            for (int i = 0; i < 16; i++) g_vpadLast[i] = -1;   // tout appui du tour suivant est logué
        }
        return;
    }
    static DWORD last = 0; DWORD now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    act_init();
    uint8_t* pt = (uint8_t*)VA(0xE4C3AF0);
    uint32_t *ai = NULL, *hu = NULL;
    for (int i = 0; i < 12; i++) {
        uint32_t* w = (uint32_t*)(pt + i * 0x50);
        if ((w[0] & 3) != 3 || (int)w[5] < 0) continue;
        if (w[1] == 0xFFFFFFFFu && w[9] == 0) ai = w;
        if (w[1] != 0xFFFFFFFFu) hu = w;
    }
    if (!ai || !ai[10]) return;
    if (ai[10] != g_aiNodesMapper || g_vpadDev != g_aiNodesDev) {
        g_aiNodesMapper = ai[10]; g_aiNodesDev = g_vpadDev;
        logf_("[rebranchement] tour IA : entree IA drapeaux %08X identite %u equipe %d, en ligne=%d", ai[0], ai[4], (int)ai[5], g_inGameOnline);
        mapper_scan(ai[10], "IA", 1);
        if (hu && hu[10] && hu[10] != ai[10]) mapper_scan(hu[10], "humain", 0);
    }
}
static void diag_mappers_pump(void) {
    rebind_pump();
    if (!g_cfg.verbose || !g_turnIsCPU || !g_turnObj) return;
    { int st = *(int*)(uintptr_t)(g_turnObj + 0x48); if (st < 7 || st > 13) return; }
    static DWORD last = 0; DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;
    act_init();
    uint8_t* pt = (uint8_t*)VA(0xE4C3AF0);
    static char prev[2][200];
    for (int who = 0; who < 2; who++) {   // 0 = IA, 1 = humain local
        uint32_t* e = NULL;
        for (int i = 0; i < 12; i++) {
            uint32_t* w = (uint32_t*)(pt + i * 0x50);
            if ((w[0] & 3) != 3 || (int)w[5] < 0) continue;
            if (who == 0 && w[1] == 0xFFFFFFFFu && w[9] == 0) e = w;
            if (who == 1 && w[1] != 0xFFFFFFFFu) e = w;
        }
        if (!e) continue;
        char b[200]; int o = 0;
        for (int k = 0; k < 7; k++) o += snprintf(b + o, sizeof b - o, " %.1f", mapper_value(e[10], g_actIdx[k]));
        if (strcmp(b, prev[who])) {
            strcpy(prev[who], b);
            logf_("[diag] traducteur %s (%08X, drapeaux %08X) G D H B Saut Saut2 Tir :%s", who ? "humain" : "IA", e[10], e[0], b);
        }
    }
}
static void install_ai_diag(void) {
    static const uint8_t d1[] = {0x55,0x8b,0xec,0x56,0xff,0x75,0x08,0x8b,0xf1};
    static const uint8_t d2[] = {0x55,0x8b,0xec,0x6a,0xff};
    if (!memcmp((void*)VA(0xDB10F0), d1, 9)) o_vpad = (PFN_VPAD)make_hook(VA(0xDB10F0), h_vpad, 9);
    else logf_("diag : signature 0xDB10F0 inattendue");
    if (!memcmp((void*)VA(0xB4DB60), d2, 5)) o_wormevt = (PFN_WORMEVT)make_hook(VA(0xB4DB60), h_wormevt, 5);
    else logf_("diag : signature 0xB4DB60 inattendue");
    static const uint8_t d4[] = {0x55,0x8b,0xec,0xff,0x01,0x8b,0x11};
    if (!memcmp((void*)VA(0xDCBD80), d4, 7)) o_aicmd = (PFN_AICMD)make_hook(VA(0xDCBD80), h_aicmd, 5);
    else logf_("diag : signature 0xDCBD80 inattendue");
    static const uint8_t d3[] = {0xff,0x71,0x0c,0x8b,0x49,0x08,0xe8};
    if (!memcmp((void*)VA(0xD53DA0), d3, 7)) o_keyval = (PFN_KEYVAL)make_hook(VA(0xD53DA0), h_keyval, 6);
    else logf_("diag : signature 0xD53DA0 inattendue");
    logf_("diagnostic des entrees IA installe");
}

static void pump(void) {
    if (!g_mainTid || GetCurrentThreadId() != g_mainTid) return;
    watch_options();
    uint8_t* n = ns();
    static DWORD lastPlayers = 0;
    if (GetTickCount() - lastPlayers >= 200) { lastPlayers = GetTickCount(); players_watch(n); }
    turn_watch();
    ai_input_pump();
    diag_mappers_pump();
    // à chaque début d'action (état 8) : équipe du tour et ver actif (index +0x30 du gestionnaire)
    if (g_turnObj) {
        static int lastSt = -1;
        int st = *(int*)(uintptr_t)(g_turnObj + 0x48);
        if (st != lastSt && st == 8) {
            g_evtLogHuman = 0; g_evtLogAI = 0; g_aiCmdLog = 0;   // plafonds de log par tour
            uint8_t* wm = *(uint8_t**)VA(0xE4C5B3C); uint8_t* tm = *(uint8_t**)VA(0xE4C506C);
            int team = tm ? ((PFN_CURTEAM)VA(0xB3E5B0))(tm, 0) : -1;
            int idx = (wm && !IsBadReadPtr(wm, 0x40)) ? *(int*)(wm + 0x30) : -1;
            void* worm = (idx >= 0 && idx < 0x30) ? *(void**)(wm + 0x3C + idx * 4) : NULL;
            logf_("[ver] debut d'action : equipe %d, ver actif index %d (%p), IA=%d", team, idx, worm, g_turnIsCPU);
        }
        lastSt = st;
    }
    // ForcerBoutonsIA : pendant le tour IA en ligne, active le drapeau « boutons IA » (+0x814 bit 0) du ver
    // actif, pour que la mise à jour du ver (0xB4CA60) applique les boutons virtuels de l'IA (+0x816).
    // Ver actif = [0xE4C5B3C] : index +0x30, tableau +0x3C.
    if (g_cfg.forcePadAI && g_inGameOnline && (g_isHostCached || g_cfg.aiShared) && g_turnIsCPU && g_turnObj) {
        int st = *(int*)(uintptr_t)(g_turnObj + 0x48);
        uint8_t* wm = *(uint8_t**)VA(0xE4C5B3C);
        if ((st == 7 || st == 8 || st == 10) && wm && !IsBadReadPtr(wm, 0x100)) {
            int idx = *(int*)(wm + 0x30);
            uint8_t* worm = (idx >= 0 && idx < 0x30) ? *(uint8_t**)(wm + 0x3C + idx * 4) : NULL;
            if (worm && !IsBadReadPtr(worm, 0x820) && !(worm[0x814] & 1)) {
                worm[0x814] |= 1;
                static DWORD lastLog = 0;
                if (GetTickCount() - lastLog > 3000) { lastLog = GetTickCount(); logf_("[ver IA] %p : drapeau boutons IA force (etat %d)", worm, st); }
            }
        }
    }
    static DWORD lastNames = 0;
    if (n && GetTickCount() - lastNames >= 300) {
        lastNames = GetTickCount();
        if (f_online()) { if (f_ishost()) pending_cpu_teams(n); refresh_cpu_names(n); }
    }
    // o_rebuild (trampoline) ne passe pas par h_rebuild : on réinjecte les IA nous-mêmes
    if (g_reqRebuild) { InterlockedExchange(&g_reqRebuild, 0); if (n) { o_rebuild(n, 0); inject_cpus(n); ui_pulse(); dump_lists("apres reception IA"); } }
    ui_pulse_pump(n);
    if (g_reqKickLo) {
        uint32_t lo = (uint32_t)InterlockedExchange(&g_reqKickLo, 0);
        if (n && f_online() && f_ishost()) { remove_cpu(n, lo); ui_pulse(); }
    }
    if (g_reqUpLo) {
        uint32_t lo = (uint32_t)InterlockedExchange(&g_reqUpLo, 0);
        if (n && f_online() && f_ishost()) { step_cpu_level(n, lo, +1); refresh_cpu_names(n); ui_pulse(); }
    }
    if (g_reqDownLo) {
        uint32_t lo = (uint32_t)InterlockedExchange(&g_reqDownLo, 0);
        if (n && f_online() && f_ishost()) { step_cpu_level(n, lo, -1); refresh_cpu_names(n); ui_pulse(); }
    }
    if (g_reqAddCpu) {
        InterlockedExchange(&g_reqAddCpu, 0);
        if (!n) { logf_("[ajout IA] pas de lobby"); return; }
        int on = f_online(), ho = f_ishost();
        if (on && !ho) { logf_("[ajout IA] seul l'hote peut ajouter des IA"); return; }
        g_cpuCounter++;
        g_forceRand = (int)(MAGIC_LO | (uint32_t)g_cpuCounter);
        int slot = f_addcpu(n, 0);
        g_forceRand = -1;
        logf_("[ajout IA] AddCpuSlot -> %d (id %08X, en ligne=%d)", slot, MAGIC_LO | g_cpuCounter, on);
        if (slot >= 0 && slot < 6 && on) {
            uint32_t lo = MAGIC_LO | g_cpuCounter;
            uint8_t* le = n + NS_LOCAL + slot * 0x18;   // nom de ligne fixé avant le 1er affichage
            if (*(uint32_t*)(le + E_ID) == lo) { g_labelNs = n; char rn[16]; cpu_rowname(rn, sizeof rn, lo); f_fmt(le + E_NAME, "%s", rn); }
            g_pendTeam[g_cpuCounter % 8] = lo;
            pending_cpu_teams(n);
            ui_pulse();
        }
        dump_lists("ajout IA");
    }
}

typedef DWORD (WINAPI *PFN_XIGS)(DWORD, void*);
static PFN_XIGS o_xigs;
static DWORD WINAPI h_xigs(DWORD i, void* s) { if (i == 0) pump(); return o_xigs(i, s); }
typedef DWORD (WINAPI *PFN_TGT)(void);
static PFN_TGT o_tgt;
static DWORD WINAPI h_tgt(void) { pump(); return o_tgt(); }

// pump() est appelée à chaque fois (elle sort tout de suite hors du thread logique) pour
// que la surveillance des options tourne aussi dans le lobby local
typedef BOOL (WINAPI *PFN_QPC)(LARGE_INTEGER*);
static PFN_QPC o_qpc;
static BOOL WINAPI h_qpc(LARGE_INTEGER* p) { pump(); return o_qpc(p); }

typedef struct { uint32_t addr; uint8_t sig[8]; int n; } sig_t;
static const sig_t SIGS[] = {
    { A_ADDCPUSLOT, {0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_REBUILD,    {0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_FINDTRACK,  {0x55,0x8b,0xec,0x56,0x8b,0x75,0x08}, 7 },
    { A_CREATETRACK,{0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_DELETETRACK,{0x55,0x8b,0xec,0x8b,0x55,0x0c,0x53}, 7 },
    { A_ONLINE,     {0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_ISHOST,     {0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_ISREADY,    {0x55,0x8b,0xec,0x6a,0xff,0x68}, 6 },
    { A_STRFMT,     {0x55,0x8b,0xec,0x81,0xec,0x04,0x04}, 7 },
    { A_OPTSELECT,  {0x55,0x8b,0xec,0x56,0x57,0x8b,0xf9}, 7 },
};
static int sigs_ok(void) {
    for (unsigned i = 0; i < sizeof SIGS / sizeof SIGS[0]; i++)
        if (memcmp((void*)VA(SIGS[i].addr), SIGS[i].sig, SIGS[i].n)) return 0;
    return 1;
}

static DWORD WINAPI worker(LPVOID _) {
    for (int t = 0; t < 600 && !sigs_ok(); t++) Sleep(100);
    if (!sigs_ok()) { logf_("ERREUR : version du jeu inattendue, mod desactive."); return 0; }
    f_addcpu      = (PFN_THIS0)VA(A_ADDCPUSLOT);
    f_createtrack = (PFN_THIS2)VA(A_CREATETRACK);
    f_deletetrack = (PFN_THIS2)VA(A_DELETETRACK);
    f_select      = (PFN_SELECT)VA(A_OPTSELECT);
    patch_ailog();
    install_spies();
    install_turn_trace();
    install_ai_input();
    install_ai_diag();
    install_banner();
    f_online      = (PFN_BOOL)VA(A_ONLINE);
    f_ishost      = (PFN_BOOL)VA(A_ISHOST);
    f_fmt         = (PFN_FMT)VA(A_STRFMT);
    o_rebuild     = (PFN_THIS0)make_hook(VA(A_REBUILD), h_rebuild, 5);
    o_findtrack   = (PFN_THIS2)make_hook(VA(A_FINDTRACK), h_findtrack, 7);
    o_ready       = (PFN_READY)make_hook(VA(A_ISREADY), h_ready, 5);
    uintptr_t s1 = find_iat("XINPUT9_1_0.dll", "XInputGetState");
    uintptr_t s2 = find_iat("WINMM.dll", "timeGetTime");
    uintptr_t s3 = find_iat("MSVCR110.dll", "rand");
    uintptr_t s4 = find_iat("KERNEL32.dll", "QueryPerformanceCounter");
    if (s4) patch_iat_slot(s4, h_qpc, (void**)&o_qpc);
    if (s1) patch_iat_slot(s1, h_xigs, (void**)&o_xigs);
    if (s2) patch_iat_slot(s2, h_tgt, (void**)&o_tgt);
    if (s3) patch_iat_slot(s3, h_rand, (void**)&o_rand);
    logf_("WMD-Mod v74 actif (base=%08X, xinput=%d tgt=%d rand=%d qpc=%d). wmdmod.ini : detail log=%d, HostSteamID=%I64u, ForcerBoutonsIA=%d, IAPartagee=%d, TraducteurIA=%d",
          (unsigned)g_base, !!s1, !!s2, !!s3, !!s4, g_cfg.verbose, g_cfg.hostSteamId, g_cfg.forcePadAI, g_cfg.aiShared, g_cfg.swapMapper);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logcs);
        g_base = (uintptr_t)GetModuleHandleA(NULL);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char* sl = strrchr(g_dir, '\\'); if (sl) sl[1] = 0;
        load_ini();
        load_real();
        AddVectoredExceptionHandler(1, veh);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
