#!/usr/bin/env python3
# scripts/generer-vocabulaire.py — genere src/mapping/VocabulaireEmbarque.h
#
# POURQUOI CE SCRIPT EXISTE.
#
# L'app Qseq colorie les scripts de broche et previent quand un objet « n'est
# pas execute par la carte ». Pour cela elle avait besoin de savoir ce que la
# carte execute — et elle le SAVAIT EN DUR : une liste de dix noms ecrite a la
# main dans son generateur de vocabulaire, figee au temps ou le firmware n'en
# executait que dix. Le fichier genere avait ensuite ete corrige a la main (92
# entrees), si bien que relancer le generateur ANNULAIT la correction. Resultat
# mesure : l'editeur signalait `in` et `graph` comme inexistants alors que la
# carte les executait parfaitement.
#
# Le correctif intermediaire faisait lire ce fichier-ci par le depot de l'app,
# a travers un chemin vers CE depot. Ca marche sur deux machines et nulle part
# ailleurs — et surtout, ca reste une supposition : l'app decrivait un firmware
# de reference, pas LA carte branchee, qui peut tourner une version plus
# ancienne.
#
# Donc : LA CARTE DIT ELLE-MEME CE QU'ELLE EXECUTE. Ce script lit le moteur
# d'a cote et en tire la reponse que la route /api/mapping/vocabulaire servira.
# La derivation ne franchit plus de frontiere de depot : la source lue et le
# fichier ecrit sont voisins, et versionnes ensemble.
#
#     python3 scripts/generer-vocabulaire.py
#
# Appele par scripts/nidmi.sh avant chaque build. Voir CONVERGENCE_NIDMI.md §9.6.

import re, sys, pathlib

RACINE  = pathlib.Path(__file__).resolve().parent.parent
MOTEUR  = RACINE / "src/mapping/MappingEngine.cpp"
CARTE   = RACINE / "src/NiDMI.cpp"
SORTIE  = RACINE / "src/mapping/VocabulaireEmbarque.h"

# Le moteur reconnait ses objets de DEUX facons, et de deux seulement :
#     verbe(seg, "nom", args)        — la grande majorite
#     seg == "nom()"                 — les objets sans argument
# Toute troisieme forme qui apparaitrait un jour echapperait a ce script ; le
# controle croise en fin de fichier est la pour la rendre visible.
FORME_VERBE = re.compile(r'verbe\(seg,\s*"([^"]+)"')
FORME_EGAL  = re.compile(r'seg\s*==\s*"([^"]+?)(?:\(\))?"')

def noms_du_moteur(source):
    noms = set()
    for m in FORME_VERBE.finditer(source): noms.add(m.group(1))
    for m in FORME_EGAL.finditer(source):  noms.add(m.group(1))
    return sorted(noms)

# LES FONCTIONS DE LA CARTE (MESURES §155). s("sys.<nom>") commande la carte,
# r("sys.<nom>") relit son etat. Les deux listes ne sont pas les memes —
# sys.reconnect se commande et ne se lit pas, sys.cable se lit et ne se
# commande pas — et chacune vit a UN endroit de NiDMI.cpp :
#     nidmi_sys_recevoir()  strcmp(nom, "sys.<nom>")              ce que s() commande
#     publierEtatsSys()     FluxRegistry::update("sys.<nom>", …)  ce que r() relit
# On les lit la, dans le corps de ces deux fonctions et nulle part ailleurs :
# un nom cite dans un commentaire n'est pas une fonction.
FORME_SYS_ENVOI   = re.compile(r'strcmp\(nom,\s*"(sys\.[^"]+)"\)')
FORME_SYS_LECTURE = re.compile(r'FluxRegistry::update\("(sys\.[^"]+)"')

