// src/mapping/VocabulaireEmbarque.h — GENERE, NE PAS EDITER A LA MAIN.
//
//     python3 scripts/generer-vocabulaire.py
//
// CE QUE CETTE CARTE-CI EXECUTE du langage .nms, derive de MappingEngine.cpp.
//
// L'app ne le devine plus : elle le DEMANDE, a /api/mapping/vocabulaire. Elle
// en portait une copie ecrite a la main — dix noms quand la carte en executait
// cent — qui signalait comme inexistants des objets parfaitement executes. Une
// liste recopiee derive ; celle-ci est derivee de ce qui execute, et ne
// traverse aucune frontiere de depot pour l'etre.
//
// Servi tel quel depuis la flash : la route ne construit rien.

#pragma once

static const char VOCABULAIRE_EMBARQUE_JSON[] =
  "{\"n\":108,\"objets\":[\"!=\",\"%\",\"&\",\"*\",\"+\",\"-\",\"/\",\"<\",\"<<\",\"<=\",\"==\",\">\",\">=\",\">>\",\"^\","
  "\"abs\",\"and\",\"atan\",\"atan2\",\"b\",\"bang\",\"bend.in\",\"bend.out\",\"block\",\"ccnum.in\",\"ceil\","
  "\"change\",\"clamp\",\"cos\",\"counter\",\"ctl.in\",\"ctl.out\",\"ctlchan.in\",\"curve\",\"dbtopow\","
  "\"dbtorms\",\"debounce\",\"del\",\"div\",\"drunk\",\"exp\",\"f\",\"float\",\"floor\",\"ftom\",\"gate\","
  "\"graph\",\"hysteresis\",\"i\",\"in\",\"inlet\",\"int\",\"inv\",\"lag\",\"loadbang\",\"log\",\"lp\","
  "\"makenote\",\"map\",\"max\",\"metro\",\"min\",\"mod\",\"mtof\",\"n\",\"neg\",\"not\",\"note.in\",\"note.off\","
  "\"note.on\",\"note.out\",\"note.out.vel\",\"notechan.in\",\"noteoff.out\",\"num\",\"number\",\"or\","
  "\"osc.in\",\"osc.out\",\"pgm.in\",\"pgm.out\",\"polytouch.in\",\"pow\",\"powtodb\",\"print\",\"r\","
  "\"ramp\",\"rand\",\"raw.in\",\"receive\",\"rmstodb\",\"round\",\"s\",\"scale\",\"sel\",\"send\",\"seq\","
  "\"sin\",\"spigot\",\"sqrt\",\"stripnote\",\"tan\",\"toggle\",\"touch.in\",\"touch.out\",\"vel.in\","
  "\"wrap\",\"|\"]}";

// 108 objets.
