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

 #include "ZHNetwork.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// === Constantes y configuraciones iniciales ================================================== //

static const TickType_t NET_MAINT_PERIOD = pdMS_TO_TICKS(50); // 50 ms */
static QueueHandle_t queuePackets = nullptr; //declaración del handle de la queue a nivel global
const char* ssid = "CLARO2GHz"; //
const char* password = "sMoaBFxYV56AMz4j";
static WiFiClientSecure client;
static HTTPClient http;

// === Prototipos de funciones ================================================================= //

void onUnicastReceiving(const char *data, const uint8_t *sender);
static void MaintTask(void *); //Ejecuta maintenance que coloca paquetes en cola
static void BridgeTask(void *); //Saca paquetes de la cola y los envia por HTTP
void sendToDatacake(const uint8_t* mac, float temperature, float battery); //conversión a JSON y envío POST

// === Variables globales ====================================================================== //

ZHNetwork myNet;
char texto[16];
struct Pkt{     //estructura con los datos de un paquete
    float temp;
    uint16_t bat;
    uint8_t mac[6];
};

// === Setup =================================================================================== //

void setup() {
    Serial.begin(115200);
    myNet.begin("ZHNetwork",/*ap+sta*/true);                                //(nombre de la red, modo gateway)
    myNet.setOnUnicastReceivingCallback(onUnicastReceiving);                //asignamos la funcion de callback para
    queuePackets = xQueueCreate(16, sizeof(Pkt));                           //cuando llega un mensaje (recibiendo)
    xTaskCreate(MaintTask, "MaintTask", 4096, nullptr, 2, nullptr);
    xTaskCreate(BridgeTask, "BridgeTask", 6144, nullptr, 3, nullptr);
    WiFi.begin(ssid,password);
    Serial.println("\nConnecting");

    while(WiFi.status() != WL_CONNECTED){
        Serial.print(".");
        delay(100);
    }
    Serial.println("\n***Conectado a la red Wi-Fi***");
    Serial.printf("STA channel = %d\n", WiFi.channel());

    client.setInsecure();
    http.setReuse(true); // habilita HTTP keep-alive
    if(http.begin(client, "https://api.datacake.co/integrations/api/edb2f2b0-d238-4c0c-8b50-ee2c5f10c142/")){
        http.addHeader("Content-Type", "application/json");
        Serial.print("***HTTP begin funciona, conexión inicializada***");
    }else{
        Serial.println("***No se pudo establecer la conexión HTTP***");
    };

}

// === Loop ==================================================================================== //

void loop() {
}

// === Tasks =================================================================================== //
//maintenance -> onUnicastReceiving -> queuePackets
static void MaintTask(void *) {
    TickType_t next = xTaskGetTickCount();
    myNet.maintenance(); //se ejecuta una vez antes de pasar al ciclo infinito.
    for (;;) {
        vTaskDelayUntil(&next, NET_MAINT_PERIOD);  // 50 ms exactos
        myNet.maintenance();
    }
}

static void BridgeTask(void *){
    Pkt pkt;
    String deviceMAC;

    for(;;){
        // Espera bloqueante: despierta al llegar un paquete
        if (xQueueReceive(queuePackets, &pkt, portMAX_DELAY) != pdTRUE) continue;
       
        sendToDatacake(pkt.mac, pkt.temp, pkt.bat);

        /*TEST
        snprintf(texto, sizeof(texto), "%.2f", m.temp); //convierte el float a texto
        Serial.print("Temperatura: ");
        Serial.println(texto);
        Serial.print("Batería: ");
        Serial.println(m.bat); */

    }

}

// === Implementación de funciones ============================================================= //
void onUnicastReceiving(const char *data, const uint8_t *sender)
{

    Serial.print("Unicast message from MAC ");
    Serial.print(myNet.macToString(sender));
    Serial.println(" Recibido.");

    Pkt pkt;

    memcpy(&pkt.temp, data, sizeof(pkt.temp)); //copiamos los valores binarios del float contenido en data
    memcpy(&pkt.bat, data+sizeof(pkt.temp), sizeof(pkt.bat));
    memcpy(&pkt.mac, sender, 6);

    if(xQueueSend(queuePackets, &pkt, 0)!=pdTRUE){
        //Encolado incorrecto
        Serial.print("Error de encolación en xQueueSend: posible cola llena");
        UBaseType_t used = uxQueueMessagesWaiting(queuePackets);
        UBaseType_t free = uxQueueSpacesAvailable(queuePackets);
        Serial.printf("[Q] FULL! used=%u free=%u (drop)\n",
                      (unsigned)used, (unsigned)free);
    }else{
        // Encolado correcto, mostrar ocupación
        UBaseType_t used = uxQueueMessagesWaiting(queuePackets);
        UBaseType_t free = uxQueueSpacesAvailable(queuePackets);
        Serial.printf("[Q] Enqueued OK. used=%u free=%u\n",
                      (unsigned)used, (unsigned)free);
    }

    /* 
    TEST
    snprintf(texto, sizeof(texto), "%.2f", temp); //convierte el float a texto
    Serial.print("Temperatura: ");
    Serial.println(texto);
    Serial.print("Batería: ");
    Serial.println(bat); */
}

void sendToDatacake(const uint8_t* mac, float temperature, float battery) {
    // Crear documento JSON
    Serial.println("Entramos a datacake");
    JsonDocument doc;
    doc["device"]      = "bbaf51c9-df26-4c5d-ac67-9ea9cf11188d"; //serial del device datacake
    doc["temperature"] = temperature;
    doc["battery"]     = battery;
    doc["sender"]      = myNet.macToString(mac);

    String payload;
    serializeJson(doc, payload);

    // Enviar por HTTP POST

    int httpCode = http.POST(payload);
    String body = http.getString();
    Serial.println(body);
    Serial.printf("Datacake response: %d\n", httpCode);
}

// === Funciones auxiliares internas =========================================================== //
