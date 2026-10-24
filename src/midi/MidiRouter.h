// Routeur vers implémentations RTP-MIDI et OSC existantes
#pragma once

#include <Arduino.h>
#include <vector>
#include "MidiSender.h"
#include "../mapping/MappingEngine.h"   // Etat par pipeline de chaque emplacement
#include "../audio/AudioEngine.h"      // AudioEngine::Pour : a quelle cue va une note (§212)

class MidiRouter : public MidiSender {
public:
    /* LA CHAINE N'A PLUS DE LONGUEUR ECRITE EN DUR.
     *
     * C'etait `MAX_SCRIPTS_MAP = 4` : un nombre choisi au §60 parce qu'il en
     * fallait « plus qu'un ». Il ne mesurait rien — ni une ressource (4
     * emplacements coutent 2,1 ko de .bss sur une carte qui en a 320) ni un
     * besoin. Et il etait FAUX par construction : la bonne longueur de chaine,
     * c'est le nombre de PISTES MAP de la composition, une decision de l'auteur.
     * Une composition a cinq pistes map en perdait une, avec un avertissement
     * pour seule trace.
     *
     * Les emplacements sont donc alloues A LA DEMANDE, un par un. C'est la
     * regle du projet (MESURES §100 puis §101) : dynamique A LA CONFIGURATION,
     * fixe pendant la performance — allouer quand l'app pousse un script est le
     * moment ou la carte peut encore refuser proprement ; allouer pendant le
     * spectacle fragmenterait les ~27 ko contigus dont depend AsyncTCP.
     *
     * PLAFOND n'est pas une longueur : c'est une borne d'INDEX, pour qu'une
     * requete malformee ne demande pas 60 000 emplacements. La vraie limite est
     * la MEMOIRE, verifiee a chaque allocation (voir _assurerEmplacement).
     *
     * Un VECTEUR DE POINTEURS, pas un vecteur d'objets : `Etat etats[]` est
     * pointe par la file des reprises (MappingEngine), et un vecteur qui
     * reallouerait les DEPLACERAIT — exactement la lecture apres liberation
     * corrigee au §101. Chaque emplacement est alloue separement et ne bouge
     * plus jamais. */
    static constexpr uint8_t PLAFOND_SCRIPTS_MAP = 64;

    /* LE PLANCHER DU BLOC CONTIGU. Sous cette taille, la pile reseau accepte les
     * connexions sans pouvoir les servir : un navigateur qui en ouvre trente de
     * front n'obtient rien, et la page reste blanche. Declare ICI parce que
     * c'est ici qu'on refuse d'allouer — et publie par /api/diag/reservoirs,
     * pour que l'app affiche le meme seuil sans en garder une copie. */
    static constexpr size_t PLANCHER_BLOC_CONTIGU = 12288;

    /* Combien la chaine en porte EN CE MOMENT. Publie par /api/midi/scripts :
     * l'app n'a plus a savoir combien la carte en tient, elle le lui demande. */
    uint8_t nEmplacements() const { return (uint8_t)emplacements.size(); }

    /* Dimensionne la chaine. `n` vient des pistes map de la composition.
     * Rend le nombre REELLEMENT obtenu — qui peut etre inferieur si la memoire
     * a manque, et c'est alors la carte qui le dit, pas l'app qui le devine. */
    uint8_t dimensionnerChaine(uint8_t n);

