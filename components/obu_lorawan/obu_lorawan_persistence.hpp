#pragma once

#include <stdbool.h>

#include <RadioLib.h>
#include "esp_err.h"

/**
 * Initialize the default ESP-IDF NVS partition and open the private
 * BicycleOBU LoRaWAN namespace. This must succeed before LoRaWAN TX is
 * allowed so nonce/frame-counter persistence is never silently bypassed.
 */
esp_err_t obu_lorawan_persistence_prepare(void);

/**
 * Attach RadioLib persistence/session callbacks to one LoRaWANNode and restore
 * any compatible state already present in NVS. Call after beginOTAA(), because
 * RadioLib computes the credential checksum there before validating restored
 * persistence.
 */
esp_err_t obu_lorawan_persistence_attach(LoRaWANNode *node,
                                           bool *restored_persistence,
                                           bool *restored_session);

/** Return false after any NVS read/write/commit failure. TX must fail closed. */
bool obu_lorawan_persistence_healthy(void);
