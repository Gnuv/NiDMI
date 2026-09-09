/**
 * @file DacHandlerRegister.cpp
 * @brief Enregistrement automatique du DacHandler (voir JoystickHandlerRegister).
 */
#include "DacHandler.h"
#include "../ComplexHandlerRegistry.h"

static ComplexHandler* creerDacHandler() { return new DacHandler(); }

static bool enregistre_dac = ComplexHandlerRegistry::registerHandler("dac_i2s", creerDacHandler);
