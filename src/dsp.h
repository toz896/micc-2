// dsp.h - Motor de audio de MilsToz Mic (v2).
// Cadena (todo por muestra, SIN lookahead => latencia = 0 muestras):
//   HPF (Off | Smooth 12 dB/oct | Cut 24 dB/oct) -> Noise Gate -> EQ 5 bandas -> CUERPO -> COMPRESSION (Soft|Medium|Hard) -> De-esser
//       -> BRILLO -> MIX (dry/wet) -> SALIDA (ganancia + soft-clip + limitador a -0.5 dBFS)
// Cada etapa se salta por completo si su perilla esta en 0 (cero CPU).
// Sin memoria dinamica en el hilo de audio: todo son variables fijas dentro de la clase.
// Numeros denormales: el host (plugin.cpp) activa FTZ+DAZ; ademas los detectores usan pisos de ruido.
#pragma once
#include <cmath>
#include <complex>
#include <algorithm>
#include "params.h"

namespace dsp {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- Biquad (RBJ)
struct BiquadCoef {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline BiquadCoef makeCoef(double b0, double b1, double b2, double a0, double a1, double a2) {
    BiquadCoef c; const double inv = 1.0 / a0;
    c.b0 = b0 * inv; c.b1 = b1 * inv; c.b2 = b2 * inv; c.a1 = a1 * inv; c.a2 = a2 * inv;
    return c;
}
inline double clampFreq(double fs, double f) { return std::min(std::max(f, 10.0), fs * 0.45); }

inline BiquadCoef highpass(double fs, double f, double q) {
    f = clampFreq(fs, f);
    const double w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    return makeCoef((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
inline BiquadCoef lowpass(double fs, double f, double q) {
    f = clampFreq(fs, f);
    const double w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
    return makeCoef((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
inline BiquadCoef peaking(double fs, double f, double dB, double q) {
    f = clampFreq(fs, f);
    const double A = std::pow(10.0, dB / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const double al = s / (2 * q);
    return makeCoef(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
inline BiquadCoef lowShelf(double fs, double f, double dB) {   // pendiente S = 1
    f = clampFreq(fs, f);
    const double A = std::pow(10.0, dB / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const double t = 2 * std::sqrt(A) * (s / std::sqrt(2.0));
    return makeCoef(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c),
                    A * ((A + 1) - (A - 1) * c - t),
                    (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
}
inline BiquadCoef highShelf(double fs, double f, double dB) {  // pendiente S = 1
    f = clampFreq(fs, f);
    const double A = std::pow(10.0, dB / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const double t = 2 * std::sqrt(A) * (s / std::sqrt(2.0));
    return makeCoef(A * ((A + 1) + (A - 1) * c + t), -2 * A * ((A - 1) + (A + 1) * c),
                    A * ((A + 1) + (A - 1) * c - t),
                    (A + 1) - (A - 1) * c + t, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - t);
}

// Respuesta en frecuencia (dB) - la usa la GUI para dibujar la curva
inline double magnitudeDb(const BiquadCoef& c, double fs, double f) {
    const double w = 2 * kPi * f / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10(std::abs(num / den) + 1e-20);
}

// Forma transpuesta II: pocas operaciones y buen comportamiento numerico
struct BiquadState {
    double z1 = 0, z2 = 0;
    inline double process(const BiquadCoef& c, double x) {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

inline double dbToLin(double db) { return std::exp(db * 0.11512925464970229); }
inline double tcCoef(double fs, double seconds) { return std::exp(-1.0 / (fs * seconds)); }

// ---------------------------------------------------------------- Compresor (3 modos)
// Detector de picos estereo (enlazado), rodilla ajustable, maquillaje automatico. Sin lookahead.
// Si progDep = true (modo Soft, estilo opto) la suelta depende del programa: cuanto mas reduccion
// de ganancia hay, mas lenta es la recuperacion (rapida -> lenta).
struct Compressor {
    bool on = false;
    double thr = -12, ratio = 2, knee = 8, makeup = 0;     // dB
    double gr = 0, env = 0;                                // estado
    double envRel = 0, atk = 0, relFast = 0, relSlow = 0;  // coeficientes
    bool progDep = false;

    void setTimes(double fs, double envRelSec, double atkSec, double relFastSec, double relSlowSec, bool pd) {
        envRel = tcCoef(fs, envRelSec); atk = tcCoef(fs, atkSec);
        relFast = tcCoef(fs, relFastSec); relSlow = tcCoef(fs, relSlowSec); progDep = pd;
    }
    void resetState() { gr = 0; env = 0; }

    inline void process(double& l, double& r) {
        const double det = std::max(std::fabs(l), std::fabs(r));
        env = det > env ? det : env * envRel;
        const double xdb = 20.0 * std::log10(env + 1e-9);
        const double over = xdb - thr;
        double ydb;
        if (2.0 * over < -knee) ydb = xdb;
        else if (2.0 * std::fabs(over) <= knee) {
            const double t = over + knee * 0.5;
            ydb = xdb + (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
        } else ydb = thr + over / ratio;
        const double grT = ydb - xdb;                      // <= 0
        double c;
        if (grT < gr) c = atk;
        else if (progDep) { const double d = std::min(-gr * (1.0 / 12.0), 1.0); c = relFast + (relSlow - relFast) * d; }
        else c = relFast;
        gr = grT + (gr - grT) * c;
        if (gr > -1e-6 && gr < 1e-6) gr = 0.0;            // evita denormales en silencio
        const double g = dbToLin(gr + makeup);
        l *= g; r *= g;
    }
};

// ---------------------------------------------------------------- Saturador suave asimetrico
// Rapido (una division). Asimetrico => genera armonicos pares (sonido "analogico", tipo valvula/transformador).
// Normalizado: para senales chicas la ganancia es 1 (solo agrega armonicos cuando sube el nivel).
struct Shaper {
    static constexpr double kBias = 0.25;
    double drive = 1.0, norm = 1.0, off = 0.0;
    void set(double d) {
        drive = d;
        const double s = 1.0 / ((1.0 + kBias) * (1.0 + kBias));   // pendiente de x/(1+|x|) en el sesgo
        norm = 1.0 / (d * s);
        off = kBias / (1.0 + kBias);
    }
    inline double process(double x) const {
        const double v = x * drive + kBias;
        return (v / (1.0 + std::fabs(v)) - off) * norm;
    }
};

// ---------------------------------------------------------------- Soft-clip transparente
// Lineal hasta -3 dBFS; por encima, curva tipo tanh (aprox. racional, sin llamadas caras) que
// se acerca asintoticamente al techo de -0.5 dBFS y NUNCA lo supera.
constexpr double kCeilDb   = -0.5;
constexpr double kCeil     = 0.9440608762859234;   // 10^(-0.5/20)
constexpr double kClipKnee = 0.7079457843841379;   // 10^(-3/20)
inline double softClip(double x) {
    const double a = std::fabs(x);
    if (a <= kClipKnee) return x;
    const double span = kCeil - kClipKnee;
    const double t = (a - kClipKnee) / span;
    const double y = t >= 3.0 ? 1.0 : t * (27.0 + t * t) / (27.0 + 9.0 * t * t);
    const double o = kClipKnee + y * span;
    return x < 0 ? -o : o;
}

// ---------------------------------------------------------------- Motor
class VoiceEngine {
public:
    VoiceEngine() { setSampleRate(48000.0); }

    void setSampleRate(double fs) {
        fs_ = std::min(std::max(fs, 8000.0), 384000.0);
        gateEnvAtk_ = tcCoef(fs_, 0.002);    // detector RMS: sube rapido...
        gateEnvRel_ = tcCoef(fs_, 0.030);    // ...y baja en 30 ms
        gateAtk_ = tcCoef(fs_, 0.002);       // abre en ~2 ms (no se come el inicio de las palabras)
        gateRel_ = tcCoef(fs_, kGateReleaseNorm * kGateReleaseMaxSec);   // release interno: 4.5 % de su escala = 135 ms
        gateHoldMax_ = (int)(0.080 * fs_);   // 80 ms de "hold"
        dsEnvRel_ = tcCoef(fs_, 0.020);
        dsAtk_ = tcCoef(fs_, 0.001);
        dsRel_ = tcCoef(fs_, 0.030);
        limRel_ = tcCoef(fs_, 0.080);
        smooth_ = 1.0 - tcCoef(fs_, 0.010);  // suavizado de MIX y SALIDA (sin "zipper noise")
        if (configured_) { float p[SP_COUNT]; std::copy(cur_, cur_ + SP_COUNT, p); configure(p); }
        else reset();
    }

    // Recalcula coeficientes con los parametros actuales (se llama solo cuando algo cambia; sin memoria dinamica)
    void configure(const float* p) {
        std::copy(p, p + SP_COUNT, cur_);
        configured_ = true;

        const int hm = std::min(std::max((int)(p[SP_HPF_MODE] + 0.5f), 0), 2);
        if (hm == HPF_SMOOTH) hpf_ = highpass(fs_, p[SP_HPF], 0.7071);                 // 12 dB/oct (Butterworth 2do orden)
        else if (hm == HPF_CUT) {                                                        // 24 dB/oct (Butterworth 4to orden)
            hpf_  = highpass(fs_, p[SP_HPF], 0.5412);
            hpf2_ = highpass(fs_, p[SP_HPF], 1.3066);
        }
        if (hm != hpfMode_) for (int c = 0; c < 2; ++c) { hpfSt_[c].reset(); hpf2St_[c].reset(); }
        hpfMode_ = hm;

        bool on[5];
        BiquadCoef cf[5];
        on[0] = std::fabs(p[SP_B1_G]) > 0.05f; if (on[0]) cf[0] = lowShelf(fs_, p[SP_B1_F], p[SP_B1_G]);
        on[1] = std::fabs(p[SP_B2_G]) > 0.05f; if (on[1]) cf[1] = peaking(fs_, p[SP_B2_F], p[SP_B2_G], p[SP_B2_Q]);
        on[2] = std::fabs(p[SP_B3_G]) > 0.05f; if (on[2]) cf[2] = peaking(fs_, p[SP_B3_F], p[SP_B3_G], p[SP_B3_Q]);
        on[3] = std::fabs(p[SP_B4_G]) > 0.05f; if (on[3]) cf[3] = peaking(fs_, p[SP_B4_F], p[SP_B4_G], p[SP_B4_Q]);
        on[4] = std::fabs(p[SP_B5_G]) > 0.05f; if (on[4]) cf[4] = highShelf(fs_, p[SP_B5_F], p[SP_B5_G]);
        for (int b = 0; b < 5; ++b) {
            if (on[b]) { band_[b] = cf[b]; if (!bandOn_[b]) { bandSt_[0][b].reset(); bandSt_[1][b].reset(); } }
            bandOn_[b] = on[b];
        }

        // ---- 1) Noise gate (expansor descendente): perilla 0 % = apagado (la senal pasa intacta)
        const double ga = p[SP_GATE];
        const bool gOn = ga > 0.005;
        if (gOn) {
            const double thr = -72.0 + 42.0 * ga;            // -72 dBFS ... -30 dBFS
            const double o = dbToLin(thr), c = dbToLin(thr - 4.0);   // histeresis de 4 dB
            gateOpenThr_ = o * o;                              // se compara contra energia (RMS al cuadrado)
            gateCloseThr_ = c * c;
            gateFloor_ = dbToLin(-(12.0 + 18.0 * ga));        // reduccion -12 ... -30 dB (suave, respeta respiraciones)
        }
        if (gOn && !gateOn_) { gateGain_ = 1.0; gateOpen_ = true; gateHold_ = gateHoldMax_; gateEnv_ = 0; }
        gateOn_ = gOn;

        // ---- 2) CUERPO: paso-banda 120-220 Hz en paralelo + armonicos pares (saturador asimetrico
        //         sobre esa banda, solo se aportan los armonicos nuevos) + low-shelf calido.
        const double ba = p[SP_BODY];
        const bool bOn = ba > 0.005;
        if (bOn) {
            bodyHp_ = highpass(fs_, 120.0, 0.7071);
            bodyLp_ = lowpass(fs_, 220.0, 0.7071);
            bodyHHp_ = highpass(fs_, 220.0, 0.7071);          // solo armonicos (2do armonico: 240-440 Hz)
            bodyHLp_ = lowpass(fs_, 1200.0, 0.7071);          // sin aspereza
            bodyShelf_ = lowShelf(fs_, 160.0, 3.0 * ba);      // calidez: hasta +3 dB bajo ~160 Hz
            bodyShaper_.set(1.0 + 5.0 * ba);
            bodyBpGain_ = 0.55 * ba;                          // hasta ~+3.8 dB en el centro de la banda
            bodyHarmMix_ = 0.9 * ba;
        }
        if (bOn && !bodyOn_) for (int c = 0; c < 2; ++c) {
            bodyHpSt_[c].reset(); bodyLpSt_[c].reset(); bodyHHpSt_[c].reset(); bodyHLpSt_[c].reset(); bodyShelfSt_[c].reset();
        }
        bodyOn_ = bOn;

        // ---- 3) COMPRESSION unificada + modo. Reduccion progresiva con maquillaje automatico.
        const double ca = p[SP_COMP];
        const bool cOn = ca > 0.005;
        const int mode = std::min(std::max((int)(p[SP_MODE] + 0.5f), 0), 2);
        if (cOn) {
            double maxRatio, kneeDb, mk;
            switch (mode) {
                case MODE_SOFT:   // Opto / LA-2A: rodilla suave, ataque lento, suelta dependiente del programa
                    comp_.setTimes(fs_, 0.010, 0.025, 0.060, 0.900, true);
                    maxRatio = 3.0; kneeDb = 12.0; mk = 0.45; break;
                case MODE_MEDIUM: // VCA: rodilla media, ataque 10 ms, ~4:1
                    comp_.setTimes(fs_, 0.005, 0.010, 0.120, 0.120, false);
                    maxRatio = 4.0; kneeDb = 6.0;  mk = 0.50; break;
                default:          // FET / 1176 anti-gritos: rodilla rigida, ataque 0.4 ms (< 1 ms), hasta 10:1
                    comp_.setTimes(fs_, 0.002, 0.0004, 0.080, 0.080, false);
                    maxRatio = 10.0; kneeDb = 0.5; mk = 0.30; break;
            }
            comp_.thr = -8.0 - 22.0 * ca;                         // -8 ... -30 dB
            comp_.ratio = 1.0 + (maxRatio - 1.0) * (0.25 + 0.75 * ca);
            comp_.knee = kneeDb;
            comp_.makeup = std::min(mk * (-comp_.thr) * (1.0 - 1.0 / comp_.ratio), 10.0);
        }
        if (cOn && !comp_.on) comp_.resetState();
        comp_.on = cOn;

        // ---- 4) De-esser: detector paso-banda 6-8 kHz, reduce solo la banda alta (split-band)
        const double da = p[SP_DEESS];
        const bool dOn = da > 0.005;
        if (dOn) {
            dsHp_ = highpass(fs_, 6000.0, 0.7071);            // sidechain: 6 kHz...
            dsDetLp_ = lowpass(fs_, 8000.0, 0.7071);          // ...a 8 kHz
            dsLp_ = lowpass(fs_, 5500.0, 0.7071);             // separacion: agudos = x - LP(x)
            dsThrDb_ = -10.0 - 30.0 * da;
        }
        if (dOn && !dsOn_) {
            dsGr_ = 0; dsEnv_ = 0;
            for (int c = 0; c < 2; ++c) { dsSt_[c].reset(); dsDetSt_[c].reset(); dsLpSt_[c].reset(); }
        }
        dsOn_ = dOn;

        // ---- 5) BRILLO: de-harsh 3.5-4.8 kHz (corte suave) + excitador de aire > 10 kHz + shelf sedoso
        const double br = p[SP_BRILLO];
        const bool brOn = br > 0.005;
        if (brOn) {
            brHarsh_ = peaking(fs_, 4150.0, -3.5 * br, 1.8);
            brHp1_ = highpass(fs_, 5000.0, 0.7071);
            brHp2_ = highpass(fs_, 10000.0, 0.7071);
            brShaper_.set(14.0);
            brAmt_ = 1.0 * br;
            brShelf_ = highShelf(fs_, 12000.0, 2.0 * br);
        }
        if (brOn && !brightOn_) for (int c = 0; c < 2; ++c) {
            brHarshSt_[c].reset(); brHp1St_[c].reset(); brHp2St_[c].reset(); brShelfSt_[c].reset();
        }
        brightOn_ = brOn;

        // ---- 6) MIX y 7) SALIDA (objetivos; se suavizan por muestra)
        mixTgt_ = std::min(std::max((double)p[SP_MIX], 0.0), 1.0);
        outTgt_ = dbToLin(p[SP_OUT]);
    }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            hpfSt_[c].reset(); hpf2St_[c].reset(); dsSt_[c].reset(); dsDetSt_[c].reset(); dsLpSt_[c].reset();
            bodyHpSt_[c].reset(); bodyLpSt_[c].reset(); bodyHHpSt_[c].reset(); bodyHLpSt_[c].reset(); bodyShelfSt_[c].reset();
            brHarshSt_[c].reset(); brHp1St_[c].reset(); brHp2St_[c].reset(); brShelfSt_[c].reset();
            for (int b = 0; b < 5; ++b) bandSt_[c][b].reset();
        }
        gateGain_ = 1.0; gateOpen_ = true; gateHold_ = 0; gateEnv_ = 0;
        comp_.resetState(); dsGr_ = 0; dsEnv_ = 0; limGain_ = 1.0;
        mixCur_ = mixTgt_; outCur_ = outTgt_;
    }

    // Procesa 2 canales (in-place permitido: se lee antes de escribir)
    void process(const float* const* in, float** out, int n) {
        const float* inL = in[0]; const float* inR = in[1];
        float* outL = out[0]; float* outR = out[1];
        bool bad = false;
        for (int i = 0; i < n; ++i) {
            const double dryL = inL[i], dryR = inR[i];
            double l = dryL, r = dryR;

            if (hpfMode_) {
                l = hpfSt_[0].process(hpf_, l); r = hpfSt_[1].process(hpf_, r);
                if (hpfMode_ == HPF_CUT) { l = hpf2St_[0].process(hpf2_, l); r = hpf2St_[1].process(hpf2_, r); }
            }

            // 1) NOISE GATE
            if (gateOn_) {
                const double e2 = std::max(l * l, r * r);
                gateEnv_ = e2 + (gateEnv_ - e2) * (e2 > gateEnv_ ? gateEnvAtk_ : gateEnvRel_);
                if (gateEnv_ < 1e-30) gateEnv_ = 0.0;
                if (gateOpen_) {
                    if (gateEnv_ < gateCloseThr_) { if (gateHold_ > 0) --gateHold_; else gateOpen_ = false; }
                    else gateHold_ = gateHoldMax_;
                } else if (gateEnv_ > gateOpenThr_) { gateOpen_ = true; gateHold_ = gateHoldMax_; }
                const double target = gateOpen_ ? 1.0 : gateFloor_;
                const double c = target > gateGain_ ? gateAtk_ : gateRel_;
                gateGain_ = target + (gateGain_ - target) * c;
                l *= gateGain_; r *= gateGain_;
            }

            // EQ 5 bandas (correctivo, antes de dar color y comprimir)
            for (int b = 0; b < 5; ++b)
                if (bandOn_[b]) { l = bandSt_[0][b].process(band_[b], l); r = bandSt_[1][b].process(band_[b], r); }

            // 2) CUERPO
            if (bodyOn_) {
                const double bl = bodyLpSt_[0].process(bodyLp_, bodyHpSt_[0].process(bodyHp_, l));   // banda 120-220 Hz
                const double br = bodyLpSt_[1].process(bodyLp_, bodyHpSt_[1].process(bodyHp_, r));
                const double hl = bodyHLpSt_[0].process(bodyHLp_, bodyHHpSt_[0].process(bodyHHp_, bodyShaper_.process(bl) - bl));
                const double hr = bodyHLpSt_[1].process(bodyHLp_, bodyHHpSt_[1].process(bodyHHp_, bodyShaper_.process(br) - br));
                l = bodyShelfSt_[0].process(bodyShelf_, l + bl * bodyBpGain_ + hl * bodyHarmMix_);
                r = bodyShelfSt_[1].process(bodyShelf_, r + br * bodyBpGain_ + hr * bodyHarmMix_);
            }

            // 3) COMPRESSION
            if (comp_.on) comp_.process(l, r);

            // 4) DE-ESSER
            if (dsOn_) {
                const double hl = dsDetSt_[0].process(dsDetLp_, dsSt_[0].process(dsHp_, l));
                const double hr = dsDetSt_[1].process(dsDetLp_, dsSt_[1].process(dsHp_, r));
                const double lvl = std::max(std::fabs(hl), std::fabs(hr));
                dsEnv_ = lvl > dsEnv_ ? lvl : dsEnv_ * dsEnvRel_;
                double gdb = 0.0;
                if (dsEnv_ > 1e-6) {
                    const double ed = 20.0 * std::log10(dsEnv_);
                    if (ed > dsThrDb_) { gdb = (dsThrDb_ - ed) * 0.75; if (gdb < -12.0) gdb = -12.0; }
                } else dsEnv_ = 0.0;
                const double c = gdb < dsGr_ ? dsAtk_ : dsRel_;
                dsGr_ = gdb + (dsGr_ - gdb) * c;
                if (dsGr_ > -1e-6) dsGr_ = 0.0;
                const double lowL = dsLpSt_[0].process(dsLp_, l), lowR = dsLpSt_[1].process(dsLp_, r);
                if (dsGr_ < -0.01) {
                    const double k = 1.0 - dbToLin(dsGr_);
                    l -= (l - lowL) * k; r -= (r - lowR) * k;   // reduce solo la banda alta
                }
            }

            // 5) BRILLO
            if (brightOn_) {
                l = brHarshSt_[0].process(brHarsh_, l); r = brHarshSt_[1].process(brHarsh_, r);     // de-harsh
                const double al = brHp1St_[0].process(brHp1_, l), ar = brHp1St_[1].process(brHp1_, r);
                const double xl = brHp2St_[0].process(brHp2_, brShaper_.process(al) - al);           // solo aire nuevo > 10 kHz
                const double xr = brHp2St_[1].process(brHp2_, brShaper_.process(ar) - ar);
                l = brShelfSt_[0].process(brShelf_, l + xl * brAmt_);
                r = brShelfSt_[1].process(brShelf_, r + xr * brAmt_);
            }

            // 6) MIX dry/wet paralelo y 7) SALIDA
            mixCur_ += (mixTgt_ - mixCur_) * smooth_;
            outCur_ += (outTgt_ - outCur_) * smooth_;
            l = (dryL + (l - dryL) * mixCur_) * outCur_;
            r = (dryR + (r - dryR) * mixCur_) * outCur_;

            // Limitador brickwall (ataque instantaneo, sin latencia) + soft-clip transparente. Techo: -0.5 dBFS
            const double pk = std::max(std::fabs(l), std::fabs(r));
            const double tg = pk > kCeil ? kCeil / pk : 1.0;
            if (tg < limGain_) limGain_ = tg; else limGain_ = tg + (limGain_ - tg) * limRel_;
            l = softClip(l * limGain_); r = softClip(r * limGain_);
            l = std::min(std::max(l, -kCeil), kCeil); r = std::min(std::max(r, -kCeil), kCeil);   // red de seguridad: jamas pasa de -0.5 dBFS

            if (!(std::fabs(l) <= 1.0) || !(std::fabs(r) <= 1.0)) { bad = true; l = r = 0.0; }
            outL[i] = (float)l; outR[i] = (float)r;
        }
        if (bad) reset();   // protege contra NaN/Inf que "envenenan" los filtros
    }

    // Para la GUI: curva total del EQ (dB) a una frecuencia, con un juego de parametros
    static double eqResponseDb(const float* p, double f, double fs = 48000.0) {
        double db = 0.0;
        const int hm = (int)(p[SP_HPF_MODE] + 0.5f);
        if (hm == HPF_SMOOTH) db += magnitudeDb(highpass(fs, p[SP_HPF], 0.7071), fs, f);
        else if (hm >= HPF_CUT) db += magnitudeDb(highpass(fs, p[SP_HPF], 0.5412), fs, f) + magnitudeDb(highpass(fs, p[SP_HPF], 1.3066), fs, f);
        if (std::fabs(p[SP_B1_G]) > 0.05f) db += magnitudeDb(lowShelf(fs, p[SP_B1_F], p[SP_B1_G]), fs, f);
        if (std::fabs(p[SP_B2_G]) > 0.05f) db += magnitudeDb(peaking(fs, p[SP_B2_F], p[SP_B2_G], p[SP_B2_Q]), fs, f);
        if (std::fabs(p[SP_B3_G]) > 0.05f) db += magnitudeDb(peaking(fs, p[SP_B3_F], p[SP_B3_G], p[SP_B3_Q]), fs, f);
        if (std::fabs(p[SP_B4_G]) > 0.05f) db += magnitudeDb(peaking(fs, p[SP_B4_F], p[SP_B4_G], p[SP_B4_Q]), fs, f);
        if (std::fabs(p[SP_B5_G]) > 0.05f) db += magnitudeDb(highShelf(fs, p[SP_B5_F], p[SP_B5_G]), fs, f);
        return db;
    }

private:
    double fs_ = 48000.0;
    bool configured_ = false;
    float cur_[SP_COUNT] = {};

    int hpfMode_ = 0; BiquadCoef hpf_, hpf2_; BiquadState hpfSt_[2], hpf2St_[2];
    bool bandOn_[5] = {false, false, false, false, false};
    BiquadCoef band_[5]; BiquadState bandSt_[2][5];

    bool gateOn_ = false, gateOpen_ = true; int gateHold_ = 0, gateHoldMax_ = 3840;
    double gateEnv_ = 0, gateGain_ = 1, gateOpenThr_ = 0, gateCloseThr_ = 0, gateFloor_ = 0.1;
    double gateEnvAtk_ = 0, gateEnvRel_ = 0, gateAtk_ = 0, gateRel_ = 0;

    bool bodyOn_ = false; double bodyBpGain_ = 0, bodyHarmMix_ = 0; Shaper bodyShaper_;
    BiquadCoef bodyHp_, bodyLp_, bodyHHp_, bodyHLp_, bodyShelf_;
    BiquadState bodyHpSt_[2], bodyLpSt_[2], bodyHHpSt_[2], bodyHLpSt_[2], bodyShelfSt_[2];

    Compressor comp_;

    bool dsOn_ = false; BiquadCoef dsHp_, dsDetLp_, dsLp_; BiquadState dsSt_[2], dsDetSt_[2], dsLpSt_[2];
    double dsThrDb_ = -20, dsGr_ = 0, dsEnv_ = 0, dsEnvRel_ = 0, dsAtk_ = 0, dsRel_ = 0;

    bool brightOn_ = false; double brAmt_ = 0; Shaper brShaper_;
    BiquadCoef brHarsh_, brHp1_, brHp2_, brShelf_;
    BiquadState brHarshSt_[2], brHp1St_[2], brHp2St_[2], brShelfSt_[2];

    double mixTgt_ = 1.0, mixCur_ = 1.0, outTgt_ = 1.0, outCur_ = 1.0, smooth_ = 0.002;
    double limGain_ = 1.0, limRel_ = 0;
};

} // namespace dsp
