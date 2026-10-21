// params.h - Parametros, rangos y presets de fabrica de MilsToz Mic (v2: un solo juego de parametros + banco de presets)
#pragma once
#include <cmath>
#include <algorithm>

// Orden = orden en el que el host ve los parametros. Los 14 primeros (HPF + EQ) conservan
// exactamente el indice que tenian dentro de un slot en la version anterior.
enum SlotParam {
    SP_HPF = 0,                    // filtro pasa-altos (Hz)  [20 = apagado]
    SP_B1_F, SP_B1_G,              // B1: low shelf     (Hz, dB)   "cuerpo"
    SP_B2_F, SP_B2_G, SP_B2_Q,     // B2: peak          "caja / hueco"
    SP_B3_F, SP_B3_G, SP_B3_Q,     // B3: peak          "nasal / medios"
    SP_B4_F, SP_B4_G, SP_B4_Q,     // B4: peak          "claridad / presencia"
    SP_B5_F, SP_B5_G,              // B5: high shelf    "aire"
    // ---- cadena inferior, en orden de izquierda a derecha
    SP_GATE,                       // NOISE GATE        0 = APAGADO
    SP_BODY,                       // CUERPO            0 = APAGADO
    SP_COMP,                       // COMPRESSION       0 = APAGADO .. 1 = maximo
    SP_MODE,                       // modo compresor    0 Soft | 1 Medium | 2 Hard
    SP_DEESS,                      // DE-ESSER          0 = APAGADO
    SP_BRILLO,                     // BRILLO            0 = APAGADO
    SP_MIX,                        // MIX dry/wet       0 = todo seco .. 1 = todo procesado
    SP_OUT,                        // SALIDA (dB)
    SP_HPF_MODE,                   // modo del HPF      0 Off | 1 Smooth (12 dB/oct) | 2 Cut (24 dB/oct)
    SP_COUNT
};
enum { PARAM_COUNT = SP_COUNT };

// Modos del compresor
enum CompMode { MODE_SOFT = 0, MODE_MEDIUM = 1, MODE_HARD = 2 };
// Modos del filtro pasa-altos
enum HpfMode { HPF_OFF = 0, HPF_SMOOTH = 1, HPF_CUT = 2 };

// Release interno del gate: fijo en el 4.5 % de su escala (0..3000 ms) = 135 ms, mas 80 ms de hold.
constexpr double kGateReleaseNorm   = 0.045;
constexpr double kGateReleaseMaxSec = 3.0;

struct ParamInfo {
    const char* shortName;   // <= 7 caracteres
    const char* key;         // clave en los archivos .json de presets
    float minV, maxV;
    bool  logScale;
    const char* unit;
};

static const ParamInfo kInfo[SP_COUNT] = {
    {"HPF",   "hpf",    20.f,   300.f, true,  "Hz"},
    {"B1 F",  "b1_f",   40.f,   400.f, true,  "Hz"},
    {"B1 G",  "b1_g",   -12.f,  12.f,  false, "dB"},
    {"B2 F",  "b2_f",   100.f,  1000.f,true,  "Hz"},
    {"B2 G",  "b2_g",   -12.f,  12.f,  false, "dB"},
    {"B2 Q",  "b2_q",   0.3f,   8.f,   true,  ""},
    {"B3 F",  "b3_f",   500.f,  4000.f,true,  "Hz"},
    {"B3 G",  "b3_g",   -12.f,  12.f,  false, "dB"},
    {"B3 Q",  "b3_q",   0.3f,   8.f,   true,  ""},
    {"B4 F",  "b4_f",   1500.f, 8000.f,true,  "Hz"},
    {"B4 G",  "b4_g",   -12.f,  12.f,  false, "dB"},
    {"B4 Q",  "b4_q",   0.3f,   8.f,   true,  ""},
    {"B5 F",  "b5_f",   3000.f, 16000.f,true, "Hz"},
    {"B5 G",  "b5_g",   -12.f,  12.f,  false, "dB"},
    {"Gate",  "gate",   0.f,    1.f,   false, "%"},
    {"Cuerpo","cuerpo", 0.f,    1.f,   false, "%"},
    {"Compr", "compression", 0.f, 1.f, false, "%"},
    {"Modo",  "modo",   0.f,    2.f,   false, ""},
    {"DeEss", "deesser",0.f,    1.f,   false, "%"},
    {"Brillo","brillo", 0.f,    1.f,   false, "%"},
    {"Mix",   "mix",    0.f,    1.f,   false, "%"},
    {"Salida","salida", -12.f,  12.f,  false, "dB"},
    {"HPmode","hpf_mode", 0.f,  2.f,   false, ""},
};

