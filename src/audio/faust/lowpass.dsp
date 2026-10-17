// A stereo resonant low-pass: the filter the June 2026 web performance used
// (Web Audio's BiquadFilterNode "lowpass": second order, with a Q — the RBJ
// cookbook's, as Web Audio defines it).
//
// The coefficients are computed at CONTROL rate, once per block, then glided
// sample by sample (a one-pole each): a sweep has no zipper. Smoothing the
// cutoff itself (fi.resonlp(cutoff : si.smoo, …)) put a tan() and three
// divisions in every sample: 864 cycles per sample on the card, a third of a
// Plaits voice (MESURES §209).
//
// Translated to C++ by scripts/faust2cpp.sh -> FaustLowpass.h (never edited by hand).
declare name "nidmi-lowpass";
import("stdfaust.lib");

cutoff = hslider("cutoff[unit:Hz]", 800, 20, 20000, 1);
q      = hslider("q", 0.707, 0.5, 20, 0.01);

w0    = 2 * ma.PI * min(cutoff, 0.45 * ma.SR) / ma.SR;
alpha = sin(w0) / (2 * q);
a0    = 1 + alpha;
b0    = (1 - cos(w0)) / 2 / a0;
b1    = (1 - cos(w0)) / a0;
a1    = -2 * cos(w0) / a0;
a2    = (1 - alpha) / a0;

lowpass = fi.tf22t(b0 : si.smoo, b1 : si.smoo, b0 : si.smoo, a1 : si.smoo, a2 : si.smoo);

process = lowpass, lowpass;
