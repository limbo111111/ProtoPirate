#pragma once
#include <stdint.h>

/**
 * Sendet ein Signal basierend auf der History.
 * @param serial: Die ID des Schlüssels
 * @param counter: Der aktuelle Zählerstand (wird im Code automatisch erhöht)
 * @param btn: Der Button-Code (z.B. 0x02 für Unlock)
 * @param brand: "Ford", "Kia", "Suzuki", "Subaru" oder "VW"
 */
void protopirate_tx_execute(uint32_t serial, uint16_t counter, uint8_t btn, const char* brand);
