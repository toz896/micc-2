// gui_win32.cpp - Interfaz de MilsToz Mic (solo Windows, GDI puro, sin dependencias externas)
//
// Notas de rendimiento:
//  * No hay medidores ni animaciones. En Equalizer APO el editor es una instancia APARTE que no
//    procesa audio; la ventana solo edita ajustes. Se dibuja unicamente cuando algo cambia.
//  * Un timer de 150 ms solo compara un contador (casi 0 CPU).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <algorithm>
#include "gui.h"
#include "dsp.h"
#include "presets.h"

namespace {

HINSTANCE g_hInst = nullptr;
volatile LONG g_classCounter = 0;

const COLORREF C_BG    = RGB(22, 24, 29);
const COLORREF C_PANEL = RGB(31, 34, 41);
const COLORREF C_GRID  = RGB(52, 57, 67);
const COLORREF C_TEXT  = RGB(214, 219, 229);
const COLORREF C_DIM   = RGB(130, 138, 152);
const COLORREF C_ACC   = RGB(161, 34, 88);
const COLORREF C_PREV  = RGB(120, 210, 230);   // curva de previsualizacion

COLORREF blend(COLORREF a, COLORREF b, double t) {   // a*(1-t) + b*t  (para simular transparencia)
    auto m = [&](int x, int y) { return (int)std::lround(x * (1.0 - t) + y * t); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

// ---------------------------------------------------------------- definiciones de la interfaz
struct NodeDef { int pf, pg, pq; const wchar_t* label; COLORREF col; const wchar_t* title; const wchar_t* tip; };
const NodeDef kNodes[6] = {
    {SP_HPF,  -1,      -1,      L"HP", RGB(165, 170, 182), L"Filtro de graves (HP)",
        L"Corta golpes de escritorio, vibraciones y soplidos. Arrastra o usa la rueda para la frecuencia; botones Off / Smooth / Cut para la pendiente."},
    {SP_B1_F, SP_B1_G, -1,      L"1",  RGB(255, 120, 100), L"Cuerpo",
        L"Cuerpo: demasiado sonará retumbante; muy poco dejará la voz delgada."},
    {SP_B2_F, SP_B2_G, SP_B2_Q, L"2",  RGB(255, 190, 80),  L"Caja / Hueco",
        L"Demasiado suena encajonado, como hablar dentro de una caja; recortarlo limpia el efecto de cuarto."},
    {SP_B3_F, SP_B3_G, SP_B3_Q, L"3",  RGB(130, 220, 120), L"Nasal / Medios",
        L"Demasiado suena nasal o de teléfono; recortarlo suaviza la voz, pero si te pasas queda apagada."},
    {SP_B4_F, SP_B4_G, SP_B4_Q, L"4",  RGB(90, 190, 255),  L"Claridad / Presencia",
        L"Ayuda a que la voz corte a través de música o juego. Demasiado cansa y raspa; muy poco suena tapada."},
    {SP_B5_F, SP_B5_G, -1,      L"5",  RGB(190, 140, 255), L"Aire / Brillo",
        L"Da apertura y brillo. Demasiado produce siseo y estridencia; muy poco deja la voz sin vida."},
};

struct ZoneDef { double lo, hi; const wchar_t* name; const wchar_t* range; const wchar_t* shortName; const wchar_t* desc; COLORREF col; };
const ZoneDef kZones[5] = {
    {20.0,    80.0,    L"SUB-GRAVES",   L"< 80 Hz",        L"Sub-graves",
        L"Sub-graves / Retumbe: golpes y vibraciones del escritorio. Conviene cortarlos con el filtro HP.", RGB(210, 90, 90)},
    {80.0,    300.0,   L"CUERPO",       L"80 - 300 Hz",    L"Cuerpo",
        L"Cuerpo / Calidez: el peso y la calidez de un locutor. Demasiado = retumbante.", RGB(235, 150, 60)},
    {300.0,   1400.0,  L"CAJA / HUECO", L"300 - 1.4 kHz",  L"Caja",
        L"Caja / Hueco: el sonido encajonado o de cuarto vacío. Recortar aquí limpia la voz.", RGB(205, 195, 70)},
    {1400.0,  5000.0,  L"CLARIDAD",     L"1.4 - 5 kHz",    L"Claridad",
        L"Claridad / Presencia: la zona que corta a través de música o juego.", RGB(80, 175, 115)},
    {5000.0,  20000.0, L"AIRE / BRILLO",L"> 5 kHz",        L"Aire",
        L"Aire / Brillo: apertura y brillo. Demasiado = siseo y estridencia.", RGB(80, 145, 215)},
};

struct KnobDef { int sp; const wchar_t* name; int cx; int r; const wchar_t* hint; };
const int NUM_KNOBS = 7;
// Orden = orden de la cadena de audio (izquierda -> derecha)
const KnobDef kKnobs[NUM_KNOBS] = {
    {SP_GATE,   L"NOISE GATE",  66,  30, L"Noise Gate: silencia el ruido de fondo cuando no hablas. En 0% está apagado y no toca tu voz."},
    {SP_BODY,   L"CUERPO",      160, 30, L"Cuerpo: peso y calidez de locutor (120-220 Hz) con un toque de armónicos de válvula."},
    {SP_COMP,   L"COMPRESSION", 280, 30, L"Compression: nivela la voz, baja los picos y sube lo suave. Elige el carácter con Soft, Medium o Hard."},
    {SP_DEESS,  L"DE-ESSER",    400, 30, L"De-esser: suaviza las 'sss' molestas (6-8 kHz) sin apagar el resto de la voz."},
    {SP_BRILLO, L"BRILLO",      490, 30, L"Brillo: quita asperezas metálicas del micrófono (3.5-4.8 kHz) y abre el aire de la voz (> 10 kHz)."},
    {SP_MIX,    L"MIX",         572, 21, L"Mix: mezcla la voz original (0%) con la procesada (100%)."},
    {SP_OUT,    L"SALIDA",      650, 30, L"Salida: volumen final. Un limitador a -0.5 dBFS evita la distorsión digital."},
};
const wchar_t* const kModeName[3] = {L"Soft", L"Medium", L"Hard"};
const wchar_t* const kModeSub[3]  = {L"Opto / LA-2A: suave y musical", L"VCA: equilibrado, ~4:1", L"FET / 1176: rápido y firme, 10:1"};
const wchar_t* const kModeHint[3] = {
    L"Soft (estilo Opto / LA-2A): ataque lento y recuperación musical. Ideal para conversación relajada.",
    L"Medium (estilo VCA): ataque de 10 ms y ratio ~4:1. Para locución y streams estándar.",
    L"Hard (estilo FET / 1176): ataque ultrarrápido (< 1 ms) y ratio hasta 10:1. Domina picos y gritos sin saturar."};
const wchar_t* const kHpName[3] = {L"Off", L"Smooth", L"Cut"};
const wchar_t* const kHpSub[3]  = {L"HP apagado", L"HP Smooth: 12 dB/oct", L"HP Cut: 24 dB/oct"};
const wchar_t* const kHpHint[3] = {
    L"HP apagado: no se corta nada por debajo de la voz.",
    L"HP Smooth (12 dB/oct): corte suave y natural, conserva el cuerpo de la voz.",
    L"HP Cut (24 dB/oct): corte firme; elimina retumbe, golpes y soplidos con más fuerza."};

// Botones (rectángulos fijos)
enum { B_PREV = 0, B_NEXT, B_NAME, B_SAVE, B_LOAD, B_MODE0, B_MODE1, B_MODE2, B_HP0, B_HP1, B_HP2, B_COUNT };
struct Rc { int l, t, r, b; };
const Rc kBtn[B_COUNT] = {
    {176, 10, 204, 40}, {494, 10, 522, 40}, {208, 10, 490, 40}, {530, 10, 602, 40}, {608, 10, 680, 40},
    {177, 438, 243, 460}, {247, 438, 313, 460}, {317, 438, 383, 460},     // Soft / Medium / Hard (66 px: "Medium" ya no se corta)
    {16, 438, 60, 460},   {64, 438, 124, 460},  {128, 438, 168, 460},     // HP: Off / Smooth / Cut
};
const wchar_t* const kBtnTip[B_COUNT] = {
    L"Preset anterior", L"Preset siguiente", L"Elegir un preset de la lista",
    L"Guarda tu configuración actual en un archivo .json", L"Carga un preset desde un archivo .json", L"", L"", L"", L"", L"", L""};

// Geometria
const int G_L = 20, G_T = 62, G_R = 680, G_B = 282;
const int G_W = G_R - G_L, G_H = G_B - G_T;
const int KNOB_CY = 356, LABEL_Y = 396, VALUE_Y = 414;
const int DD_L = 176, DD_T = 42, DD_R = 560, DD_ITEM = 26, DD_B = DD_T + 8 + NUM_PRESETS * DD_ITEM;
const double kDbRange = 15.0;   // la grafica muestra +-15 dB

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct Gui {
    SharedState* st = nullptr;
    HWND hwnd = nullptr;
    wchar_t cls[64] = {0};
    HFONT fSmall = nullptr, fNorm = nullptr, fNormB = nullptr, fBold = nullptr;
    uint32_t lastVersion = 0;
    int dragKind = 0;            // 0 nada, 1 nodo, 2 perilla
    int dragIdx = -1, hover = -1, hoverKnob = -1, hoverZone = -1, hoverBtn = -1;
    int dragStartY = 0, dragDx = 0, dragDy = 0;   // dragDx/Dy: desfase entre el raton y el centro del nodo al agarrarlo (sin saltos)
    float dragStartNorm = 0.f;
    bool tracking = false;

    bool dropOpen = false; int dropHover = -1;
    wchar_t customName[64] = L"Personalizado";
    wchar_t status[200] = {0}; ULONGLONG statusUntil = 0;

    // ---------- acceso a parametros ----------
    float val(int sp) const { return st->get(sp); }
    void setVal(int sp, float v) { st->edit(sp, v); }       // marca el preset como "modificado"
    void invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void setStatus(const wchar_t* s) { _snwprintf(status, 199, L"%s", s); status[199] = 0; statusUntil = GetTickCount64() + 5000; }

    // ---------- conversiones ----------
    static int freqToX(double f) {
        f = std::min(std::max(f, 20.0), 20000.0);
        return G_L + (int)std::lround(std::log(f / 20.0) / std::log(1000.0) * G_W);
    }
    static double xToFreq(int x) {
        x = std::min(std::max(x, G_L), G_R);
        return 20.0 * std::pow(1000.0, (double)(x - G_L) / G_W);
    }
    static int gainToY(double g) {
        g = std::min(std::max(g, -kDbRange), kDbRange);
        return G_T + G_H / 2 - (int)std::lround(g / kDbRange * (G_H / 2));
    }
    static double yToGain(int y) { return (double)(G_T + G_H / 2 - y) / (G_H / 2) * kDbRange; }

    // Los nodos de banda se dibujan EXACTAMENTE sobre la curva total del EQ (x = su frecuencia, y = respuesta
    // de la curva a esa frecuencia). El nodo HP queda en una guia fija (y fija).
    void nodePos(int n, int& x, int& y) const {
        const NodeDef& d = kNodes[n];
        x = freqToX(val(d.pf));
        if (d.pg < 0) { y = G_B - 34; return; }
        float p[SP_COUNT]; st->snapshot(p);
        y = gainToY(dsp::VoiceEngine::eqResponseDb(p, p[d.pf]));
    }
    // Ganancia de la banda n para que la curva total pase por 'targetDb' a la frecuencia actual de la banda
    // (biseccion: la respuesta es monotona con la ganancia). Asi el nodo sigue al raton sin saltos.
    static double solveGain(int n, const float* base, double targetDb) {
        const NodeDef& d = kNodes[n];
        float q[SP_COUNT]; std::copy(base, base + SP_COUNT, q);
        double lo = kInfo[d.pg].minV, hi = kInfo[d.pg].maxV;
        for (int i = 0; i < 22; ++i) {
            const double mid = 0.5 * (lo + hi);
            q[d.pg] = (float)mid;
            if (dsp::VoiceEngine::eqResponseDb(q, q[d.pf]) < targetDb) lo = mid; else hi = mid;
        }
        return 0.5 * (lo + hi);
    }
    float knobNorm(int k) const {
        const int sp = kKnobs[k].sp;
        return paramToNorm(kInfo[sp], val(sp));
    }
    void applyKnobNorm(int k, float n) {
        n = std::min(std::max(n, 0.f), 1.f);
        const int sp = kKnobs[k].sp;
        float real;
        if (sp == SP_OUT) { real = -12.f + 24.f * n; if (std::fabs(real) < 0.4f) real = 0.f; }
        else { real = n; if (real < 0.02f) real = 0.f; if (sp == SP_MIX && real > 0.98f) real = 1.f; }
        setVal(sp, real);
    }
    int factoryRef() const { const int p = st->preset.load(); return (p >= 0 && p < NUM_PRESETS) ? p : 0; }

    // ---------- presets ----------
    void applyPreset(int i) {
        if (i < 0 || i >= NUM_PRESETS) return;
        st->loadFactory(i);
        status[0] = 0; statusUntil = 0;
        invalidate();
    }
    void stepPreset(int dir) {
        const int cur = st->preset.load();
        const int next = cur < 0 ? (dir > 0 ? 0 : NUM_PRESETS - 1) : (cur + dir + NUM_PRESETS) % NUM_PRESETS;
        applyPreset(next);
    }
    static bool pickFile(HWND owner, bool save, wchar_t* path /*[MAX_PATH]*/) {
        OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Preset MilsToz (*.json)\0*.json\0Todos los archivos\0*.*\0";
        ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = L"json";
        ofn.lpstrTitle = save ? L"Guardar preset" : L"Cargar preset";
        ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) != 0;
    }
    static std::wstring stem(const wchar_t* path) {
        std::wstring s(path); const size_t a = s.find_last_of(L"\\/"); if (a != std::wstring::npos) s = s.substr(a + 1);
        const size_t d = s.find_last_of(L'.'); if (d != std::wstring::npos) s = s.substr(0, d);
        return s;
    }
    void savePreset() {
        wchar_t path[MAX_PATH] = {0};
        _snwprintf(path, MAX_PATH - 1, L"Mi preset.json");
        dragKind = 0; dropOpen = false;
        if (GetCapture() == hwnd) ReleaseCapture();      // el dialogo es modal: soltar el raton antes
        if (!pickFile(hwnd, true, path)) { invalidate(); return; }
        float v[SP_COUNT]; st->snapshot(v);
        const std::wstring nm = stem(path);
        const std::string js = presets::toJson(v, wideToUtf8(nm).c_str());
        FILE* f = _wfopen(path, L"wb");
        if (!f) { setStatus(L"No se pudo escribir el archivo."); invalidate(); return; }
        const size_t w = fwrite(js.data(), 1, js.size(), f); fclose(f);
        if (w != js.size()) setStatus(L"Error al escribir el archivo.");
        else { wchar_t b[200]; _snwprintf(b, 199, L"Preset guardado: %s", nm.c_str()); b[199] = 0; setStatus(b); }
        invalidate();
    }
    void loadPreset() {
        wchar_t path[MAX_PATH] = {0};
        dragKind = 0; dropOpen = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!pickFile(hwnd, false, path)) { invalidate(); return; }
        FILE* f = _wfopen(path, L"rb");
        if (!f) { setStatus(L"No se pudo abrir el archivo."); invalidate(); return; }
        std::string txt; char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && txt.size() < 65536) txt.append(buf, n);
        fclose(f);
        float v[SP_COUNT]; st->snapshot(v);
        std::string name;
        if (!presets::fromJson(txt, v, &name)) { setStatus(L"El archivo no es un preset válido de MilsToz Mic."); invalidate(); return; }
        st->loadValues(v, -1);
        const std::wstring nm = name.empty() ? stem(path) : utf8ToWide(name);
        _snwprintf(customName, 63, L"%s", nm.c_str()); customName[63] = 0;
        wchar_t b[200]; _snwprintf(b, 199, L"Preset cargado: %s", nm.c_str()); b[199] = 0; setStatus(b);
        invalidate();
    }

