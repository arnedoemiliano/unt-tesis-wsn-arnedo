/************************************************************************************************
Copyright (c) 2025, Emiliano Arnedo
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction...
SPDX-License-Identifier: MIT
************************************************************************************************/

/** \brief Nodo ESP32 basado en ESP-NOW y la biblioteca ZHNetwork
 **
 ** Estructura general del firmware principal. Preparado para integrar funciones específicas como
 ** lectura de sensores, transmisión de datos y multitarea con FreeRTOS.
 **/
// === Includes =============================================================================== //
#include "ZHNetwork.h"

// === Constantes y configuraciones iniciales ================================================= //

ZHNetwork myNet;

uint64_t messageLastTime{ 0 };  // contador para comparar
uint16_t messageTimerDelay{ 1000 }; //cada cuanto se envia un mensaje
const uint8_t target[6]{ 0xA4, 0xCF, 0x12, 0x05, 0x1B, 0x64 };  //mac del gateway
String msg = "";
//String espMAC; //mac local (WIFI_STA) del micro para testing


// === Prototipos de funciones ================================================================= //
// función de callback para cuando llega o no una confirmación de mensaje recibido (transmisor)
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);
//funcion de callback para cuando se recibe un mensaje (receptor)
void onUnicastReceiving(const char *data, const uint8_t *sender);

// === Variables globales ====================================================================== //


// === Setup =================================================================================== //

void setup() {
    Serial.begin(115200);
    Serial.println();
    myNet.begin("ZHNetwork");   //nombre de la red y gateway=false por defecto
    myNet.setOnConfirmReceivingCallback(onConfirmReceiving);    // asignamos la función de callback
    myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
    msg = "Mensaje enviado desde " + myNet.getNodeMac();
    //espMAC = myNet.getNodeMac();
}

// === Loop ==================================================================================== //

void loop() {
    if ((millis() - messageLastTime) > messageTimerDelay) {
        myNet.sendUnicastMessage(msg.c_str(), target);
        messageLastTime = millis();

    }

    myNet.maintenance();
}

// === Implementación de funciones ============================================================= //
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status) {
    Serial.print("Message to MAC ");
    Serial.print(myNet.macToString(target));
    Serial.print(" ID ");
    Serial.print(id);
    Serial.println(status ? " delivered." : " undelivered.");
}

void onUnicastReceiving(const char *data, const uint8_t *sender)
{
    Serial.print("Unicast message from MAC ");
    Serial.print(myNet.macToString(sender));
    Serial.println(" received.");
    Serial.print("Message: ");
    Serial.println(data);
}

// === Funciones auxiliares internas =========================================================== //