def corps_de(source, signature):
    """Le corps, accolades comprises, de la definition qui suit `signature`."""
    i = source.find(signature)
    if i < 0: return None
    j = source.find("{", i)
    profondeur = 0
    for k in range(j, len(source)):
        if source[k] == "{": profondeur += 1
        elif source[k] == "}":
            profondeur -= 1
            if profondeur == 0: return source[j:k + 1]
    return None

def dans_l_ordre(motif, texte):
    # L'ordre du source, pas l'ordre alphabetique : c'est l'ordre dans lequel
    # la carte les decrit, et l'editeur les cite ainsi.
    vus = []
    for m in motif.finditer(texte):
        if m.group(1) not in vus: vus.append(m.group(1))
    return vus

def fonctions_de_la_carte(source):
    envoi   = corps_de(source, "void nidmi_sys_recevoir(")
    lecture = corps_de(source, "static void publierEtatsSys(")
    if envoi is None or lecture is None: return None
    return dans_l_ordre(FORME_SYS_ENVOI, envoi), dans_l_ordre(FORME_SYS_LECTURE, lecture)

def echappe(n):
    return n.replace('\\', '\\\\').replace('"', '\\"')

def main():
    if not MOTEUR.exists():
        print(f"✗ moteur introuvable : {MOTEUR}", file=sys.stderr)
        return 1
    source = MOTEUR.read_text(encoding="utf-8", errors="replace")
    noms   = noms_du_moteur(source)
    if not noms:
        print("✗ aucun objet extrait — la forme de reconnaissance a change ?", file=sys.stderr)
        return 1
    if not CARTE.exists():
        print(f"✗ carte introuvable : {CARTE}", file=sys.stderr)
        return 1
    sysfn = fonctions_de_la_carte(CARTE.read_text(encoding="utf-8", errors="replace"))
    if not sysfn or not sysfn[0] or not sysfn[1]:
        print("✗ fonctions sys.* introuvables dans NiDMI.cpp — nidmi_sys_recevoir() ou "
              "publierEtatsSys() a change de forme ?", file=sys.stderr)
        return 1
    sys_s, sys_r = sysfn

    # La reponse est ecrite ICI, une fois, et vit en flash. La route n'a plus
    # qu'a la servir : pas de String construite a chaque requete, donc pas un
    # octet de tas pris a AsyncTCP — c'est le bloc contigu le plus grand qui
    # conditionne le serveur, et il est deja la ressource rare.
    objets = ",".join(f'"{echappe(n)}"' for n in noms)
    liste  = lambda l: ",".join(f'"{echappe(n)}"' for n in l)
    corps  = (f'{{"n":{len(noms)},"objets":[{objets}],'
              f'"sys":{{"s":[{liste(sys_s)}],"r":[{liste(sys_r)}]}}}}')

    # Decoupe en lignes de source lisibles (~90 colonnes) sans couper un nom.
    lignes, courante = [], ""
    for morceau in corps.replace('},', '},\x00').split('\x00'):
        for bout in re.findall(r'.{1,86}(?:,|$)', morceau):
            lignes.append(bout)
    litteral = "\n".join(f'  "{l.replace(chr(92), chr(92)*2).replace(chr(34), chr(92)+chr(34))}"'
                         for l in lignes if l)

    SORTIE.write_text(f'''// src/mapping/VocabulaireEmbarque.h — GENERE, NE PAS EDITER A LA MAIN.
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
{litteral};

// Ce que s("sys.<nom>") commande, pour le message qu'une faute de nom fait
// ecrire a la carte (NiDMI.cpp) : la meme liste, pas une recopie.
#define VOCABULAIRE_SYS_COMMANDES "{", ".join(sys_s)}"

// {len(noms)} objets ; s() commande {", ".join(sys_s)} ; r() relit {", ".join(sys_r)}.
''', encoding="utf-8")

    print(f"✓ {len(noms)} objets, {len(sys_s)} + {len(sys_r)} fonctions sys "
          f"→ src/mapping/VocabulaireEmbarque.h ({len(corps)} octets servis)")
    return 0

sys.exit(main())
