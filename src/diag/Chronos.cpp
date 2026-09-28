#include "Chronos.h"

/* Voir Chronos.h. */
namespace Chronos {

Chrono horloge, differes, mux, composants, execution, emission, impression, traitementUsb,
       retardCue, applicationCue;

void razTout() {
  horloge.raz(); differes.raz(); mux.raz(); composants.raz();
  execution.raz(); emission.raz(); impression.raz(); traitementUsb.raz();
  retardCue.raz(); applicationCue.raz();
}

}  // namespace Chronos
