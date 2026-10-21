// presets.h - Preset Manager: serializacion a .json (ligero, sin dependencias) y utilidades.
// Todo esto corre SOLO en el hilo de la GUI / del host (nunca en el hilo de audio).
#pragma once
#include <string>
#include <sstream>
#include <locale>
#include <cstring>
#include <cctype>
#include "params.h"

namespace presets {

// Nombre de un preset para mostrar (indice -1 = "Personalizado")
inline const char* nameOf(int idx) { return (idx >= 0 && idx < NUM_PRESETS) ? kPresets[idx].name : "Personalizado"; }

inline std::string toJson(const float* v, const char* name) {
    std::string nm;
    for (const char* c = name ? name : ""; *c; ++c) if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20) nm += *c;
    std::ostringstream os; os.imbue(std::locale::classic());
    os.setf(std::ios::fixed); os.precision(4);
    os << "{\n  \"format\": \"MilsToz Mic preset\",\n  \"version\": 1,\n  \"name\": \"" << nm << "\",\n  \"params\": {\n";
    for (int i = 0; i < SP_COUNT; ++i)
        os << "    \"" << kInfo[i].key << "\": " << v[i] << (i + 1 < SP_COUNT ? "," : "") << "\n";
    os << "  }\n}\n";
    return os.str();
}

inline bool findNumber(const std::string& t, size_t from, const char* key, double& out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = t.find(k, from);
    if (p == std::string::npos) return false;
    p = t.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
    std::istringstream is(t.substr(p, 40)); is.imbue(std::locale::classic());
    double d; if (!(is >> d)) return false;
    out = d; return true;
}

// 'out' debe venir con los valores actuales: lo que falte en el archivo se conserva.
// Devuelve false si el texto no parece un preset valido (menos de 10 parametros reconocidos).
inline bool fromJson(const std::string& text, float* out, std::string* nameOut = nullptr) {
    const size_t pp = text.find("\"params\"");
    if (pp == std::string::npos) return false;
    int found = 0; bool have[SP_COUNT] = {};
    for (int i = 0; i < SP_COUNT; ++i) {
        double d;
        if (findNumber(text, pp, kInfo[i].key, d) && std::isfinite(d)) {
            const ParamInfo& pi = kInfo[i];
            float f = (float)d;
            f = std::min(std::max(f, pi.minV), pi.maxV);
            if (isDiscreteParam(i)) f = std::floor(f + 0.5f);
            out[i] = f; ++found; have[i] = true;
        }
    }
    if (found < 10) return false;
    // Presets antiguos (sin "hpf_mode"): el HPF estaba activo si su frecuencia era > 21 Hz
    if (!have[SP_HPF_MODE] && have[SP_HPF]) out[SP_HPF_MODE] = out[SP_HPF] > 21.f ? (float)HPF_SMOOTH : (float)HPF_OFF;
    if (nameOut) {
        nameOut->clear();
        const size_t n = text.find("\"name\"");
        if (n != std::string::npos && n < pp) {
            const size_t c = text.find(':', n + 6);
            const size_t a = (c == std::string::npos) ? c : text.find('"', c);
            const size_t b = (a == std::string::npos) ? a : text.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos && b - a < 80) *nameOut = text.substr(a + 1, b - a - 1);
        }
    }
    return true;
}

} // namespace presets
