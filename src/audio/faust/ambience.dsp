// A light reverb, made for the card: two diffusing all-passes, then four delay
// lines in a feedback network (Hadamard), damped; stereo in (summed), stereo out.
//
// Why not the June freeverb: its 24 delay lines (149 KB) only fit in PSRAM,
// and reading them there cost 75 % of a core on average, 177 % on the worst
// block (MESURES §209). Here the lines are short (11.6 to 20.8 ms, prime
// lengths) and hold 18 KB: small enough to stay in the cache, wherever they live.
//
// Translated to C++ by scripts/faust2cpp.sh -> FaustAmbience.h (never edited by hand).
declare name "nidmi-ambience";
import("stdfaust.lib");

decay = hslider("decay[unit:s]", 1.2, 0.1, 4, 0.01);   // time to -60 dB
damp  = hslider("damp", 0.35, 0, 0.9, 0.01);           // the highs die first
wet   = hslider("wet", 0.25, 0, 1, 0.01);

N = 4;
len(0) = 557; len(1) = 683; len(2) = 811; len(3) = 997;   // samples at 48 kHz, all < 1024

gain(i) = pow(0.001, len(i) / (decay * ma.SR));          // per pass, so that the whole decays in `decay`
line(i) = @(len(i) - 1) : *(1 - damp) : fi.pole(damp) : *(gain(i));
fdn = (si.bus(N), si.bus(N) :> par(i, N, line(i))) ~ (ro.hadamard(N) : par(i, N, *(0.5)));

diffuse = fi.allpass_comb(256, 241, -0.6) : fi.allpass_comb(128, 109, -0.6);
room = + : *(0.5) : diffuse <: par(i, N, *(1 - 2 * (i % 2))) : fdn
     : ro.interleave(2, 2) : +, + : *(0.5), *(0.5);

process = _, _ <: (room : *(wet), *(wet)), (*(1 - wet), *(1 - wet)) :> _, _;
