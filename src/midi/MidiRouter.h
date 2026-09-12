// Routeur vers implémentations RTP-MIDI et OSC existantes
#pragma once

#include <Arduino.h>
#include <vector>
#include "MidiSender.h"
#include "../mapping/MappingEngine.h"   // Etat par pipeline de chaque emplacement

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

    MidiRouter();
    ~MidiRouter() override;

    void begin() override;
    void update() override;

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

    // Charge un script PAR NOM depuis mapfs. persister = memoriser ce nom en
    // NVS pour qu'il revienne au demarrage : la carte redevient autonome, sans
    // qu'un navigateur ait a la reconfigurer. Seul le NOM va en NVS — le
    // contenu vit dans LittleFS, et l'y ecrire a chaque cue ferait payer une
    // ecriture flash, donc un craquement audio (MESURES.md §13).
    bool chargerScriptNomme(const char* nom, bool persister = false, uint8_t emplacement = 0);
    const String& nomScript() const { return nomEmplacement(0); }
    /* Le CONTENU d'un maillon — ce que la carte execute a cet instant. Publie
     * par /api/midi/script?slot=N : sans lui, on ne pouvait que deviner. */
    const String& contenuEmplacement(uint8_t e) const {
        static const String vide;
        return (e < emplacements.size()) ? emplacements[e]->contenu : vide;
    }
    /* Le nom porte par un emplacement donne — chaine vide s'il est libre OU
     * s'il n'existe pas encore : pour un lecteur, les deux se valent. */
    const String& nomEmplacement(uint8_t e) const {
        static const String vide;
        return (e < emplacements.size()) ? emplacements[e]->nom : vide;
    }

    // Au boot : recharge le script memorise. Appele une fois depuis nidmi_setup.
    void restaurerScript();
    const String& scriptMidi() const {
        static const String vide;
        return emplacements.empty() ? vide : emplacements[0]->contenu;
    }

    // Point d'entree UNIQUE de toute note ENTRANTE (USB, clavier de l'app par
    // WebSocket, RTP...) : applique le script s'il y en a un, puis joue.
    // Sans lui, chaque source appelait AudioEngine directement et le script
    // n'aurait porte que sur celles qu'on aurait pense cabler.
    void noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff);

    // Meme role pour les CONTROLEURS CONTINUS. Un CC entrant n'allait qu'aux
    // composants (LEDs) : rien ne le reliait a un parametre, donc le CC learn
    // n'existait que dans le navigateur et mourait avec lui. Il passe
    // desormais par la table CcMap — apprentissage compris — avant d'aller
    // aux composants comme avant.
    void ccEntrant(uint8_t channel, uint8_t control, uint8_t value);

    /* La chaine des scripts map appliquee a un CC, sans emission ni
     * apprentissage : ce que les emplacements font de (canal, cc, valeur).
     * Rend false si l'un d'eux a avale l'evenement. Employee par ccEntrant ET
     * par /api/midi/chaine — un seul code, donc un essai qui prouve la
     * production. */
    bool chaineScriptsCc(uint8_t& canal, uint8_t& cc, uint8_t& valeur);

    // Réception MIDI pour piloter les LEDs
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value);

private:
    /* PLUSIEURS SCRIPTS MAP, en CHAINE.
     *
     * Il n'y en avait qu'un (`scriptEntrant`), si bien qu'une composition a deux
     * pistes map ne pouvait pas exister en headless. La limite n'etait pas dans
     * le stockage — mapfs tient les fichiers depuis toujours — mais dans le
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
        bool   initEnAttente = true;   // un loadbang() est du
        MappingEngine::Etat etats[MappingEngine::MAX_PIPELINES_SCRIPT];
    };
    /* Alloues un par un, jamais deplaces : voir PLAFOND_SCRIPTS_MAP. */
    std::vector<Emplacement*> emplacements;
    uint8_t _nPermanents = 0;     // les maillons de la zone MAIN, en tete
    bool    _enLecture   = false; // transport : l'horloge des scripts ne bat que sous PLAY
    /* Rend l'emplacement `e`, en allouant ce qui manque jusqu'a lui. nullptr si
     * l'index depasse le plafond ou si la memoire a manque — dans les deux cas
     * la carte le DIT sur le port serie plutot que d'ecrire dans le vide. */
    Emplacement* _assurerEmplacement(uint8_t e);
    /* Libere les emplacements au-dela de `n`, apres avoir purge leurs reprises :
     * une reprise retient un pointeur sur le texte du script (§101). */
    void _reduireChaine(uint8_t n);
    bool rtpEnabled;
    bool oscEnabled;
    bool bluetoothEnabled;
    bool usbMidiEnabled;
    bool oscToSta;
    uint16_t oscPort;
    uint8_t defaultChannel;
};