    // ---------- hit tests ----------
    static bool inRc(const Rc& r, int x, int y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }
    int hitBtn(int x, int y) const {
        for (int i = 0; i < B_COUNT; ++i) if (inRc(kBtn[i], x, y)) return i;
        return -1;
    }
    int hitDropItem(int x, int y) const {
        if (x < DD_L || x >= DD_R || y < DD_T + 4 || y >= DD_B - 4) return -1;
        const int i = (y - DD_T - 4) / DD_ITEM;
        return (i >= 0 && i < NUM_PRESETS) ? i : -1;
    }
    bool inDrop(int x, int y) const { return x >= DD_L && x < DD_R && y >= DD_T && y < DD_B; }
    int hitNode(int mx, int my) const {
        int best = -1; double bd = 13.0 * 13.0;
        for (int n = 0; n < 6; ++n) {
            int x, y; nodePos(n, x, y);
            const double d = (double)(mx - x) * (mx - x) + (double)(my - y) * (my - y);
            if (d <= bd) { bd = d; best = n; }
        }
        return best;
    }
    int hitKnob(int mx, int my) const {
        for (int k = 0; k < NUM_KNOBS; ++k) {
            const int dx = mx - kKnobs[k].cx, dy = my - KNOB_CY, r = kKnobs[k].r + 8;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }
    int hitZone(int x, int y) const {
        if (x < G_L || x > G_R || y < G_T || y > G_B) return -1;
        const double f = xToFreq(x);
        for (int i = 0; i < 5; ++i) if (f < kZones[i].hi) return i;
        return 4;
    }

    // ---------- eventos ----------
    void resetNode(int n) {
        const NodeDef& d = kNodes[n]; const int s = factoryRef();
        setVal(d.pf, kPresets[s].v[d.pf]);
        if (d.pg >= 0) setVal(d.pg, kPresets[s].v[d.pg]);
        if (d.pq >= 0) setVal(d.pq, kPresets[s].v[d.pq]);
        if (n == 0) setVal(SP_HPF_MODE, kPresets[s].v[SP_HPF_MODE]);
    }
    void onDown(int x, int y, bool dbl) {
        if (dropOpen) {                                   // lista abierta: elegir o cerrar
            const int it = hitDropItem(x, y);
            dropOpen = false; dropHover = -1;
            if (it >= 0) applyPreset(it);
            invalidate(); return;
        }
        const int b = hitBtn(x, y);
        if (b >= 0) {
            if (b >= B_HP0) setVal(SP_HPF_MODE, (float)(b - B_HP0));
            else if (b >= B_MODE0) setVal(SP_MODE, (float)(b - B_MODE0));
            else switch (b) {
                case B_PREV: stepPreset(-1); break;
                case B_NEXT: stepPreset(+1); break;
                case B_NAME: dropOpen = true; break;
                case B_SAVE: savePreset(); return;
                case B_LOAD: loadPreset(); return;
                default: break;
            }
            invalidate(); return;
        }
        const int n = hitNode(x, y);
        if (n >= 0) {
            hover = n;
            if (dbl) resetNode(n);
            else { int nx, ny; nodePos(n, nx, ny); dragDx = x - nx; dragDy = y - ny; dragKind = 1; dragIdx = n; }
            invalidate(); return;
        }
        const int k = hitKnob(x, y);
        if (k >= 0) {
            if (dbl) setVal(kKnobs[k].sp, kPresets[factoryRef()].v[kKnobs[k].sp]);
            else { dragKind = 2; dragIdx = k; dragStartY = y; dragStartNorm = knobNorm(k); }
            invalidate();
        }
    }
    void onMove(int x, int y) {
        if (dragKind == 1) {
            const NodeDef& d = kNodes[dragIdx];
            const int tx = x - dragDx, ty = y - dragDy;            // posicion deseada del CENTRO del nodo
            setVal(d.pf, (float)xToFreq(tx));
            if (dragIdx == 0) {
                if (val(SP_HPF_MODE) < 0.5f) setVal(SP_HPF_MODE, (float)HPF_SMOOTH);   // mover el nodo lo activa
            } else if (d.pg >= 0) {
                float p[SP_COUNT]; st->snapshot(p);                  // ya con la nueva frecuencia
                double g = solveGain(dragIdx, p, yToGain(ty));
                if (std::fabs(g) < 0.25) g = 0.0;
                setVal(d.pg, (float)g);
            }
            invalidate();
        } else if (dragKind == 2) {
            applyKnobNorm(dragIdx, dragStartNorm + (float)(dragStartY - y) / 180.f);
            invalidate();
        } else {
            if (dropOpen) {
                const int h = hitDropItem(x, y);
                if (h != dropHover) { dropHover = h; invalidate(); }
                return;
            }
            const int h = hitNode(x, y), hk = hitKnob(x, y), hz = (h >= 0) ? -1 : hitZone(x, y), hb = hitBtn(x, y);
            if (h != hover || hk != hoverKnob || hz != hoverZone || hb != hoverBtn) {
                hover = h; hoverKnob = hk; hoverZone = hz; hoverBtn = hb; invalidate();
            }
        }
    }
    void onLeave() {
        tracking = false;
        if (hover != -1 || hoverKnob != -1 || hoverZone != -1 || hoverBtn != -1 || dropHover != -1) {
            hover = hoverKnob = hoverZone = hoverBtn = dropHover = -1; invalidate();
        }
    }
    void onUp() { dragKind = 0; dragIdx = -1; invalidate(); }
    void onWheel(int x, int y, int delta) {
        if (dropOpen) return;
        const int n = hitNode(x, y);
        if (n == 0) {                                          // nodo HP: la rueda mueve la frecuencia
            if (val(SP_HPF_MODE) < 0.5f) setVal(SP_HPF_MODE, (float)HPF_SMOOTH);
            setVal(SP_HPF, val(SP_HPF) * std::pow(1.08f, (float)delta / 120.f));
            invalidate(); return;
        }
        if (n >= 0 && kNodes[n].pq >= 0) {
            setVal(kNodes[n].pq, val(kNodes[n].pq) * std::pow(1.12f, (float)delta / 120.f));
            invalidate(); return;
        }
        const int k = hitKnob(x, y);
        if (k >= 0) { applyKnobNorm(k, knobNorm(k) + 0.02f * (float)delta / 120.f); invalidate(); }
    }

    // ---------- dibujo ----------
    void createFonts() {
        auto mk = [](int h, int w) { return CreateFontW(h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
        fSmall = mk(-11, FW_NORMAL); fNorm = mk(-13, FW_NORMAL); fNormB = mk(-13, FW_BOLD); fBold = mk(-18, FW_BOLD);
    }
    void destroyFonts() {
        HFONT* fs[4] = {&fSmall, &fNorm, &fNormB, &fBold};
        for (HFONT* f : fs) { if (*f) DeleteObject(*f); *f = nullptr; }
    }

    static void fillRect(HDC dc, int l, int t, int r, int b, COLORREF c) {
        RECT rc = {l, t, r, b};
        HBRUSH br = CreateSolidBrush(c); FillRect(dc, &rc, br); DeleteObject(br);
    }
    static void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w = 1) {
        HPEN pen = CreatePen(PS_SOLID, w, c); HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
        SelectObject(dc, old); DeleteObject(pen);
    }
    static void roundRect(HDC dc, int l, int t, int r, int b, COLORREF fill, COLORREF border, int rad) {
        HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        RoundRect(dc, l, t, r, b, rad, rad);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pen);
    }
    static void circle(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border, int bw = 1) {
        HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, bw, border);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pen);
    }
    static void arc(HDC dc, int cx, int cy, int r, double a0, double a1, COLORREF col, int w) {
        POINT pts[64];
        int n = std::max(2, (int)((a1 - a0) / 5.0) + 1);
        if (n > 64) n = 64;
        for (int i = 0; i < n; ++i) {
            const double a = (a0 + (a1 - a0) * i / (n - 1)) * dsp::kPi / 180.0;
            pts[i].x = cx + (LONG)std::lround(r * std::sin(a));
            pts[i].y = cy - (LONG)std::lround(r * std::cos(a));
        }
        HPEN pen = CreatePen(PS_SOLID, w, col); HGDIOBJ old = SelectObject(dc, pen);
        Polyline(dc, pts, n);
        SelectObject(dc, old); DeleteObject(pen);
    }
    static void text(HDC dc, HFONT f, int x, int y, const wchar_t* s, COLORREF c, UINT align) {
        SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, align | TA_TOP);
        TextOutW(dc, x, y, s, (int)wcslen(s));
    }
    // Texto dentro de un rectangulo (con ajuste de linea y/o puntos suspensivos)
    static void textBox(HDC dc, HFONT f, RECT rc, const wchar_t* s, COLORREF c, UINT flags) {
        SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, s, -1, &rc, flags);
    }
    static int textHeight(HDC dc, HFONT f, int width, const wchar_t* s) {
        SelectObject(dc, f); RECT rc = {0, 0, width, 0};
        DrawTextW(dc, s, -1, &rc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        return rc.bottom - rc.top;
    }

    int curve(const float* p, POINT* pts, int maxPts) const {
        int np = 0;
        for (int x = 0; x <= G_W && np < maxPts; x += 2) {
            const double d = dsp::VoiceEngine::eqResponseDb(p, xToFreq(G_L + x), 48000.0);
            pts[np].x = G_L + x; pts[np].y = gainToY(d); ++np;
        }
        return np;
    }
    void drawCurve(HDC dc, const float* p, COLORREF col, int width, int style) const {
        POINT pts[340]; const int np = curve(p, pts, 340);
        HPEN pen = CreatePen(style, width, col); HGDIOBJ old = SelectObject(dc, pen);
        SetBkMode(dc, TRANSPARENT);
        Polyline(dc, pts, np);
        SelectObject(dc, old); DeleteObject(pen);
    }
    void drawTooltip(HDC dc, int ax, int ay, const wchar_t* title, const wchar_t* detail, const wchar_t* body) {
        const int W = 262, pad = 8;
        const int hb = textHeight(dc, fSmall, W - 2 * pad, body);
        const int H = pad + 16 + 16 + hb + pad;
        int x = ax + 18, y = ay + 14;
        if (x + W > GUI_W - 4) x = ax - 18 - W;
        if (x < 4) x = 4;
        if (y + H > G_B + 6) y = ay - 14 - H;
        if (y < G_T - 4) y = G_T - 4;
        roundRect(dc, x, y, x + W, y + H, RGB(14, 16, 20), C_ACC, 8);
        text(dc, fNormB, x + pad, y + pad - 1, title, C_TEXT, TA_LEFT);
        text(dc, fNorm, x + pad, y + pad + 16, detail, C_ACC, TA_LEFT);
        RECT rc = {x + pad, y + pad + 34, x + W - pad, y + H - pad};
        textBox(dc, fSmall, rc, body, C_DIM, DT_WORDBREAK | DT_NOPREFIX);
    }

    void drawButton(HDC dc, int b, const wchar_t* label, bool active = false) {
        const Rc& r = kBtn[b];
        const bool h = (hoverBtn == b);
        const COLORREF fill = active ? C_ACC : (h ? RGB(44, 48, 58) : C_PANEL);
        roundRect(dc, r.l, r.t, r.r, r.b, fill, active ? C_ACC : (h ? C_DIM : C_GRID), 8);
        RECT rc = {r.l, r.t, r.r, r.b};
        textBox(dc, fNorm, rc, label, active ? RGB(255, 255, 255) : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    }

    void drawAll(HDC dc) {
        fillRect(dc, 0, 0, GUI_W, GUI_H, C_BG);
        float p[SP_COUNT]; st->snapshot(p);
        const int cur = st->preset.load(), dirty = st->dirty.load();

        // --- titulo
        text(dc, fBold, 20, 8, L"MilsToz Mic", C_TEXT, TA_LEFT);
        text(dc, fSmall, 20, 30, L"voz + micrófono económico", C_DIM, TA_LEFT);

        // --- barra de presets: [<] [nombre v] [>] [Guardar] [Cargar]
        drawButton(dc, B_PREV, L"<");
        drawButton(dc, B_NEXT, L">");
        {
            const Rc& r = kBtn[B_NAME];
            roundRect(dc, r.l, r.t, r.r, r.b, hoverBtn == B_NAME ? RGB(44, 48, 58) : C_PANEL, dropOpen ? C_ACC : C_GRID, 8);
            std::wstring nm = (cur >= 0) ? utf8ToWide(presets::nameOf(cur)) : std::wstring(customName);
            RECT rc = {r.l + 10, r.t, r.r - 34, r.b};
            textBox(dc, fNormB, rc, nm.c_str(), C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (dirty) text(dc, fNormB, r.r - 30, r.t + 6, L"*", C_ACC, TA_LEFT);
            text(dc, fSmall, r.r - 12, r.t + 9, L"v", C_DIM, TA_CENTER);
        }
        drawButton(dc, B_SAVE, L"Guardar");
        drawButton(dc, B_LOAD, L"Cargar");

        // --- panel de la grafica + zonas de frecuencia semitransparentes
        roundRect(dc, G_L - 6, G_T - 6, G_R + 6, G_B + 6, C_PANEL, C_GRID, 10);
        for (int z = 0; z < 5; ++z) {
            const int x0 = (z == 0) ? G_L : freqToX(kZones[z].lo), x1 = (z == 4) ? G_R : freqToX(kZones[z].hi);
            const double a = (hoverZone == z && hover < 0 && dragKind == 0) ? 0.24 : 0.12;
            fillRect(dc, x0, G_T, x1, G_B, blend(C_PANEL, kZones[z].col, a));
            if (z > 0) line(dc, x0, G_T, x0, G_B, blend(C_PANEL, kZones[z].col, 0.45));
            const COLORREF tc = blend(kZones[z].col, RGB(255, 255, 255), 0.35);
            text(dc, fSmall, (x0 + x1) / 2, G_T + 3, kZones[z].name, tc, TA_CENTER);
            text(dc, fSmall, (x0 + x1) / 2, G_T + 16, kZones[z].range, blend(C_PANEL, tc, 0.7), TA_CENTER);
        }
        const double fl[8] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
        const wchar_t* fn[8] = {L"50", L"100", L"200", L"500", L"1k", L"2k", L"5k", L"10k"};
        for (int i = 0; i < 8; ++i) {
            const int x = freqToX(fl[i]);
            line(dc, x, G_T + 32, x, G_B, C_GRID);
            text(dc, fSmall, x, G_B - 14, fn[i], C_DIM, TA_CENTER);
        }
        const int dl[5] = {-12, -6, 0, 6, 12};
        const wchar_t* dn[5] = {L"-12", L"-6", L"0", L"+6", L"+12"};
        for (int i = 0; i < 5; ++i) {
            const int y = gainToY(dl[i]);
            if (y > G_T + 32) line(dc, G_L, y, G_R, y, dl[i] == 0 ? C_DIM : C_GRID);
            if (y > G_T + 32) text(dc, fSmall, G_L + 3, y - 13, dn[i], C_DIM, TA_LEFT);
        }

        // --- curvas: la activa (magenta) y, solo mientras se navega la lista, la previsualizacion
        const int pv = (dropOpen && dropHover >= 0) ? dropHover : -1;
        if (pv >= 0) drawCurve(dc, kPresets[pv].v, C_PREV, 2, PS_SOLID);
        drawCurve(dc, p, pv >= 0 ? C_GRID : C_ACC, 2, PS_SOLID);

        // --- nodos
        const int active = (dragKind == 1) ? dragIdx : hover;
        for (int n = 0; n < 6; ++n) {
            int x, y; nodePos(n, x, y);
            const bool a = (n == active);
            const bool hpOff = (n == 0 && val(SP_HPF_MODE) < 0.5f);
            circle(dc, x, y, a ? 11 : 9, hpOff ? blend(kNodes[n].col, C_PANEL, 0.6) : kNodes[n].col, a ? RGB(255, 255, 255) : RGB(20, 22, 26), a ? 2 : 1);
            text(dc, fSmall, x, y - 8, kNodes[n].label, RGB(15, 17, 21), TA_CENTER);
        }

        // --- linea de informacion (explicaciones, resumen de cambios, avisos)
        {
            wchar_t b[260] = {0}; COLORREF col = C_DIM;
            const wchar_t* shown = nullptr;
            if (status[0] && GetTickCount64() < statusUntil) { shown = status; col = C_TEXT; }
            else if (pv >= 0) { _snwprintf(b, 259, L"Previsualizando: %s  (línea celeste).  Haz clic para cargarlo.", utf8ToWide(kPresets[pv].name).c_str()); shown = b; col = C_PREV; }
            else if (hoverKnob >= 0) shown = kKnobs[hoverKnob].hint;
            else if (hoverBtn >= B_MODE0 && hoverBtn <= B_MODE2) shown = kModeHint[hoverBtn - B_MODE0];
            else if (hoverBtn >= B_HP0 && hoverBtn <= B_HP2) shown = kHpHint[hoverBtn - B_HP0];
            else if (hoverBtn >= 0 && hoverBtn < B_MODE0 && kBtnTip[hoverBtn][0]) shown = kBtnTip[hoverBtn];
            else if (hover < 0 && hoverZone >= 0) shown = kZones[hoverZone].desc;
            else shown = L"Arrastra los puntos: izquierda/derecha = frecuencia, arriba/abajo = ganancia.  Rueda = Q (en HP: frecuencia).  Doble clic = restablecer.";
            RECT rc = {G_L, G_B + 12, G_R, G_B + 44};
            textBox(dc, fSmall, rc, shown, col, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        }

        // --- perillas
        for (int k = 0; k < NUM_KNOBS; ++k) {
            const int cx = kKnobs[k].cx, cy = KNOB_CY, R = kKnobs[k].r, sp = kKnobs[k].sp;
            const float n = knobNorm(k), v = val(sp);
            const bool hot = (hoverKnob == k || (dragKind == 2 && dragIdx == k));
            arc(dc, cx, cy, R, -135.0, 135.0, C_GRID, R > 25 ? 5 : 4);
            if (n > 0.001f) arc(dc, cx, cy, R, -135.0, -135.0 + 270.0 * n, C_ACC, R > 25 ? 5 : 4);
            circle(dc, cx, cy, R - (R > 25 ? 9 : 7), C_PANEL, hot ? C_DIM : C_GRID);
            const double a = (-135.0 + 270.0 * n) * dsp::kPi / 180.0;
            const int r0 = R > 25 ? 5 : 3, r1 = R > 25 ? 19 : 12;
            line(dc, cx + (int)std::lround(r0 * std::sin(a)), cy - (int)std::lround(r0 * std::cos(a)),
                     cx + (int)std::lround(r1 * std::sin(a)), cy - (int)std::lround(r1 * std::cos(a)), C_TEXT, 2);
            text(dc, fNorm, cx, LABEL_Y, kKnobs[k].name, C_TEXT, TA_CENTER);
            wchar_t b[32]; bool off = false;
            if (sp == SP_OUT) _snwprintf(b, 31, L"%+.1f dB", v);
            else if (sp != SP_MIX && v < 0.005f) { _snwprintf(b, 31, L"OFF"); off = true; }
            else _snwprintf(b, 31, L"%d%%", (int)(v * 100.f + 0.5f));
            b[31] = 0;
            text(dc, fNorm, cx, VALUE_Y, b, off ? C_DIM : C_ACC, TA_CENTER);
        }

        // --- selector de modo del compresor
        {
            const int mode = std::min(std::max((int)(val(SP_MODE) + 0.5f), 0), 2);
            for (int m = 0; m < 3; ++m) drawButton(dc, B_MODE0 + m, kModeName[m], m == mode);
            text(dc, fSmall, kKnobs[2].cx, 464, kModeSub[mode], C_DIM, TA_CENTER);
        }

        // --- selector del filtro HP: Off / Smooth (12 dB/oct) / Cut (24 dB/oct)
        {
            const int hm = std::min(std::max((int)(val(SP_HPF_MODE) + 0.5f), 0), 2);
            for (int m = 0; m < 3; ++m) drawButton(dc, B_HP0 + m, kHpName[m], m == hm);
            text(dc, fSmall, 92, 464, kHpSub[hm], C_DIM, TA_CENTER);
        }

        // --- lista desplegable de presets (encima de todo)
        if (dropOpen) {
            roundRect(dc, DD_L, DD_T, DD_R, DD_B, RGB(26, 29, 35), C_ACC, 8);
            for (int i = 0; i < NUM_PRESETS; ++i) {
                const int t = DD_T + 4 + i * DD_ITEM;
                if (i == dropHover) fillRect(dc, DD_L + 4, t, DD_R - 4, t + DD_ITEM, blend(C_PANEL, C_ACC, 0.35));
                RECT rc = {DD_L + 14, t, DD_R - 30, t + DD_ITEM};
                textBox(dc, i == cur ? fNormB : fNorm, rc, utf8ToWide(kPresets[i].name).c_str(), C_TEXT,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (i == cur) text(dc, fNormB, DD_R - 18, t + 4, L"•", C_ACC, TA_CENTER);
            }
        }

        // --- tooltip flotante del nodo (Hz, dB y que hace la banda)
        if (active >= 0 && !dropOpen) {
            const NodeDef& d = kNodes[active]; int nx, ny; nodePos(active, nx, ny);
            wchar_t detail[96];
            if (d.pg < 0) _snwprintf(detail, 95, L"%d Hz   %s", (int)(val(d.pf) + 0.5f), kHpName[std::min(std::max((int)(val(SP_HPF_MODE) + 0.5f), 0), 2)]);
            else if (d.pq >= 0) _snwprintf(detail, 95, L"%d Hz   %+.1f dB   Q %.2f", (int)(val(d.pf) + 0.5f), val(d.pg), val(d.pq));
            else _snwprintf(detail, 95, L"%d Hz   %+.1f dB", (int)(val(d.pf) + 0.5f), val(d.pg));
            detail[95] = 0;
            drawTooltip(dc, nx, ny, d.title, detail, d.tip);
        }
    }

    void paint(HDC hdc) {
        lastVersion = st->version.load(std::memory_order_acquire);
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, GUI_W, GUI_H);
        HGDIOBJ oldBmp = SelectObject(mdc, bmp);
        drawAll(mdc);
        BitBlt(hdc, 0, 0, GUI_W, GUI_H, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, oldBmp); DeleteObject(bmp); DeleteDC(mdc);
    }
};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        Gui* gg = static_cast<Gui*>(cs->lpCreateParams);
        if (gg) gg->hwnd = h;
        return DefWindowProcW(h, m, w, l);
    }
    Gui* g = reinterpret_cast<Gui*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!g) return DefWindowProcW(h, m, w, l);

    switch (m) {
        case WM_CREATE: g->createFonts(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
            g->paint(hdc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(h); SetCapture(h);
            g->onDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), false);
            if (g->dragKind == 0 && GetCapture() == h) ReleaseCapture();   // los clics en botones no necesitan captura
            return 0;
        case WM_LBUTTONDBLCLK:
            g->onDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), true);
            return 0;
        case WM_MOUSEMOVE:
            if (!g->tracking) {
                TRACKMOUSEEVENT t; ZeroMemory(&t, sizeof(t)); t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = h;
                if (TrackMouseEvent(&t)) g->tracking = true;
            }
            g->onMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_MOUSELEAVE:
            g->onLeave();
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == h) ReleaseCapture();
            g->onUp();
            return 0;
        case WM_CAPTURECHANGED:
            g->dragKind = 0;
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &pt);
            g->onWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(w));
            return 0;
        }
        case WM_TIMER:   // por si el host cambia un parametro mientras la ventana esta abierta
            if (w == 1) {
                if (g->status[0] && GetTickCount64() >= g->statusUntil) { g->status[0] = 0; g->invalidate(); }
                else if (g->st->version.load(std::memory_order_acquire) != g->lastVersion) g->invalidate();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            g->destroyFonts();
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

void* guiOpen(SharedState* st, void* parentWnd) {
    Gui* g = new Gui();
    g->st = st;
    // Nombre de clase unico por ventana: se registra al abrir y se borra al cerrar,
    // asi nunca queda una clase "huerfana" apuntando a codigo de la DLL ya descargada.
    const LONG id = InterlockedIncrement(&g_classCounter);
    _snwprintf(g->cls, 63, L"MilsTozMicGUI_%ld", (long)id);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = g->cls;
    if (!RegisterClassExW(&wc)) { delete g; return nullptr; }

    g->hwnd = CreateWindowExW(0, g->cls, L"MilsToz Mic", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                          0, 0, GUI_W, GUI_H, static_cast<HWND>(parentWnd), nullptr, g_hInst, g);
    if (!g->hwnd) { UnregisterClassW(g->cls, g_hInst); delete g; return nullptr; }
    SetTimer(g->hwnd, 1, 150, nullptr);
    return g;
}

void guiClose(void* p) {
    Gui* g = static_cast<Gui*>(p);
    if (!g) return;
    if (g->hwnd && IsWindow(g->hwnd)) DestroyWindow(g->hwnd);
    UnregisterClassW(g->cls, g_hInst);
    delete g;
}
