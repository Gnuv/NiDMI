/**
 * @file SdHandlerRegister.cpp
 * @brief Enregistrement automatique du SdHandler (voir DacHandlerRegister).
 */
#include "SdHandler.h"
#include "../ComplexHandlerRegistry.h"

static ComplexHandler* creerSdHandler() { return new SdHandler(); }

static bool enregistre_sd = ComplexHandlerRegistry::registerHandler("sd_spi", creerSdHandler);
