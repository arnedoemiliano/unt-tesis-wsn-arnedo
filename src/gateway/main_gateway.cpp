/************************************************************************************************
Copyright (c) 2025, Emiliano Arnedo
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction...
SPDX-License-Identifier: MIT
************************************************************************************************/

/** \brief Gateway con ESP32 basado en ESP-NOW y la biblioteca ZHNetwork
 **
 ** Estructura general del firmware principal. Preparado para integrar funciones específicas como
 ** lectura de sensores, transmisión de datos y multitarea con FreeRTOS.
 **/

// === Includes ================================================================================ //
#include <ZHNetwork.h>
#define PIN_LED 13

// === Constantes y configuraciones iniciales ================================================== //


// === Prototipos de funciones ================================================================= //
void onUnicastReceiving(const char *data, const uint8_t *sender);

// === Variables globales ====================================================================== //
ZHNetwork myNet;

// === Setup =================================================================================== //

void setup() {
    Serial.begin(115200);
    Serial.println();
    myNet.begin("ZHNetwork"); //nombre de la red (debe ser el mismo para todos)
    myNet.setOnUnicastReceivingCallback(onUnicastReceiving); //asignamos la funcion de callback para
                                                           //cuando llega un mensaje (recibiendo)

}

// === Loop ==================================================================================== //

void loop() {
    myNet.maintenance();
}

// === Implementación de funciones ============================================================= //
void onUnicastReceiving(const char *data, const uint8_t *sender)
{
    Serial.print("Unicast message from MAC ");
    Serial.print(myNet.macToString(sender));
    Serial.println(" received.");
    Serial.print("Message: ");
    Serial.println(data);
}

// === Funciones auxiliares internas =========================================================== //
