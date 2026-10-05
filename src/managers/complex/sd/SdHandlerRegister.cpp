/**
 * @file SdHandlerRegister.cpp
 * @brief Automatic registration of SdHandler (see DacHandlerRegister).
 */
#include "SdHandler.h"
#include "../ComplexHandlerRegistry.h"

static ComplexHandler* createSdHandler() { return new SdHandler(); }

static bool registered_sd = ComplexHandlerRegistry::registerHandler("sd_spi", createSdHandler);