    /* ── LES MAILLONS PERMANENTS : LA ZONE MAIN ────────────────────────────
     *
     * Les `n` PREMIERS maillons appartiennent a la zone MAIN de la composition
     * — des scripts qui tournent TOUT LE TEMPS, quelle que soit la cue. Une cue
     * ne les touche pas : ni pour les charger, ni pour les vider.
     *
     * Sans cette frontiere, un script MAIN n'avait aucun moyen d'exister sur la
     * carte : les cues reecrivent la chaine entiere a chaque activation, et
     * `chargerScriptNomme` REINITIALISE l'etat du maillon. Un counter() ou un
     * toggle() de MAIN serait reparti de zero a chaque GO — ce qui est la
     * negation meme de « toujours actif ».
     *
     * Ils sont EN TETE parce que la chaine est serielle et que le premier
     * maillon voit l'evenement tel qu'il arrive : un traitement global (filtrer
     * un canal, transposer l'ensemble) doit s'appliquer avant ce que la cue en
     * fait. C'est aussi l'ordre visuel — la colonne MAIN est dessinee avant la
     * case 1.
     *
     * ⚠ DIVERGENCE ASSUMEE AVEC LE NAVIGATEUR, et elle est ecrite ici pour
     * qu'on ne la redecouvre pas : cote app, chaque groupe a SA chaine et
     * l'evenement entrant les traverse EN PARALLELE (`AudioEngine.midi` appelle
     * `_routeNoteToGroup` pour chaque groupe, puis pour « mix »). La carte n'a
     * qu'UNE chaine serielle — choix du §60, jamais signale. Tant que la carte
     * n'a qu'un moteur audio, le routage par groupe n'a pas d'objet ici ; le
     * jour ou elle en aura plusieurs, c'est cette difference qu'il faudra
     * trancher, pas ce compteur. */
    void    fixerMaillonsPermanents(uint8_t n);
    uint8_t nMaillonsPermanents() const { return _nPermanents; }

    /* ── LE TRANSPORT GOUVERNE L'HORLOGE DES SCRIPTS ───────────────────────
     *
     * Un script map continuait de tourner APRES un STOP : son metro() battait,
     * ses pipelines emettaient, et rien ne le liait au transport. « Le script
     * de la cue continue a recevoir quand je mets stop. »
     *
     * Ce qui s'arrete, c'est l'HORLOGE — metro(), loadbang(), tout ce qui tire
     * sans qu'un evenement arrive. Le MIDI ENTRANT continue d'etre traite :
     * c'est du jeu live, et la porte de silence de l'audio se rouvre elle aussi
     * a la note suivante apres un STOP (AudioEngine). Arreter le traitement des
     * notes rendrait le clavier muet a l'arret, ce que personne ne demande.
     *
     * Pose par le transport de l'app (/api/audio/resume et /api/audio/stop) ET
     * par le sequenceur embarque (Cues::demarrer / Cues::arreter) : les deux
     * chemins mènent au même drapeau, sinon une carte headless aurait sa propre
     * idee du transport. */
    void fixerTransport(bool enLecture) { _enLecture = enLecture; }
    bool enLecture() const { return _enLecture; }

    /* ── LES SCRIPTS DE LA CUE QUI PART (MESURES §212) ──────────────────────
     * Pendant son Release, la cue qui part se comporte normalement : ses
     * maillons de cue tournent A COTE de ceux de la cue qui arrive — une
     * branche chacune apres les maillons permanents de MAIN, chacune vers les
     * instruments de SA cue (AudioEngine::Pour). Ouvrir : les maillons de cue
     * qui jouaient deviennent la branche sortante, avec leur etat et leurs
     * metro() ; des maillons vides prennent leur place, que la cue qui arrive
     * remplit. Fermer : la branche part (ses reprises purgees). Tout y passe :
     * les notes, les CC, la pression des pads, l'OSC et les horloges — et ce
     * qu'un maillon en fait va aux instruments et aux effets de SA cue. */
    void ouvrirSortie();
    void fermerSortie();
    bool sortieOuverte() const { return !sortants.empty(); }
    String nomsSortantsJson();   // ["a.nms",""] : les maillons de la branche sortante, sous le verrou

    /* ── THE GROUPS (MESURES §221) ──────────────────────────────────────────
     * Each map link belongs to its track's group, and what a group's links make of a note is for
     * that group's instruments only — each group its own walk of the chain, from the note as it
     * came, as in the browser. A link runs once per event, in its group's walk. The cue says the
     * group of every link (sgrp=, by position, MAIN included; 0 when it does not) and of every
     * instrument block (grp=, AudioEngine::setBlockGroups). Without them everything is in group 0:
     * one walk, as before. The CCs still walk the whole chain once; what a link EMITS — a note
     * from a metro(), an OSC message, a del() — goes to its own group. */
    void setLinkGroups(const uint8_t* groups, uint8_t n);
    String linkGroupsJson();     // [0,1,2] : the group of each link, under the lock
    uint8_t groupeCourant() const;
    /* A deferred emission's tag (MappingEngine::onTag): its branch and group, set around it on
     * the task that emits (under the scripts lock); TAG_NONE puts the context back. */
    void emissionDifferee(uint8_t tag);
    static uint8_t tagDe(AudioEngine::Pour pour, uint8_t groupe);

