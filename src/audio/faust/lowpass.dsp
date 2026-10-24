// A stereo resonant low-pass: the filter the June 2026 web performance used
// (Web Audio's BiquadFilterNode "lowpass": second order, with a Q — the RBJ
// cookbook's, as Web Audio defines it).
//
// Written as a state-variable filter (Simper, Zavalishin's "TPT"): the same
// response as the RBJ biquad — both are the bilinear transform of
// 1/(s² + s/Q + 1), prewarped at the cutoff — made of two numbers, g = tan(w0/2)
// and k = 1/Q, that keep it stable whatever they are, as long as they are
// positive (MESURES §229). The biquad, its five coefficients glided one by one in
// float, went unstable near 20 Hz: a one-pole glide in float stops up to ~1 000
// steps of rounding beside its target, and there the poles sit 7e-6 inside the
// unit circle. Its state went to infinity, then NaN — and a NaN never leaves a
// recursion: the sound of the cue stayed dead, whatever the cutoff became.
//
// g and k are computed at CONTROL rate, once per block, then glided sample by
// sample (a one-pole each): a sweep has no zipper. Smoothing the cutoff itself
// put a tan() in every sample: 864 cycles per sample on the card, a third of a
// Plaits voice (MESURES §209). What is left per sample is one division: the
// reciprocal goes through nidmi_inverse (FaustArch.h), a function Faust cannot
// see into — written here, it became four divisions, one per product.
//
// Translated to C++ by scripts/faust2cpp.sh -> FaustLowpass.h (never edited by hand).
declare name "nidmi-lowpass";
import("stdfaust.lib");

cutoff = hslider("cutoff[unit:Hz]", 800, 20, 20000, 1);
q      = hslider("q", 0.707, 0.5, 20, 0.01);

g = tan(ma.PI * min(cutoff, 0.45 * ma.SR) / ma.SR) : si.smoo;
k = 1 / q : si.smoo;
inverse = ffunction(float nidmi_inverse(float), "FaustArch.h", "");

// One sample: the two integrators' states in, their new states and the low-pass out.
lowpass(x) = (tick ~ si.bus(2)) : (!, !, _)
with {
  a1 = inverse(1 + g * (g + k));
  a2 = g * a1;
  a3 = g * a2;
  tick(ic1, ic2) = 2 * v1 - ic1, 2 * v2 - ic2, v2
  with {
    v3 = x - ic2;
    v1 = a1 * ic1 + a2 * v3;
    v2 = ic2 + a2 * ic1 + a3 * v3;
  };
};

process = lowpass, lowpass;
