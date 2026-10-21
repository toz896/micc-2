// plugin.cpp - MilsToz Mic: plugin VST 2.x (64 bits) para Equalizer APO
#include <cstring>
#include <cstdio>
#include <vector>
#include <new>
#include "vst2_abi.h"
#include "state.h"
#include "dsp.h"
#include "presets.h"
#include "gui.h"

#if defined(__SSE2__) || defined(_M_X64)
  #include <xmmintrin.h>
  #define VEQ_HAVE_SSE 1
#endif

#if defined(_WIN32)
  #define VST_EXPORT extern "C" __declspec(dllexport)
#else
  #define VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr uint32_t kChunkMagicV1 = 0x31514556;   // "VEQ1": 4 slots x 18 parametros
constexpr uint32_t kChunkMagicV2 = 0x32514556;   // "VEQ2": 4 slots x 21 parametros
constexpr uint32_t kChunkMagicV3 = 0x33514556;   // "VEQ3": 22 parametros + preset + modificado (sin modo de HPF)
constexpr uint32_t kChunkMagic   = 0x34514556;   // "VEQ4": actual = SP_COUNT parametros + preset + modificado
constexpr int      kV3Params     = 22;

struct Plugin {
    AEffect effect;
    audioMasterCallback master;
    SharedState st;
    dsp::VoiceEngine engine;
    uint32_t seenVersion = 0;
    ERect rect{0, 0, 0, 0};
    void* gui = nullptr;
    std::vector<uint8_t> chunk;       // solo se toca en effGetChunk (hilo del host, no el de audio)

    explicit Plugin(audioMasterCallback m) : master(m) {}
};

Plugin* P(AEffect* e) { return static_cast<Plugin*>(e->object); }

void copyStr(void* dst, const char* s, size_t maxLen) {
    if (!dst) return;
    std::strncpy(static_cast<char*>(dst), s, maxLen - 1);
    static_cast<char*>(dst)[maxLen - 1] = 0;
}

void hostNotify(void* user, int idx, float norm) {
    Plugin* p = static_cast<Plugin*>(user);
    if (p->master) p->master(&p->effect, audioMasterAutomate, idx, 0, nullptr, norm);
}

void paramDisplay(int idx, float real, char* out8) {
    switch (idx) {
        case SP_GATE: case SP_BODY: case SP_COMP: case SP_DEESS: case SP_BRILLO:
            if (real < 0.005f) std::snprintf(out8, 8, "OFF"); else std::snprintf(out8, 8, "%d", (int)(real * 100.f + 0.5f));
            break;
        case SP_MIX:  std::snprintf(out8, 8, "%d", (int)(real * 100.f + 0.5f)); break;
        case SP_MODE: std::snprintf(out8, 8, "%s", real < 0.5f ? "Soft" : (real < 1.5f ? "Medium" : "Hard")); break;
        case SP_HPF_MODE: std::snprintf(out8, 8, "%s", real < 0.5f ? "Off" : (real < 1.5f ? "Smooth" : "Cut")); break;
        case SP_HPF: case SP_B1_F: case SP_B2_F: case SP_B3_F: case SP_B4_F: case SP_B5_F:
            std::snprintf(out8, 8, "%d", (int)(real + 0.5f)); break;
        case SP_B2_Q: case SP_B3_Q: case SP_B4_Q:
            std::snprintf(out8, 8, "%.2f", real); break;
        default:
            std::snprintf(out8, 8, "%+.1f", real); break;
    }
}

// ------------------------------------------------------------ audio (sin asignaciones de memoria)
void runEngine(Plugin* p, const float* const* in, float** out, int n) {
    const uint32_t v = p->st.version.load(std::memory_order_acquire);
    if (v != p->seenVersion) {
        p->seenVersion = v;
        float sp[SP_COUNT];
        p->st.snapshot(sp);
        p->engine.configure(sp);
    }
#ifdef VEQ_HAVE_SSE
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040);          // FTZ + DAZ: evita numeros denormales (picos de CPU)
#endif
    p->engine.process(in, out, n);
#ifdef VEQ_HAVE_SSE
    _mm_setcsr(csr);
#endif
}

void processReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    if (n <= 0 || !in || !out) return;
    runEngine(P(e), in, out, n);
}

void processAccumulating(AEffect* e, float** in, float** out, VstInt32 n) {   // API vieja, por compatibilidad
    if (n <= 0 || !in || !out) return;
    float bufL[256], bufR[256];          // en la pila: nada de memoria dinamica en el hilo de audio
    for (VstInt32 off = 0; off < n; off += 256) {
        const int m = (int)std::min<VstInt32>(256, n - off);
        const float* ii[2] = {in[0] + off, in[1] + off};
        float* oo[2] = {bufL, bufR};
        runEngine(P(e), ii, oo, m);
        for (int i = 0; i < m; ++i) { out[0][off + i] += bufL[i]; out[1][off + i] += bufR[i]; }
    }
}

