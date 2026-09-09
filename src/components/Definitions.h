#pragma once

/**
 * @file Definitions.h
 * @brief Centralise tous les includes de définitions pour garantir leur enregistrement automatique
 * 
 * Ce fichier centralise tous les includes de définitions pour garantir
 * leur enregistrement automatique via les constructeurs statiques dans les fichiers .cpp.
 * 
 * Inclure ce fichier dans ComponentRegistry.cpp au lieu d'inclure chaque définition individuellement.
 * 
 * Pour ajouter une nouvelle définition :
 * 1. Créer MyComponentDef.h/cpp dans src/components/[famille]/
 * 2. Ajouter #include "[famille]/MyComponentDef.h" ci-dessous
 * 3. L'enregistrement se fera automatiquement via le constructeur statique dans le .cpp
 */

// === FAMILLE BASIC ===
#include "basic/PotentiometerDef.h"
#include "basic/ButtonDef.h"
#include "basic/LedDef.h"
#include "basic/VelostatDef.h"
#include "basic/TouchDef.h"
#include "basic/JoystickDef.h"
#include "audio/DacI2sDef.h"
#include "basic/Joystick3Def.h"

// === FAMILLE MULTIPLEXER ===
#include "multiplexer/MuxDef.h"

// === FAMILLE DISTANCE ===
// Ultrasonique + autres capteurs de distance Seeed/Grove (non implémentés)
#include "basic/UltrasonicDef.h"
#include "distance/IrDistanceSharpDef.h"
#include "distance/LidarTofDef.h"
#include "distance/InductiveProximityDef.h"

// === FAMILLE ENVIRONMENT ===
#include "environment/EnvironmentGenericDef.h"
#include "environment/LightSensorGroveDef.h"
#include "environment/TempHumDhtDef.h"
#include "environment/TempHumI2cDef.h"
#include "environment/BarometerDps310Def.h"
#include "environment/SoilMoistureCapacitiveDef.h"
#include "environment/UvSensorGroveDef.h"

// === FAMILLE MOTION ===
#include "motion/MotionGenericDef.h"
#include "motion/PirMotionDef.h"
#include "motion/Imu6AxisDef.h"
#include "motion/Lis3dhDef.h"
#include "motion/GestureIrDef.h"
#include "motion/RadarDopplerDef.h"

// === FAMILLE COLOR ===
#include "color/ColorGenericDef.h"

// === FAMILLE INTERFACE ===
// On garde uniquement les contrôles qui n’existent pas déjà dans BASIC
// (FSR, encodeurs complexes, joystick...). Boutons et sliders restent dans BASIC.
#include "interface/FsrDef.h"
#include "interface/Mpr121Def.h"
#include "interface/RotaryAngleGroveDef.h"
#include "interface/ThumbJoystickGroveDef.h"

// === FAMILLE ACTUATOR ===
#include "actuator/ActuatorGenericDef.h"
#include "actuator/RelayGroveDef.h"
#include "actuator/BuzzerGroveDef.h"
#include "actuator/VibrationMotorGroveDef.h"
#include "actuator/SolenoidGroveDef.h"

// === FAMILLE SIGNAL ===
// Modules signal/CV : bruit, LFO, oscillateurs, VCA (génération + échantillonnage)
#include "signal/NoiseSamplerDef.h"

// === FAMILLE SCREEN ===
#include "display/DisplayGenericDef.h"
#include "display/BargraphLedGroveDef.h"
#include "display/OledI2cGroveDef.h"
#include "display/Lcd16x2I2cGroveDef.h"
#include "display/FourDigitDisplayGroveDef.h"
#include "display/LedMatrixGroveDef.h"

// Pour ajouter une nouvelle définition :
// #include "[famille]/MyComponentDef.h"