    MidiRouter();
    ~MidiRouter() override;

    void begin() override;

    /* Le RTP-MIDI SORTANT (MESURES §175) : les envois sont deposes, et la
     * boucle les emet juste avant de servir la session — une seule tache
     * touche AppleMIDI. loopTask seulement. `rtpJetes` : ce que la file
     * pleine a jete (256 places ; une boucle arretee des secondes). */
    void emettreRtp();
    static uint32_t rtpJetes();

    void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendControlChange(uint8_t channel, uint8_t control, uint8_t value) override;
    
    // Nouveaux messages MIDI
    void sendProgramChange(uint8_t channel, uint8_t program) override;
    void sendPitchBend(uint8_t channel, int bend) override;
    void sendAftertouch(uint8_t channel, uint8_t pressure) override;
    void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) override;
    void sendClock() override;
    void sendStart() override;
    void sendStop() override;
    void sendContinue() override;

    void enableRtpMidi(bool enabled);
    void enableOsc(bool enabled);
    void enableBluetooth(bool enabled);
    void enableUsbMidi(bool enabled);

    void setOscTargetSta(bool sta);
    void setOscPort(uint16_t port);

    void setMidiChannel(uint8_t channel); // défaut 1
    
    // ── Script .nms de mapping du MIDI ENTRANT ──────────────────────────────
    // TOUT est fait dans la carte : le navigateur POUSSE le script, il ne
    // l'execute jamais. Debranche, la carte continue d'appliquer le meme
    // traitement — c'est la regle du headless.
    void setScriptMidi(const String& script, uint8_t emplacement = 0);

    /* Le battement d'horloge du script map. Appele periodiquement par la tache
     * temps reel : c'est lui qui fait tourner metro(), et qui envoie le Init de
     * loadbang() apres un chargement. Sans lui, un script map ne s'executait
     * QUE sur un MIDI entrant — donc jamais en headless si rien n'entre, et
     * deux scripts ne pouvaient pas se parler par le registre. */
    void battreHorloge(uint32_t maintenant);

    /* Un message OSC entrant, offert aux QUATRE emplacements. Contrairement au
     * CC, ils ne sont pas montes en chaine ici : un message OSC n'est pas
     * transforme de proche en proche, il est ECOUTE — chaque script decide seul
     * s'il repond a cette adresse. */
    void recevoirOsc(const char* adresse, float valeur);


    // Parametres du script, format "cle=valeur;cle=valeur". Un .nms lit ses
    // reglages par r("param","nom",min,max,defaut) : sans eux il retombe sur le
    // DEFAUT et parait ne rien faire — c'est ce qui rendait un bloc transpose
    // silencieux alors que le script etait bien charge.
    void setParamsScript(const String& params);

    // Charge un script PAR NOM depuis storage. persister = memoriser ce nom en
    // NVS pour qu'il revienne au demarrage : la carte redevient autonome, sans
    // qu'un navigateur ait a la reconfigurer. Seul le NOM va en NVS — le
    // contenu vit dans LittleFS, et l'y ecrire a chaque cue ferait payer une
    // ecriture flash, donc un craquement audio (MESURES.md §13).
    bool chargerScriptNomme(const char* nom, bool persister = false, uint8_t emplacement = 0);

    /* LES LECTEURS RENDENT UNE COPIE, prise sous le verrou des scripts (§170).
     * Ils rendaient une reference : le serveur web la recopiait dans sa
     * reponse pendant qu'une autre tache pouvait remplacer le texte. */
    String nomScript() const { return nomEmplacement(0); }
    /* Le CONTENU d'un maillon — ce que la carte execute a cet instant. Publie
     * par /api/midi/script?slot=N : sans lui, on ne pouvait que deviner. */
    String contenuEmplacement(uint8_t e) const {
        MappingEngine::Verrou verrou;
        return (e < emplacements.size()) ? emplacements[e]->contenu : String();
    }
    /* Le nom porte par un emplacement donne — chaine vide s'il est libre OU
     * s'il n'existe pas encore : pour un lecteur, les deux se valent. */
    String nomEmplacement(uint8_t e) const {
        MappingEngine::Verrou verrou;
        return (e < emplacements.size()) ? emplacements[e]->nom : String();
    }

    /* Au boot : la chaine de la composition OUVERTE — sa longueur, sa zone
     * MAIN, les scripts memorises —, lue dans son chain.txt (MESURES §186). Elle
     * vivait en NVS, une seule pour toute la carte ; elle suit maintenant la
     * composition. Appele une fois depuis nidmi_setup. */
    void restaurerScript();
    /* La composition ouverte a change (Repertoire::ouvrir) : la chaine se vide
     * — sans rien ecrire, l'ancienne composition garde la sienne — puis se
     * relit dans la nouvelle. */
    void rechargerChaine();
    String scriptMidi() const { return contenuEmplacement(0); }

    /* ── LE MIDI COMPTE AUSSI POUR LE SILENCE (MESURES §170) ───────────────
     * Une ecriture en flash arrete les deux coeurs jusqu'a ~45 ms (§155) :
     * elle attend donc que rien ne s'entende. Mais « rien » ne voulait dire que
     * la sortie AUDIO — une note arrivee pendant l'ecriture, dans le creux
     * d'une sequence, partait en retard d'autant ; et une carte sans son (un
     * controleur MIDI) ecrivait n'importe quand. Chaque message MIDI, entrant
     * ou sortant, date donc son passage ; AudioEngine::silencePourLaFlash()
     * exige les deux silences. Un mot atomique : rien de plus sur le chemin. */
    static void noterTrafic();
    static uint32_t silenceMidiDepuisMs();

    // Point d'entree UNIQUE de toute note ENTRANTE (USB, clavier de l'app par
    // WebSocket, RTP...) : applique le script s'il y en a un, puis joue.
    // Sans lui, chaque source appelait AudioEngine directement et le script
    // n'aurait porte que sur celles qu'on aurait pense cabler.
    void noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff);

    /* THE OTHER MESSAGES OF A CONTROLLER (§222) — CC, pitch bend, channel pressure, a pad's
     * pressure (polyphonic aftertouch), program change — each through the map scripts of
     * EVERY GROUP apart, from the message as it came, as a note is (§221) and as the browser
     * does (engine._routeCCToGroup, _routePBToGroup…). What a group's chain makes of a CC —
     * or a CC a script makes of another message (`bend.in() : ctl.out(…)`) — goes to that
     * group only: its learnt CCs (CcMap), its SFZ kits. A pitch bend, a pressure or a
     * program that comes out of the chain plays nothing: no instrument of the card reads
     * them, as in the browser — the scripts do (s("sys.cue"), a CC, a registry value). */
    void ccEntrant(uint8_t channel, uint8_t control, uint8_t value);
    void polytouchEntrant(uint8_t channel, uint8_t note, uint8_t pressure);
    void bendEntrant(uint8_t channel, int16_t bend);
    void touchEntrant(uint8_t channel, uint8_t pressure);
    void pgmEntrant(uint8_t channel, uint8_t program);

    /* What group `groupe`'s links (MAIN, then the cue's) make of a CC, without emitting nor
     * learning — the production path, under the same lock; /api/midi/chaine. False if a link
     * swallowed it. */
    bool chaineScriptsCc(uint8_t groupe, uint8_t& canal, uint8_t& cc, uint8_t& valeur);
    /* The groups a message other than a note walks through (§222): those of the instruments
     * that sound, of the links, of the learnt CCs. */
    uint32_t groupesDesMessages();
    /* What an incoming CC does after group `groupe`'s scripts (learn, CcMap, SFZ, components)
     * — for the instruments of the cue `pour` (§212): the branch it came out of. The learning
     * and the components follow the cue that plays only; the components once per CC
     * (`composants`). */
    void appliquerCc(uint8_t canal, uint8_t cc, uint8_t valeur,
                     AudioEngine::Pour pour = AudioEngine::Pour::TOUS,
                     uint8_t groupe = AudioEngine::GROUP_ALL, bool composants = true);
    /* WHOSE SCRIPTS RUN on THIS task now (§212): the branch a link belongs to — what it
     * emits (a note, s("sys.lp_cutoff")) goes to its own cue. Any other task: TOUS. */
    AudioEngine::Pour pourCourant() const;

    /* EVERY CC A GROUP'S SCRIPTS EMIT IS THAT GROUP'S (§225), as in the browser
     * (_applyCCToGroupParamMappings): learnt, applied, read by its kits — a CC made of a note
     * (`note.in() : ctl.out(…)`), of a metro(), a del(), a lag() or an OSC message, not only
     * one that came through the chain. They are set aside while the script runs, under its
     * lock; this applies them, after it (the learning writes to the console and the flash).
     * Called by whoever ran the scripts: the notes, the clock, the deferred emissions, OSC. */
    void appliquerCcEmis();
    uint32_t ccEmisPerdus() const { return _ccEmisPerdus; }   // /api/diag/reservoirs

    // Réception MIDI pour piloter les LEDs
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value);

