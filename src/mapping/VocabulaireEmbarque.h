// src/mapping/VocabulaireEmbarque.h — GENERE, NE PAS EDITER A LA MAIN.
//
//     python3 scripts/generer-vocabulaire.py
//
// CE QUE CETTE CARTE-CI EXECUTE du langage .nms, derive de MappingEngine.cpp ;
// et ses fonctions pour les scripts — "sys" : ce que s("sys.<nom>") commande,
// ce que r("sys.<nom>") relit —, derivees de NiDMI.cpp (MESURES §155).
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
  "\"wrap\",\"|\"],\"sys\":{\"s\":[\"sys.wifi\",\"sys.standalone\",\"sys.cablefirst\",\"sys.reconnect\"],"
  "\"r\":[\"sys.wifi\",\"sys.cable\",\"sys.standalone\",\"sys.cablefirst\"]}}";

// Ce que s("sys.<nom>") commande, pour le message qu'une faute de nom fait
// ecrire a la carte (NiDMI.cpp) : la meme liste, pas une recopie.
#define VOCABULAIRE_SYS_COMMANDES "sys.wifi, sys.standalone, sys.cablefirst, sys.reconnect"

// 108 objets ; s() commande sys.wifi, sys.standalone, sys.cablefirst, sys.reconnect ; r() relit sys.wifi, sys.cable, sys.standalone, sys.cablefirst.