void processDouble(AEffect*, double**, double**, VstInt32) {}   // no se anuncia soporte

// ------------------------------------------------------------ parametros
void setParameter(AEffect* e, VstInt32 idx, float value) { P(e)->st.setNormalized(idx, value); }
float getParameter(AEffect* e, VstInt32 idx) {
    if (idx < 0 || idx >= PARAM_COUNT) return 0.f;
    return P(e)->st.normalized(idx);
}

// ------------------------------------------------------------ migracion de ajustes de versiones anteriores
// Se toma el slot que estaba activo; los efectos nuevos quedan en su valor de fabrica.
void migrateLegacy(Plugin* p, const uint8_t* d, int count, int stride) {
    auto F = [&](int i) { float f = 0.f; if (i >= 0 && i < count) std::memcpy(&f, d + 4 * i, 4); return f; };
    int slot = (int)(F(0) + 0.5f);
    slot = slot < 0 ? 0 : (slot > 3 ? 3 : slot);
    const int base = 1 + slot * stride;
    auto L = [&](int q) { return (base + q < count) ? F(base + q) : 0.f; };
    float nv[SP_COUNT];
    for (int i = 0; i < SP_COUNT; ++i) nv[i] = kPresets[0].v[i];
    for (int q = 0; q <= SP_B5_G; ++q) if (base + q < count) nv[q] = L(q);   // HPF + EQ: mismo indice
    const float gate = L(14), comp = L(15), deess = L(16), out = L(17);
    const float comp2 = stride > 18 ? L(18) : 0.f, crys = stride > 19 ? L(19) : 0.f, body = stride > 20 ? L(20) : 0.f;
    nv[SP_GATE] = gate; nv[SP_DEESS] = deess; nv[SP_OUT] = out; nv[SP_BODY] = body; nv[SP_BRILLO] = crys;
    nv[SP_COMP] = std::max(comp, comp2);
    nv[SP_MODE] = comp2 > comp ? (float)MODE_HARD : (float)MODE_SOFT;
    nv[SP_MIX] = 1.f;
    nv[SP_HPF_MODE] = nv[SP_HPF] > 21.f ? (float)HPF_SMOOTH : (float)HPF_OFF;
    p->st.loadValues(nv, 0, false);
    p->st.dirty.store(1);
}

