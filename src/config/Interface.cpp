#include "Interface.h"
#include "EcrituresDifferees.h"
#include "../mapping/Repertoire.h"
#include "../audio/AudioEngine.h"

#include <Preferences.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <inttypes.h>

namespace Interface {
namespace {

// Les espaces de la NVS qui SONT l'interface, et ce qui, dedans, n'en est pas.
constexpr const char* ESPACES[] = { "nidmi", "nidmi-cues" };
constexpr const char* HORS[]    = { "compo", "instrument", "interface", "nvs_schema" };   // de « nidmi »
constexpr const char* SECRET    = "sta_pass";                                              // de « nidmi »
constexpr const char* NVS_ESPACE = "nidmi";
constexpr const char* NVS_CLE    = "interface";

bool gere(const char* espace) {
  for (const char* e : ESPACES) if (!strcmp(e, espace)) return true;
  return false;
}
bool hors(const char* espace, const char* cle) {
  if (strcmp(espace, "nidmi")) return false;
  for (const char* h : HORS) if (!strcmp(h, cle)) return true;
  return false;
}
bool secret(const char* espace, const char* cle) { return !strcmp(espace, "nidmi") && !strcmp(cle, SECRET); }

struct Type { nvs_type_t t; const char* nom; };
constexpr Type TYPES[] = {
  { NVS_TYPE_U8, "u8" },   { NVS_TYPE_I8, "i8" },   { NVS_TYPE_U16, "u16" }, { NVS_TYPE_I16, "i16" },
  { NVS_TYPE_U32, "u32" }, { NVS_TYPE_I32, "i32" }, { NVS_TYPE_U64, "u64" }, { NVS_TYPE_I64, "i64" },
  { NVS_TYPE_STR, "str" }, { NVS_TYPE_BLOB, "blob" },
};
const char* nomDuType(nvs_type_t t) {
  for (const Type& x : TYPES) if (x.t == t) return x.nom;
  return nullptr;
}
bool typeDuNom(const char* n, size_t k, nvs_type_t& t) {
  for (const Type& x : TYPES) if (strlen(x.nom) == k && !strncmp(x.nom, n, k)) { t = x.t; return true; }
  return false;
}

// Tout ce qui est gros se prend en PSRAM : la carte redemarre ensuite, mais le
// tas interne doit tenir jusque-la.
char* psram(size_t n) { return (char*)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* Un reglage en texte (PSRAM, a liberer) : nombre en decimal, texte tel quel,
 * octets en hexadecimal. nullptr : illisible. */
char* lireEnTexte(nvs_handle_t h, const char* cle, nvs_type_t t) {
  char nb[24];
  switch (t) {
    case NVS_TYPE_U8:  { uint8_t v;  if (nvs_get_u8(h, cle, &v))  return nullptr; snprintf(nb, sizeof nb, "%u", (unsigned)v); break; }
    case NVS_TYPE_I8:  { int8_t v;   if (nvs_get_i8(h, cle, &v))  return nullptr; snprintf(nb, sizeof nb, "%d", (int)v); break; }
    case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%u", (unsigned)v); break; }
    case NVS_TYPE_I16: { int16_t v;  if (nvs_get_i16(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%d", (int)v); break; }
    case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%" PRIu32, v); break; }
    case NVS_TYPE_I32: { int32_t v;  if (nvs_get_i32(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%" PRId32, v); break; }
    case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%" PRIu64, v); break; }
    case NVS_TYPE_I64: { int64_t v;  if (nvs_get_i64(h, cle, &v)) return nullptr; snprintf(nb, sizeof nb, "%" PRId64, v); break; }
    case NVS_TYPE_STR: {
      size_t n = 0;
      if (nvs_get_str(h, cle, nullptr, &n)) return nullptr;
      char* p = psram(n);
      if (!p || nvs_get_str(h, cle, p, &n)) { heap_caps_free(p); return nullptr; }
      return p;
    }
    case NVS_TYPE_BLOB: {
      size_t n = 0;
      if (nvs_get_blob(h, cle, nullptr, &n)) return nullptr;
      uint8_t* b = (uint8_t*)psram(n);
      char* p = psram(2 * n + 1);
      if (!b || !p || nvs_get_blob(h, cle, b, &n)) { heap_caps_free(b); heap_caps_free(p); return nullptr; }
      for (size_t i = 0; i < n; i++) snprintf(p + 2 * i, 3, "%02x", b[i]);
      p[2 * n] = 0;
      heap_caps_free(b);
      return p;
    }
    default: return nullptr;
  }
  char* p = psram(strlen(nb) + 1);
  if (p) strcpy(p, nb);
  return p;
}

int chiffreHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decoder une valeur de la ligne dans `out` (au moins k + 1 octets) : un texte
 * %-encode, des octets en hexadecimal, un nombre tel quel. Rend la longueur, -1
 * si elle est mal formee. `out` est termine par un zero. */
long decoder(nvs_type_t t, const char* v, size_t k, char* out) {
  size_t n = 0;
  if (t == NVS_TYPE_BLOB) {
    if (k % 2) return -1;
    for (size_t i = 0; i < k; i += 2) {
      const int a = chiffreHex(v[i]), b = chiffreHex(v[i + 1]);
      if (a < 0 || b < 0) return -1;
      out[n++] = (char)((a << 4) | b);
    }
  } else {
    for (size_t i = 0; i < k; i++) {
      if (v[i] != '%') { out[n++] = v[i]; continue; }
      if (i + 2 >= k) return -1;                    // « % » sans ses deux chiffres
      const int a = chiffreHex(v[i + 1]), b = chiffreHex(v[i + 2]);
      if (a < 0 || b < 0) return -1;
      out[n++] = (char)((a << 4) | b);
      i += 2;
    }
  }
  out[n] = 0;
  return (long)n;
}

/* Un nombre du type voulu, dans ses bornes. */
bool nombre(nvs_type_t t, const char* s, int64_t& si, uint64_t& su) {
  if (!*s) return false;
  char* fin = nullptr;
  const bool signe = (t == NVS_TYPE_I8 || t == NVS_TYPE_I16 || t == NVS_TYPE_I32 || t == NVS_TYPE_I64);
  if (signe) { si = strtoll(s, &fin, 10); } else { if (*s == '-') return false; su = strtoull(s, &fin, 10); }
  if (!fin || *fin) return false;
  switch (t) {
    case NVS_TYPE_U8:  return su <= 0xFF;
    case NVS_TYPE_U16: return su <= 0xFFFF;
    case NVS_TYPE_U32: return su <= 0xFFFFFFFFull;
    case NVS_TYPE_I8:  return si >= -128 && si <= 127;
    case NVS_TYPE_I16: return si >= -32768 && si <= 32767;
    case NVS_TYPE_I32: return si >= INT32_MIN && si <= INT32_MAX;
    default:           return true;
  }
}

esp_err_t poser(nvs_handle_t h, const char* cle, nvs_type_t t, const char* v, size_t n) {
  int64_t si = 0; uint64_t su = 0;
  if (t != NVS_TYPE_STR && t != NVS_TYPE_BLOB && !nombre(t, v, si, su)) return ESP_ERR_INVALID_ARG;
  switch (t) {
    case NVS_TYPE_U8:   return nvs_set_u8(h, cle, (uint8_t)su);
    case NVS_TYPE_I8:   return nvs_set_i8(h, cle, (int8_t)si);
    case NVS_TYPE_U16:  return nvs_set_u16(h, cle, (uint16_t)su);
    case NVS_TYPE_I16:  return nvs_set_i16(h, cle, (int16_t)si);
    case NVS_TYPE_U32:  return nvs_set_u32(h, cle, (uint32_t)su);
    case NVS_TYPE_I32:  return nvs_set_i32(h, cle, (int32_t)si);
    case NVS_TYPE_U64:  return nvs_set_u64(h, cle, su);
    case NVS_TYPE_I64:  return nvs_set_i64(h, cle, si);
    case NVS_TYPE_STR:  return nvs_set_str(h, cle, v);
    case NVS_TYPE_BLOB: return nvs_set_blob(h, cle, v, n);
    default:            return ESP_ERR_INVALID_ARG;
  }
}

/* Une ligne « espace|cle|type|valeur », decoupee sans copie. */
struct Ligne {
  const char* espace; size_t le;
  const char* cle;    size_t lc;
  const char* type;   size_t lt;
  const char* valeur; size_t lv;
};
bool decouper(const char* d, size_t k, Ligne& l) {
  size_t pos[3]; int trouves = 0;
  for (size_t i = 0; i < k && trouves < 3; i++) if (d[i] == '|') pos[trouves++] = i;
  if (trouves < 3) return false;
  l.espace = d;                   l.le = pos[0];
  l.cle    = d + pos[0] + 1;      l.lc = pos[1] - pos[0] - 1;
  l.type   = d + pos[1] + 1;      l.lt = pos[2] - pos[1] - 1;
  l.valeur = d + pos[2] + 1;      l.lv = k - pos[2] - 1;
  return true;
}

/* Chaque ligne utile du texte : `f(numero, ligne)` ; false l'arrete. */
template <typename F> void chaqueLigne(const char* t, size_t n, F f) {
  size_t debut = 0; int numero = 0;
  while (debut < n) {
    size_t fin = debut;
    while (fin < n && t[fin] != '\n') fin++;
    size_t k = fin - debut;
    if (k && t[debut + k - 1] == '\r') k--;
    numero++;
    if (k && t[debut] != '#' && !f(numero, t + debut, k)) return;
    debut = fin + 1;
  }
}

StaticSemaphore_t _tampon;
SemaphoreHandle_t _verrou = xSemaphoreCreateMutexStatic(&_tampon);
struct Verrou {
  Verrou()  { xSemaphoreTake(_verrou, portMAX_DELAY); }
  ~Verrou() { xSemaphoreGive(_verrou); }
};
String _nom;
bool   _nomLu = false;

}  // namespace

String nom() {
  Verrou v;
  if (!_nomLu) {
    Preferences p;
    if (p.begin(NVS_ESPACE, true)) { _nom = p.getString(NVS_CLE, ""); p.end(); }
    _nomLu = true;
  }
  return _nom;
}

bool renommer(const String& n, String& raison) {
  if (n.length() && !Repertoire::nomValide(n, raison)) return false;
  nom();                                             // l'ancien, lu une fois
  {
    Verrou v;
    if (_nom == n) return true;
    _nom = n;
  }
  if (n.length()) Differe::nvsChaine(NVS_ESPACE, NVS_CLE, n);   // au silence (§157)
  else            Differe::nvsRetirer(NVS_ESPACE, NVS_CLE);
  return true;
}

bool visiter(bool secrets, Visiteur voir, void* ctx, String& tus) {
  /* CE QUE LA CARTE PORTE : un reglage change a l'instant attend peut-etre le
   * silence pour aller en NVS. Enregistrer une interface est un geste de
   * preparation : ce qui attend part d'abord (annonce au moteur, §157). */
  Differe::nvsEcrireMaintenant();
  bool ok = true;
  for (const char* espace : ESPACES) {
    nvs_handle_t h;
    if (nvs_open(espace, NVS_READONLY, &h) != ESP_OK) continue;        // espace jamais ecrit : rien
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, espace, NVS_TYPE_ANY, &it);
    while (r == ESP_OK) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      const char* type = nomDuType(info.type);
      if (type && !hors(espace, info.key)) {
        if (secret(espace, info.key) && !secrets) {
          if (tus.length()) tus += ",";
          tus += info.key;
        } else {
          char* v = lireEnTexte(h, info.key, info.type);
          if (v) { voir(espace, info.key, type, v, ctx); heap_caps_free(v); }
          else ok = false;
        }
      }
      r = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    nvs_close(h);
  }
  return ok;
}

bool appliquer(const char* texte, size_t n, const String& nomNouveau, String& raison,
               int& poses, int& retires) {
  poses = retires = 0;
  if (nomNouveau.length() && !Repertoire::nomValide(nomNouveau, raison)) return false;
  /* Le tampon de decodage : aucune valeur n'est plus longue que sa ligne. */
  char* scratch = psram(n + 1);
  if (!scratch) { raison = "pas de place en PSRAM"; return false; }
  struct Liberer { char* p; ~Liberer() { heap_caps_free(p); } } liberer{ scratch };

  // 1. TOUT VERIFIER, avant d'ecrire quoi que ce soit.
  bool bon = true;
  int lignes = 0;
  chaqueLigne(texte, n, [&](int numero, const char* d, size_t k) {
    Ligne l;
    nvs_type_t t;
    char espace[16], cle[16];
    if (!decouper(d, k, l)) { raison = "ligne " + String(numero) + " : quatre champs attendus"; return bon = false; }
    if (!l.le || l.le >= sizeof espace || !l.lc || l.lc >= sizeof cle) {
      raison = "ligne " + String(numero) + " : espace ou cle trop longs (15 au plus)"; return bon = false;
    }
    memcpy(espace, l.espace, l.le); espace[l.le] = 0;
    memcpy(cle, l.cle, l.lc); cle[l.lc] = 0;
    if (!gere(espace)) { raison = "ligne " + String(numero) + " : « " + espace + " » n'est pas un espace de l'interface"; return bon = false; }
    if (hors(espace, cle)) { raison = "ligne " + String(numero) + " : « " + cle + " » n'est pas un reglage de l'interface"; return bon = false; }
    if (!typeDuNom(l.type, l.lt, t)) { raison = "ligne " + String(numero) + " : type inconnu"; return bon = false; }
    const long k2 = decoder(t, l.valeur, l.lv, scratch);
    int64_t si; uint64_t su;
    if (k2 < 0 || (t != NVS_TYPE_STR && t != NVS_TYPE_BLOB && !nombre(t, scratch, si, su))) {
      raison = "ligne " + String(numero) + " : valeur mal formee pour « " + cle + " »"; return bon = false;
    }
    lignes++;
    return true;
  });
  if (!bon) return false;

  /* Ce qui attendait le silence part d'abord : ecrit APRES le document — au
   * redemarrage —, un reglage en attente l'aurait defait. */
  Differe::nvsEcrireMaintenant();
  // 2. POSER, espace par espace ; retenir ce que le document porte.
  AudioEngine::ecritureFlashDebut();
  String portes = "\n";                               // « espace|cle\n » pour chaque reglage pose
  bool ok = true;
  for (const char* espace : ESPACES) {
    nvs_handle_t h;
    if (nvs_open(espace, NVS_READWRITE, &h) != ESP_OK) { ok = false; continue; }
    chaqueLigne(texte, n, [&](int, const char* d, size_t k) {
      Ligne l;
      decouper(d, k, l);
      if (l.le != strlen(espace) || strncmp(l.espace, espace, l.le)) return true;
      char cle[16]; memcpy(cle, l.cle, l.lc); cle[l.lc] = 0;
      nvs_type_t t; typeDuNom(l.type, l.lt, t);
      const long k2 = decoder(t, l.valeur, l.lv, scratch);
      /* Un reglage d'un autre type sous la meme cle : NVS refuse de le changer
       * de type en place — on retire l'ancien d'abord. */
      esp_err_t e = poser(h, cle, t, scratch, (size_t)k2);
      if (e == ESP_ERR_NVS_TYPE_MISMATCH) { nvs_erase_key(h, cle); e = poser(h, cle, t, scratch, (size_t)k2); }
      if (e == ESP_OK) { poses++; portes += String(espace) + "|" + cle + "\n"; }
      else { ok = false; raison = String("« ") + cle + " » : ecriture refusee (" + esp_err_to_name(e) + ")"; }
      return true;
    });
    // 3. RETIRER ce que le document ne porte pas — sauf ce qui n'est pas
    //    l'interface, et le mot de passe WiFi.
    String aRetirer;
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, espace, NVS_TYPE_ANY, &it);
    while (r == ESP_OK) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      if (!hors(espace, info.key) && !secret(espace, info.key)
          && portes.indexOf("\n" + String(espace) + "|" + info.key + "\n") < 0)
        aRetirer += String(info.key) + "\n";
      r = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    int d = 0;
    while (d < (int)aRetirer.length()) {
      const int f = aRetirer.indexOf('\n', d);
      const String cle = aRetirer.substring(d, f);
      if (nvs_erase_key(h, cle.c_str()) == ESP_OK) retires++;
      d = f + 1;
    }
    // Son nom : le fichier le porte (§9.7).
    if (!strcmp(espace, NVS_ESPACE)) {
      if (nomNouveau.length()) nvs_set_str(h, NVS_CLE, nomNouveau.c_str());
      else nvs_erase_key(h, NVS_CLE);
    }
    if (nvs_commit(h) != ESP_OK) ok = false;
    nvs_close(h);
  }
  AudioEngine::ecritureFlashFin();
  { Verrou v; _nom = nomNouveau; _nomLu = true; }
  (void)lignes;
  return ok;
}

}  // namespace Interface
