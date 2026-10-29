#pragma once
// src/config/Stockage.h — LA PARTITION DE FICHIERS, NOMMEE UNE FOIS
// (CONVERGENCE_NIDMI.md §9.2, MESURES §185).
//
// Tout ce qui est un fichier sur la carte vit dans UNE partition LittleFS :
// les compositions, l'interface (ses scripts de broche, ses sauvegardes), les
// sons. Elle s'appelait « mapfs » (« scripts de mapping ») et partageait la
// flash avec « seqfs » (jamais lue) et « coredump » (plus ecrite depuis le
// §163) : les trois n'en font plus qu'une, `storage`, 1,56 Mo — le nom d'usage
// d'une partition de fichiers dans les exemples d'ESP-IDF.
//
// Le nom etait recopie dans six fichiers : une copie finit par diverger. Il
// est ici, et nulle part ailleurs ; la table (tools/nidmi_s3_ota.csv) dit le
// meme.
namespace Stockage {
constexpr const char* PARTITION = "storage";    // le nom dans la table de partitions
constexpr const char* BASE      = "/storage";   // son point de montage ; les chemins montres commencent apres
}  // namespace Stockage