// ------------------------------------------------------------ dispatcher
VstIntPtr dispatcher(AEffect* e, VstInt32 op, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
    Plugin* p = P(e);
    switch (op) {
        case effOpen: return 0;
        case effClose:
            if (p->gui) { guiClose(p->gui); p->gui = nullptr; }
            delete p;
            return 0;
        case effSetProgram: case effSetProgramName: return 0;
        case effGetProgram: return 0;
        case effGetProgramName: copyStr(ptr, presets::nameOf(p->st.preset.load()), 24); return 0;

        case effGetParamLabel:
            if (index < 0 || index >= PARAM_COUNT) return 0;
            copyStr(ptr, kInfo[index].unit, 8); return 0;
        case effGetParamDisplay:
            if (index < 0 || index >= PARAM_COUNT || !ptr) return 0;
            paramDisplay(index, p->st.get(index), static_cast<char*>(ptr)); return 0;
        case effGetParamName:
            if (index < 0 || index >= PARAM_COUNT || !ptr) return 0;
            copyStr(ptr, kInfo[index].shortName, 8); return 0;

        case effSetSampleRate:
            if (opt > 1000.f) p->engine.setSampleRate((double)opt);
            p->seenVersion = 0;      // fuerza recalculo de coeficientes
            return 0;
        case effSetBlockSize: return 0;
        case effMainsChanged: if (value) p->engine.reset(); return 0;

        case effEditGetRect:
            if (!ptr) return 0;
            p->rect.top = 0; p->rect.left = 0; p->rect.bottom = (int16_t)GUI_H; p->rect.right = (int16_t)GUI_W;
            *static_cast<ERect**>(ptr) = &p->rect;
            return 1;
        case effEditOpen:
            if (p->gui) { guiClose(p->gui); p->gui = nullptr; }
            p->gui = guiOpen(&p->st, ptr);
            return p->gui ? 1 : 0;
        case effEditClose:
            if (p->gui) { guiClose(p->gui); p->gui = nullptr; }
            return 0;
        case effEditIdle: return 0;

        case effGetChunk: {       // es lo que Equalizer APO guarda en config.txt: parametros + preset activo
            const int count = PARAM_COUNT + 2;
            const size_t bytes = 8 + sizeof(float) * count;
            p->chunk.resize(bytes);
            const uint32_t hdr[2] = {kChunkMagic, (uint32_t)count};
            std::memcpy(p->chunk.data(), hdr, 8);
            for (int i = 0; i < PARAM_COUNT; ++i) {
                const float f = p->st.get(i);
                std::memcpy(p->chunk.data() + 8 + 4 * i, &f, 4);
            }
            const float pr = (float)p->st.preset.load(), di = (float)p->st.dirty.load();
            std::memcpy(p->chunk.data() + 8 + 4 * PARAM_COUNT, &pr, 4);
            std::memcpy(p->chunk.data() + 8 + 4 * (PARAM_COUNT + 1), &di, 4);
            if (ptr) *static_cast<void**>(ptr) = p->chunk.data();
            return (VstIntPtr)bytes;
        }
        case effSetChunk: {
            if (!ptr || value < 8) return 0;
            const uint8_t* d = static_cast<const uint8_t*>(ptr);
            uint32_t hdr[2]; std::memcpy(hdr, d, 8);
            const int avail = (int)((value - 8) / 4);
            const int count = (int)std::min<uint32_t>(hdr[1], (uint32_t)avail);
            if (hdr[0] == kChunkMagicV1) { migrateLegacy(p, d + 8, count, 18); return 0; }
            if (hdr[0] == kChunkMagicV2) { migrateLegacy(p, d + 8, count, 21); return 0; }
            int nParams;
            if (hdr[0] == kChunkMagic) nParams = PARAM_COUNT;
            else if (hdr[0] == kChunkMagicV3) nParams = kV3Params;
            else return 0;
            for (int i = 0; i < count && i < nParams && i < PARAM_COUNT; ++i) {
                float f; std::memcpy(&f, d + 8 + 4 * i, 4);
                p->st.setReal(i, f, false);
            }
            if (nParams < PARAM_COUNT)   // ajustes VEQ3: el HPF estaba activo si su frecuencia era > 21 Hz
                p->st.setReal(SP_HPF_MODE, p->st.get(SP_HPF) > 21.f ? (float)HPF_SMOOTH : (float)HPF_OFF, false);
            if (count >= nParams + 2) {
                float pr, di;
                std::memcpy(&pr, d + 8 + 4 * nParams, 4);
                std::memcpy(&di, d + 8 + 4 * (nParams + 1), 4);
                const int pi = std::isfinite(pr) ? (int)pr : 0;
                p->st.preset.store(pi >= -1 && pi < NUM_PRESETS ? pi : -1);
                p->st.dirty.store(std::isfinite(di) && di > 0.5f ? 1 : 0);
            }
            return 0;
        }

        case effCanBeAutomated: return (index >= 0 && index < PARAM_COUNT) ? 1 : 0;
        case effGetPlugCategory: return kPlugCategEffect;
        case effGetEffectName: copyStr(ptr, "MilsToz Mic", 32); return 1;
        case effGetVendorString: copyStr(ptr, "MilsToz", 64); return 1;
        case effGetProductString: copyStr(ptr, "MilsToz Mic", 64); return 1;
        case effGetVendorVersion: return 2000;
        case effCanDo: return 0;
        case effGetVstVersion: return 2400;
        default: return 0;
    }
}

} // namespace

VST_EXPORT AEffect* VSTPluginMain(audioMasterCallback master) {
    if (!master) return nullptr;
    master(nullptr, audioMasterVersion, 0, 0, nullptr, 0.f);

    Plugin* p = new (std::nothrow) Plugin(master);
    if (!p) return nullptr;

    std::memset(&p->effect, 0, sizeof(AEffect));
    AEffect& e = p->effect;
    e.magic = kEffectMagic;
    e.dispatcher = dispatcher;
    e.process = processAccumulating;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.numPrograms = 1;
    e.numParams = PARAM_COUNT;
    e.numInputs = 2;
    e.numOutputs = 2;
    e.flags = effFlagsHasEditor | effFlagsCanReplacing | effFlagsProgramChunks;
    e.initialDelay = 0;                       // latencia cero
    e.object = p;
    e.uniqueID = ('M' << 24) | ('T' << 16) | ('Z' << 8) | 'm';
    e.version = 2000;
    e.processReplacing = processReplacing;
    e.processDoubleReplacing = processDouble;

    p->st.notify = hostNotify;
    p->st.notifyUser = p;
    return &e;
}
