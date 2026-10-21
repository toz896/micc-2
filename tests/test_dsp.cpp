// test_dsp.cpp - pruebas del motor, presets y chunk (Linux/WSL: ./run_tests.sh)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <cmath>
#include "../src/vst2_abi.h"
#include "../src/dsp.h"
#include "../src/presets.h"
#include "../src/state.h"
#include <xmmintrin.h>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);
static VstIntPtr master(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) { return 0; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("  FALLA: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static const double FS = 48000.0;

static void baseParams(float* p) {   // todo apagado, EQ plano, mix 100 %
    for (int i = 0; i < SP_COUNT; ++i) p[i] = 0.f;
    p[SP_HPF] = 20.f; p[SP_B1_F] = 140.f; p[SP_B2_F] = 280.f; p[SP_B2_Q] = 1.f; p[SP_B3_F] = 1000.f; p[SP_B3_Q] = 1.f;
    p[SP_B4_F] = 3200.f; p[SP_B4_Q] = 1.f; p[SP_B5_F] = 10000.f; p[SP_MIX] = 1.f;
}
static void run(dsp::VoiceEngine& e, const std::vector<float>& in, std::vector<float>& out) {
    out.assign(in.size(), 0.f);
    const float* ii[2] = {in.data(), in.data()}; float* oo[2] = {out.data(), out.data()};
    // dos canales distintos no hacen falta: se usa el mismo buffer de salida para L y R seria un choque -> buffer aparte
    std::vector<float> r(in.size());
    oo[1] = r.data();
    e.process(ii, oo, (int)in.size());
}
static std::vector<float> tone(double f, double amp, double sec) {
    std::vector<float> x((size_t)(sec * FS));
    for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(amp * std::sin(2 * dsp::kPi * f * i / FS));
    return x;
}
static double peak(const std::vector<float>& x, size_t a = 0, size_t b = (size_t)-1) {
    double m = 0; for (size_t i = a; i < x.size() && i < b; ++i) m = std::max(m, (double)std::fabs(x[i])); return m;
}

int main() {
    std::printf("== 1) bypass real: todo apagado = senal identica\n");
    { dsp::VoiceEngine e; e.setSampleRate(FS); float p[SP_COUNT]; baseParams(p); e.configure(p); e.reset();
      auto x = tone(440, 0.3, 0.5); std::vector<float> y; run(e, x, y);
      double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max(d, (double)std::fabs(x[i] - y[i]));
      CHECK(d < 1e-6, "diferencia %g", d); }

    std::printf("== 2) latencia cero: un impulso sale en la MISMA muestra\n");
    for (int k = 0; k < NUM_PRESETS; ++k) {
      dsp::VoiceEngine e; e.setSampleRate(FS); e.configure(kPresets[k].v); e.reset();
      std::vector<float> x(64, 0.f), y; x[0] = 0.5f; run(e, x, y);
      CHECK(std::fabs(y[0]) > 1e-4, "preset %d: y[0]=%g", k, y[0]);
    }

    std::printf("== 3) techo -0.5 dBFS (entrada hasta +12 dBFS) y sin NaN en todos los presets\n");
    for (int k = 0; k < NUM_PRESETS; ++k) {
      dsp::VoiceEngine e; e.setSampleRate(FS); e.configure(kPresets[k].v); e.reset();
      auto x = tone(220, 4.0, 1.0); std::vector<float> y; run(e, x, y);
      const double pk = peak(y);
      CHECK(pk <= dsp::kCeil + 1e-6, "preset %d: pico %.4f (%.2f dBFS)", k, pk, 20 * std::log10(pk));
      CHECK(std::isfinite(pk), "preset %d: no finito", k);
    }

    std::printf("== 4) compresor: ataque Soft > Medium > Hard y reduccion progresiva\n");
    double t90[3] = {0, 0, 0}, grFinal[3] = {0, 0, 0};
    for (int m = 0; m < 3; ++m) {
      dsp::VoiceEngine e; e.setSampleRate(FS); float p[SP_COUNT]; baseParams(p);
      p[SP_COMP] = 0.6f; p[SP_MODE] = (float)m; e.configure(p); e.reset();
      auto lo = tone(1000, 0.03, 0.5), hi = tone(1000, 0.5, 1.0);
      std::vector<float> x = lo; x.insert(x.end(), hi.begin(), hi.end());
      std::vector<float> y; run(e, x, y);
      // medir envolvente (pico por ciclo de 1 ms) en la parte alta
      const size_t off = lo.size(), w = (size_t)(0.001 * FS);
      std::vector<double> env; for (size_t i = off; i + w < y.size(); i += w) env.push_back(peak(y, i, i + w));
      const double pk0 = env.front(), pkEnd = env.back();
      double target = pk0 - 0.9 * (pk0 - pkEnd);   // 90 % del camino hacia el valor final
      size_t idx = 0; while (idx < env.size() && env[idx] > target) ++idx;
      t90[m] = idx * 1.0; grFinal[m] = 20 * std::log10(pkEnd / 0.5);
      std::printf("  modo %d: GR+makeup final %.1f dB, 90%% de la reduccion en %.0f ms (pico inicial %.3f -> %.3f)\n", m, grFinal[m], t90[m], pk0, pkEnd);
    }
    CHECK(t90[0] > t90[1] && t90[1] > t90[2], "ataques no ordenados: %.0f %.0f %.0f ms", t90[0], t90[1], t90[2]);
    CHECK(t90[2] <= 6.0, "Hard debe reaccionar en pocos ms, fue %.0f ms", t90[2]);
    { // progresivo: mas perilla = mas reduccion (modo Medium, nivel -6 dBFS, sin makeup comparado via nivel de salida relativo)
      double lastRed = -1;
      for (int c = 1; c <= 4; ++c) {
        dsp::VoiceEngine e; e.setSampleRate(FS); float p[SP_COUNT]; baseParams(p);
        p[SP_COMP] = c * 0.25f; p[SP_MODE] = 1.f; e.configure(p); e.reset();
        auto loud = tone(1000, 0.5, 1.0), quiet = tone(1000, 0.03, 1.0); std::vector<float> yl, yq;
        run(e, loud, yl); e.reset(); run(e, quiet, yq);
        const double dyn = 20 * std::log10(peak(yl, 24000) / peak(yq, 24000));   // rango dinamico a la salida
        const double inDyn = 20 * std::log10(0.5 / 0.03);
        const double red = inDyn - dyn;
        std::printf("  compresion %d%%: rango %.1f dB -> %.1f dB (reduccion %.1f dB)\n", c * 25, inDyn, dyn, red);
        CHECK(red > lastRed - 0.01, "no progresiva"); lastRed = red;
      }
    }

    std::printf("== 5) gate: 0 %% = abierto; release fijo ~135 ms\n");
    { dsp::VoiceEngine e; e.setSampleRate(FS); float p[SP_COUNT]; baseParams(p); p[SP_GATE] = 0.5f; e.configure(p); e.reset();
      auto a = tone(300, 0.2, 0.3); std::vector<float> sil((size_t)(1.0 * FS), 0.f), x = a; x.insert(x.end(), sil.begin(), sil.end());
      for (size_t i = a.size(); i < x.size(); ++i) x[i] = 1e-4f * (float)std::sin(0.3 * i);   // ruido de fondo muy bajo
      std::vector<float> y; run(e, x, y);
      const size_t w = (size_t)(0.005 * FS); double first = peak(x, a.size(), a.size() + w);
      double tClose = -1;
      for (size_t i = a.size(); i + w < x.size(); i += w) {
        const double g = peak(y, i, i + w) / (peak(x, i, i + w) + 1e-12);
        if (g < 0.3) { tClose = (i - a.size()) / FS * 1000.0; break; }
      }
      (void)first;
      std::printf("  el gate bajo a <30%% de ganancia tras %.0f ms\n", tClose);
      CHECK(tClose > 100 && tClose < 600, "tiempo de cierre %.0f ms fuera de rango suave", tClose); }

    std::printf("== 6) MIX = 0 deja pasar solo la senal seca\n");
    { dsp::VoiceEngine e; e.setSampleRate(FS); e.configure(kPresets[1].v); float p[SP_COUNT]; std::copy(kPresets[1].v, kPresets[1].v + SP_COUNT, p);
      p[SP_MIX] = 0.f; p[SP_OUT] = 0.f; e.configure(p); e.reset();
      auto x = tone(500, 0.3, 0.6); std::vector<float> y; run(e, x, y);
      double d = 0; for (size_t i = 24000; i < x.size(); ++i) d = std::max(d, (double)std::fabs(x[i] - y[i]));
      CHECK(d < 1e-3, "mix=0 difiere %g", d); }

    std::printf("== 7) silencio tras senal fuerte: sin NaN/denormales; consumo de CPU\n");
    { dsp::VoiceEngine e; e.setSampleRate(FS); float p[SP_COUNT]; std::copy(kPresets[1].v, kPresets[1].v + SP_COUNT, p);
      p[SP_GATE] = 0.5f; p[SP_COMP] = 0.8f; p[SP_DEESS] = 0.8f; p[SP_BRILLO] = 1.f; p[SP_BODY] = 1.f; e.configure(p); e.reset();
      std::vector<float> a = tone(180, 0.7, 0.5), sil((size_t)(20 * FS), 0.f), y;
      const unsigned csr = _mm_getcsr(); _mm_setcsr(csr | 0x8040);
      run(e, a, y);
      auto t0 = std::chrono::steady_clock::now(); run(e, sil, y);
      auto t1 = std::chrono::steady_clock::now();
      // 60 s de voz sintetica con TODO activado
      std::vector<float> v((size_t)(30 * FS)); for (size_t i = 0; i < v.size(); ++i) v[i] = (float)(0.3 * std::sin(2*dsp::kPi*150*i/FS) * (0.6 + 0.4*std::sin(2*dsp::kPi*3*i/FS)) + 0.05 * std::sin(2*dsp::kPi*7000*i/FS));
      auto t2 = std::chrono::steady_clock::now(); run(e, v, y); auto t3 = std::chrono::steady_clock::now();
      _mm_setcsr(csr);
      const double ts = std::chrono::duration<double>(t1 - t0).count(), tv = std::chrono::duration<double>(t3 - t2).count();
      std::printf("  20 s de silencio: %.1f ms (%.2f%% de un nucleo)   30 s de voz, todo activo: %.1f ms (%.2f%% de un nucleo)\n", ts*1e3, ts/20*100, tv*1e3, tv/30*100);
      CHECK(peak(y) < 1.0 && std::isfinite(peak(y)), "salida invalida");
      CHECK(tv / 30 < 0.05, "CPU demasiado alta"); }

    std::printf("== 8) JSON: ida y vuelta de los presets de fabrica\n");
    for (int k = 0; k < NUM_PRESETS; ++k) {
      std::string js = presets::toJson(kPresets[k].v, kPresets[k].name);
      float o[SP_COUNT]; for (int i = 0; i < SP_COUNT; ++i) o[i] = -999.f; std::string nm;
      const bool ok = presets::fromJson(js, o, &nm);
      CHECK(ok, "preset %d no se pudo leer", k);
      double d = 0; for (int i = 0; i < SP_COUNT; ++i) d = std::max(d, (double)std::fabs(o[i] - kPresets[k].v[i]));
      CHECK(d < 1e-3, "preset %d difiere %g", k, d);
      CHECK(nm == kPresets[k].name, "nombre: '%s'", nm.c_str());
    }
    { float o[SP_COUNT] = {}; CHECK(!presets::fromJson("{ basura }", o), "acepto basura"); }

    std::printf("== 9) chunk VST: guardar/cargar + migracion de ajustes viejos\n");
    { AEffect* a = VSTPluginMain(master); AEffect* b = VSTPluginMain(master);
      a->setParameter(a, SP_COMP, 0.5f); a->setParameter(a, SP_MODE, 1.0f);
      void* ch = nullptr; VstIntPtr n = a->dispatcher(a, effGetChunk, 0, 0, &ch, 0);
      b->dispatcher(b, effSetChunk, 0, n, ch, 0);
      for (int i = 0; i < SP_COUNT; ++i) CHECK(std::fabs(a->getParameter(a, i) - b->getParameter(b, i)) < 1e-6, "param %d", i);
      // chunk VEQ2 viejo: slot activo = 3 (MilsToz viejo)
      const int stride = 21; std::vector<float> old(1 + 4 * stride, 0.f); old[0] = 3.f;
      const float mils[21] = { 80.f, 130.f,+5.5f, 250.f,-2.0f,1.0f, 1100.f,-6.5f,1.1f, 3500.f,+2.5f,0.9f, 9500.f,+4.0f, 0.f, 0.50f, 0.50f, -4.4f, 0.25f, 0.50f, 0.40f };
      for (int q = 0; q < 21; ++q) old[1 + 3 * stride + q] = mils[q];
      std::vector<uint8_t> blob(8 + 4 * old.size()); uint32_t hdr[2] = {0x32514556u, (uint32_t)old.size()};
      std::memcpy(blob.data(), hdr, 8); std::memcpy(blob.data() + 8, old.data(), 4 * old.size());
      AEffect* c = VSTPluginMain(master); c->dispatcher(c, effSetChunk, 0, (VstIntPtr)blob.size(), blob.data(), 0);
      void* ch2 = nullptr; VstIntPtr n2 = c->dispatcher(c, effGetChunk, 0, 0, &ch2, 0);
      const float* f = (const float*)((const uint8_t*)ch2 + 8); (void)n2;
      CHECK(std::fabs(f[SP_B1_G] - 5.5f) < 1e-4, "EQ migrado: %g", f[SP_B1_G]);
      CHECK(std::fabs(f[SP_COMP] - 0.5f) < 1e-4 && f[SP_MODE] == 0.0f, "comp migrado: %g modo %g (comp 0.5 > comp2 0.25 => Soft)", f[SP_COMP], f[SP_MODE]);
      CHECK(std::fabs(f[SP_BRILLO] - 0.5f) < 1e-4 && std::fabs(f[SP_BODY] - 0.4f) < 1e-4, "brillo/cuerpo migrados");
      CHECK(std::fabs(f[SP_OUT] + 4.4f) < 1e-4 && f[SP_MIX] == 1.0f, "salida/mix");
      for (AEffect* z : {a, b, c}) z->dispatcher(z, effClose, 0, 0, nullptr, 0); }

    std::printf("== 10) preset Default plano = senal identica; Broadcast no satura\n");
    { dsp::VoiceEngine e; e.setSampleRate(FS); e.configure(kPresets[0].v); e.reset();
      auto x = tone(440, 0.3, 0.5); std::vector<float> y; run(e, x, y);
      double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max(d, (double)std::fabs(x[i] - y[i]));
      CHECK(d < 1e-6, "Default no es plano: %g", d);
      CHECK(kPresets[0].v[SP_COMP] == 0.f && kPresets[0].v[SP_GATE] == 0.f && kPresets[0].v[SP_HPF_MODE] == 0.f, "Default con etapas activas"); }
    { dsp::VoiceEngine e; e.setSampleRate(FS); e.configure(kPresets[1].v); e.reset();
      auto x = tone(120, 0.35, 1.0); std::vector<float> y; run(e, x, y);   // voz grave fuerte (-9 dBFS)
      CHECK(peak(y) < dsp::kClipKnee, "Broadcast llega a la zona de soft-clip: %.3f", peak(y)); }

    std::printf("== 11) HPF: Off / Smooth 12 dB/oct / Cut 24 dB/oct\n");
    { float p[SP_COUNT]; baseParams(p); p[SP_HPF] = 100.f;
      double sl[3];
      for (int m = 0; m < 3; ++m) {
        p[SP_HPF_MODE] = (float)m;
        sl[m] = dsp::VoiceEngine::eqResponseDb(p, 50.0) - dsp::VoiceEngine::eqResponseDb(p, 25.0);   // dB por octava, 2 octavas bajo el corte
        std::printf("  modo %d: pendiente %.1f dB/oct, respuesta a 25 Hz %.1f dB\n", m, sl[m], dsp::VoiceEngine::eqResponseDb(p, 25.0));
      }
      CHECK(std::fabs(sl[0]) < 1e-6, "Off no es plano");
      CHECK(sl[1] > 11.0 && sl[1] < 13.0, "Smooth %.1f dB/oct", sl[1]);
      CHECK(sl[2] > 23.0 && sl[2] < 25.0, "Cut %.1f dB/oct", sl[2]);
      // el motor real debe coincidir: tono de 25 Hz mucho mas atenuado en Cut que en Smooth
      double pk[3];
      for (int m = 0; m < 3; ++m) {
        dsp::VoiceEngine e; e.setSampleRate(FS); p[SP_HPF_MODE] = (float)m; e.configure(p); e.reset();
        auto x = tone(25, 0.5, 2.0); std::vector<float> y; run(e, x, y); pk[m] = peak(y, 48000);
      }
      CHECK(pk[0] > 0.49 && pk[1] < pk[0] * 0.2 && pk[2] < pk[1] * 0.2, "motor: %.3f %.3f %.3f", pk[0], pk[1], pk[2]); }

    std::printf("== 12) chunk VEQ3 (sin modo HPF): migra y conserva preset\n");
    { std::vector<float> v3(22 + 2); for (int i = 0; i < 22; ++i) v3[i] = kPresets[1].v[i]; v3[22] = 1.f; v3[23] = 0.f;
      std::vector<uint8_t> blob(8 + 4 * v3.size()); uint32_t hdr[2] = {0x33514556u, (uint32_t)v3.size()};
      std::memcpy(blob.data(), hdr, 8); std::memcpy(blob.data() + 8, v3.data(), 4 * v3.size());
      AEffect* c = VSTPluginMain(master); c->dispatcher(c, effSetChunk, 0, (VstIntPtr)blob.size(), blob.data(), 0);
      void* ch = nullptr; c->dispatcher(c, effGetChunk, 0, 0, &ch, 0);
      const float* f = (const float*)((const uint8_t*)ch + 8);
      CHECK(f[SP_HPF_MODE] == 1.f && std::fabs(f[SP_B1_G] - 5.5f) < 1e-4 && f[SP_COUNT] == 1.f, "VEQ3: modo %g b1g %g preset %g", f[SP_HPF_MODE], f[SP_B1_G], f[SP_COUNT]);
      c->dispatcher(c, effClose, 0, 0, nullptr, 0); }

    std::printf("%s (%d fallos)\n", fails ? "RESULTADO: FALLO" : "RESULTADO: TODO OK", fails);
    return fails ? 1 : 0;
}
