// A stereo resonant low-pass: the filter the June 2026 web performance used
// (Web Audio's BiquadFilterNode "lowpass": second order, with a Q).
// Translated to C++ by scripts/faust2cpp.sh -> FaustLowpass.h (never edited by hand).
declare name "nidmi-lowpass";
import("stdfaust.lib");

cutoff = hslider("cutoff[unit:Hz]", 800, 20, 20000, 1) : si.smoo;
q      = hslider("q", 0.707, 0.5, 20, 0.01) : si.smoo;

process = par(i, 2, fi.resonlp(cutoff, q, 1));