private:
    /* PLUSIEURS SCRIPTS MAP, en CHAINE.
     *
     * Il n'y en avait qu'un (`scriptEntrant`), si bien qu'une composition a deux
     * pistes map ne pouvait pas exister en headless. La limite n'etait pas dans
     * le stockage — storage tient les fichiers depuis toujours — mais dans le
     * runtime, qui n'en tenait qu'un.
     *
     * SEMANTIQUE : une chaine, pas un parallele. C'est deja ce que le code
     * faisait avec un seul script — il transforme l'evenement, et la suite voit
     * le resultat, pas l'original. Chaque emplacement peut donc transformer a
     * son tour, ou AVALER l'evenement (traite sans emission), ce qui rend un
     * filtre exprimable. Les blocs d'une piste s'enchainent de la meme facon
     * cote app : la carte dit la meme chose.
     *
     * Chaque emplacement a SON etat par pipeline. Sans ca ils partageraient le
     * tableau global du moteur — deux toggle() dans deux scripts differents se
     * marcheraient dessus sans rien dire (meme piege que le §59). */

    struct Emplacement {
        String contenu;        // "" = emplacement vide, ignore
        String nom;            // nom du fichier .nms, ou "(en ligne)"
        /* Le nom MEMORISE pour ce maillon : ce que chain.txt garde, et ce que la
         * carte recharge au demarrage (un maillon MAIN, un script pousse en
         * ligne). Celui qu'une cue y charge n'en est pas : la cue le redonne. */
        String nomMemorise;
        bool   initEnAttente = true;   // un loadbang() est du
        /* A quoi ce maillon peut repondre, et quand son horloge a quelque chose
         * a faire (MESURES §170) : on ne l'execute pas pour rien. Voir
         * MappingEngine, « A QUOI UN SCRIPT REPOND ». Pose avec le contenu. */
        uint16_t repond = 0;
        uint8_t  groupe = 0;              // its track's group, from the cue (§221)
        uint32_t prochainTick = 0;
        bool     tickConnu = false;       // faux : battre au prochain tour
        MappingEngine::Etat etats[MappingEngine::MAX_PIPELINES_SCRIPT];
    };
    /* Alloues un par un, jamais deplaces : voir PLAFOND_SCRIPTS_MAP. */
    std::vector<Emplacement*> emplacements;
    /* La branche de la cue qui part (§212) : ses maillons de cue, rang pour
     * rang ceux qui suivent les permanents. Vide : aucune. */
    std::vector<Emplacement*> sortants;
    /* A qui va ce qu'un maillon EMET (une note, un effet par s("sys.…")) pendant qu'il
     * s'execute : pose autour de chaque branche, SOUS le verrou des scripts — une
     * execution a la fois —, et lu seulement par la tache qui l'a pose (une emission
     * d'une autre tache, un capteur, va partout). */
    TaskHandle_t        _tacheEmission = nullptr;
    AudioEngine::Pour   _pourEmission  = AudioEngine::Pour::TOUS;
    uint8_t             _groupeEmission = AudioEngine::GROUP_ALL;   // and to which group (§221)
    /* The context a deferred emission replaced, given back after it. */
    TaskHandle_t        _tacheAvantDiffere = nullptr;
    AudioEngine::Pour   _pourAvantDiffere  = AudioEngine::Pour::TOUS;
    uint8_t             _groupeAvantDiffere = AudioEngine::GROUP_ALL;
    bool                _differeEnCours = false;
    struct Contexte;
    /* The CCs a link emitted, waiting for appliquerCcEmis (§225). Written under the scripts'
     * lock, by the task that runs them; the same CC of the same group and cue keeps its last
     * value. Beyond kCcEmisMax distinct CCs in one go, a CC's local application is lost
     * (counted) — never its MIDI output. */
    struct CcEmis { uint8_t c, n, v, groupe; AudioEngine::Pour pour; };
    static constexpr uint8_t kCcEmisMax = 16;
    CcEmis   _ccEmis[kCcEmisMax];
    uint8_t  _nCcEmis = 0;
    uint32_t _ccEmisPerdus = 0;
    void _retenirCcEmis(uint8_t c, uint8_t n, uint8_t v, AudioEngine::Pour pour, uint8_t groupe);
    /* A message other than a note (§222): its kind and its fields, as MappingEngine reads
     * them — a: CC number, pressure, program, a pad's note; b: CC value, a pad's pressure;
     * v14: pitch bend. */
    struct Message {
        MappingEngine::Evenement::Type type = MappingEngine::Evenement::Aucun;
        uint8_t c = 1, a = 0, b = 0;
        int16_t v14 = 0;
    };
    /* A CC a link made of another message (`polytouch.in() : ctl.out(…)`). */
    struct CcFait { bool fait = false; uint8_t c = 1, n = 0, v = 0; };
    /* Les maillons [de, a) de `v`, pour la cue `pour`. Note : faux si l'un l'a avalee.
     * OSC : chacun l'entend. */
    bool _passerNote(std::vector<Emplacement*>& v, size_t de, size_t a,
                     uint8_t& c, uint8_t& n, uint8_t& vel, bool estNoteOff, const char* branche,
                     AudioEngine::Pour pour, uint8_t groupe);
    /* Group `groupe`'s links in [de, a) of `v`, for the cue `pour` (§222): each takes the
     * message as the previous one left it. False when the message stops there — a link
     * answered and gave nothing of its kind: it swallowed it, or made a CC of it (`cc`). */
    bool _passerMessage(std::vector<Emplacement*>& v, size_t de, size_t a, Message& m, CcFait& cc,
                        const char* branche, AudioEngine::Pour pour, uint8_t groupe);
    /* Every group's walk, then what comes out of them (§222). */
    void _messageEntrant(const Message& m);
    void _passerOsc(std::vector<Emplacement*>& v, size_t de, size_t a, const char* adresse, float valeur,
                    const char* branche, AudioEngine::Pour pour);
    /* La note jouee — les LEDs appairees, puis les instruments de la cue `pour`. */
    void _jouerNote(uint8_t c, uint8_t n, uint8_t v, bool estNoteOff, AudioEngine::Pour pour, uint8_t groupe,
                    bool leds);
    uint8_t _nPermanents = 0;     // les maillons de la zone MAIN, en tete
    bool    _enLecture   = false; // transport : l'horloge des scripts ne bat que sous PLAY
    /* Rend l'emplacement `e`, en allouant ce qui manque jusqu'a lui. nullptr si
     * l'index depasse le plafond ou si la memoire a manque — dans les deux cas
     * la carte le DIT sur le port serie plutot que d'ecrire dans le vide. */
    Emplacement* _assurerEmplacement(uint8_t e);
    /* Libere les emplacements au-dela de `n`, apres avoir purge leurs reprises :
     * une reprise retient un pointeur sur le texte du script (§101). */
    void _reduireChaine(uint8_t n);
    /* Nomme un maillon apres coup, sous le verrou — s'il existe encore. */
    void _nommer(uint8_t e, const String& nom);
    /* Le nom memorise d'un maillon (vide : plus rien), puis la chaine reecrite.
     * Hors du verrou des scripts : il le prend lui-meme, le temps d'un echange. */
    void _memoriser(uint8_t e, const String& nom);
    /* chain.txt de la composition ouverte, reecrit en entier : quelques
     * dizaines d'octets, au silence (Differe) — une rafale n'ecrit qu'une fois. */
    void _ecrireChaine();
    bool rtpEnabled;
    bool oscEnabled;
    bool bluetoothEnabled;
    bool usbMidiEnabled;
    bool oscToSta;
    uint16_t oscPort;
    uint8_t defaultChannel;
};


