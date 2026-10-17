// The reverb of the June 2026 web performance: Faust's freeverb demo (demos.lib,
// Jezar's freeverb: 8 combs and 4 all-passes per channel), Wet 0.3333.
//
// One change, and it is not a sound: the sample rate is fixed at the card's,
// 48 kHz. With ma.SR left to the runtime, Faust sizes every delay line for its
// upper bound, 192 kHz: 16 combs of 8 192 floats, ~560 KB. At 48 kHz the
// lines are their real length (2 048 floats at most).
//
// Translated to C++ by scripts/faust2cpp.sh -> FaustFreeverb.h (never edited by hand).
declare name "nidmi-freeverb";
import("stdfaust.lib");

ma48 = library("maths.lib")[SR = 48000.0;];
re48 = library("reverbs.lib")[ma = ma48;];
dm48 = library("demos.lib")[ma = ma48; re = re48;];

process = dm48.freeverb_demo;
