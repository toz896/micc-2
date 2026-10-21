// state.h - Estado compartido (hilo de audio <-> host <-> GUI). Sin bloqueos (atomics).
#pragma once
#include <atomic>
#include <cstdint>
#include <cmath>
#include "params.h"

struct SharedState {
    std::atomic<float>    v[SP_COUNT];        // valores REALES (Hz, dB, 0..1)
    std::atomic<int>      preset{0};          // preset de origen (0..NUM_PRESETS-1) o -1 = cargado de archivo
    std::atomic<int>      dirty{0};           // 1 = el usuario modifico algo desde que cargo el preset
    std::atomic<uint32_t> version{1};         // sube con cada cambio
    void (*notify)(void* user, int index, float normalized) = nullptr;
    void* notifyUser = nullptr;

    SharedState() { for (int i = 0; i < SP_COUNT; ++i) v[i].store(kPresets[0].v[i]); }

    float get(int idx) const { return v[idx].load(std::memory_order_relaxed); }
    void snapshot(float* out) const { for (int i = 0; i < SP_COUNT; ++i) out[i] = get(i); }
    float normalized(int idx) const { return paramToNorm(kInfo[idx], get(idx)); }

    static float sanitize(int idx, float real) {
        const ParamInfo& pi = kInfo[idx];
        real = std::min(std::max(real, pi.minV), pi.maxV);
        if (isDiscreteParam(idx)) real = std::floor(real + 0.5f);
        return real;
    }

    // Cambia un valor real (con limites). notifyHost=false cuando el cambio viene del host.
    void setReal(int idx, float real, bool notifyHost = true) {
        if (idx < 0 || idx >= SP_COUNT || !std::isfinite(real)) return;
        real = sanitize(idx, real);
        v[idx].store(real, std::memory_order_relaxed);
        version.fetch_add(1, std::memory_order_release);
        if (notifyHost && notify) notify(notifyUser, idx, paramToNorm(kInfo[idx], real));
    }
    void setNormalized(int idx, float n) {
        if (idx < 0 || idx >= SP_COUNT || !std::isfinite(n)) return;
        setReal(idx, paramFromNorm(kInfo[idx], n), false);
    }
    // Edicion hecha por el usuario en la GUI: marca el preset como "modificado"
    void edit(int idx, float real) { setReal(idx, real, true); dirty.store(1, std::memory_order_relaxed); }

    // Carga un juego completo de valores (preset de fabrica o de archivo)
    void loadValues(const float* vals, int presetIdx, bool notifyHost = true) {
        for (int i = 0; i < SP_COUNT; ++i) {
            if (!std::isfinite(vals[i])) continue;
            const float r = sanitize(i, vals[i]);
            v[i].store(r, std::memory_order_relaxed);
            if (notifyHost && notify) notify(notifyUser, i, paramToNorm(kInfo[i], r));
        }
        preset.store(presetIdx, std::memory_order_relaxed);
        dirty.store(0, std::memory_order_relaxed);
        version.fetch_add(1, std::memory_order_release);
    }
    void loadFactory(int i, bool notifyHost = true) {
        if (i < 0 || i >= NUM_PRESETS) return;
        loadValues(kPresets[i].v, i, notifyHost);
    }
};