inline bool isDiscreteParam(int sp) { return sp == SP_MODE || sp == SP_HPF_MODE; }

inline bool isPercentParam(int sp) {
    return sp == SP_GATE || sp == SP_BODY || sp == SP_COMP || sp == SP_DEESS || sp == SP_BRILLO || sp == SP_MIX;
}

// ---------------------------------------------------------------------------------------------
// Banco de presets de fabrica
// ---------------------------------------------------------------------------------------------
enum { NUM_PRESETS = 7 };

struct FactoryPreset { const char* name; float v[SP_COUNT]; };

// Orden: HPF | B1 F,G | B2 F,G,Q | B3 F,G,Q | B4 F,G,Q | B5 F,G | GATE CUERPO COMP MODO DEESS BRILLO MIX SALIDA | HPF_MODO
static const FactoryPreset kPresets[NUM_PRESETS] = {
    // 0) DEFAULT: totalmente plano (HPF apagado, EQ a 0 dB, todas las etapas en 0 %) = señal original
    {"Default (Plano)",
     {  20.f, 140.f, 0.0f,  280.f, 0.0f,1.0f, 1000.f, 0.0f,1.0f, 3200.f, 0.0f,1.0f, 10000.f, 0.0f,
        0.f, 0.f, 0.f, 1, 0.f, 0.f, 1.f, 0.0f,  0 }},
    // 1) Broadcast: graves/cuerpo reforzados; salida -4 dB para dejar margen (el limitador no llega a trabajar)
    {"MilsToz Broadcast (Voz Gruesa)",
     {  70.f, 140.f,+5.5f,  280.f,-1.5f,1.0f, 1000.f,-3.0f,1.0f, 3200.f,+2.0f,0.9f, 10000.f,+2.5f,
        0.f, 0.70f, 0.50f, 1, 0.45f, 0.35f, 1.f, -4.0f,  1 }},
    {"Voz Cálida y Profunda",
     {  70.f, 150.f,+4.0f,  300.f,-2.0f,1.0f,  800.f,-1.5f,1.0f, 3000.f,+1.0f,1.0f,  9000.f,+1.0f,
        0.f, 0.50f, 0.45f, 0, 0.30f, 0.20f, 1.f, -2.0f,  1 }},
    {"Voz Cristalina & Aire",
     {  90.f, 160.f, 0.0f,  350.f,-1.5f,1.2f, 1200.f,-3.0f,1.5f, 4000.f,+2.0f,1.0f, 11000.f,+4.0f,
        0.f, 0.20f, 0.40f, 0, 0.50f, 0.65f, 1.f, -1.5f,  1 }},
    {"Podcast Dinámico / En Vivo",
     {  85.f, 130.f,+2.0f,  280.f,-2.0f,1.0f, 1000.f,-1.5f,1.0f, 3200.f,+2.0f,1.0f, 10000.f,+2.0f,
        0.10f, 0.35f, 0.50f, 1, 0.55f, 0.35f, 1.f, -2.0f,  1 }},
    {"Voz Aguda & Femenina",
     {  110.f,180.f,+1.0f,  350.f,-2.0f,1.1f, 2800.f,-1.5f,1.4f, 4500.f,-1.0f,1.2f, 10000.f,+1.5f,
        0.f, 0.30f, 0.40f, 0, 0.65f, 0.30f, 1.f, -1.0f,  1 }},
    {"Control de Gritos (Gamer / Reacción)",
     {  100.f,140.f,+2.0f,  300.f,-2.0f,1.0f, 1000.f,-1.0f,1.0f, 3200.f,+1.5f,1.0f,  9000.f,+1.0f,
        0.35f, 0.30f, 0.75f, 2, 0.55f, 0.25f, 1.f, -2.0f,  1 }},
};

// ---- utilidades de mapeo (real <-> normalizado 0..1 para el host) ----
inline float paramToNorm(const ParamInfo& p, float v) {
    v = std::min(std::max(v, p.minV), p.maxV);
    return p.logScale ? std::log(v / p.minV) / std::log(p.maxV / p.minV)
                      : (v - p.minV) / (p.maxV - p.minV);
}
inline float paramFromNorm(const ParamInfo& p, float n) {
    n = std::min(std::max(n, 0.f), 1.f);
    return p.logScale ? p.minV * std::pow(p.maxV / p.minV, n) : p.minV + n * (p.maxV - p.minV);
}
