#!/bin/bash
# scripts/faust2cpp.sh — a Faust .dsp translated to a C++ class for the firmware.
#
#   bash scripts/faust2cpp.sh src/audio/faust/freeverb.dsp FaustFreeverb
#   -> src/audio/faust/FaustFreeverb.h
#
# The compiler: `faust` on the PATH, or FAUST=<its bin/faust> — the official
# GRAME release (Faust-<version>-arm64.dmg), installed in ~/Applications/Faust
# (a link to its version's folder) and linked as ~/.local/bin/faust. Its
# libraries are where it says (--dspdir): through a link, the folder next to the
# command is not its own. The architecture is faust-arch.h.in (+ FaustArch.h), next to
# the .dsp. Options: single precision; denormals flushed by the generated code
# (-ftz 2, as the June web performance was compiled); superclass FaustDsp.
set -e
DSP="${1:?usage: faust2cpp.sh <file.dsp> <ClassName>}"
CLASS="${2:?usage: faust2cpp.sh <file.dsp> <ClassName>}"
FAUST="${FAUST:-faust}"
DIR="$(dirname "$DSP")"
LIBS="$("$FAUST" --dspdir)"
"$FAUST" -I "$LIBS" -lang cpp -single -ftz 2 -cn "$CLASS" -scn FaustDsp \
  -a "$DIR/faust-arch.h.in" -o "$DIR/$CLASS.h" "$DSP"
echo "$DIR/$CLASS.h ($("$FAUST" --version | head -1))"
