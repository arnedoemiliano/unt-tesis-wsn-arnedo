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
// === Includes y Defines ===================================================================== //
#include "ZHNetwork.h"
#include "sensor.h"
#define PAYLOAD_SIZE 6 //tamaño en bytes del payload (1 uint16_t + 1 float)
                        //maintenace ya envía la mac origen


// === Constantes y configuraciones iniciales ================================================= //
static QueueHandle_t queueMuestras = nullptr; //declaración del handle de la queue a nivel global
static const TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(5000); // 5 s
static const TickType_t NET_MAINT_PERIOD = pdMS_TO_TICKS(50); // 50 ms
ZHNetwork myNet;
Sensor sensor;
const uint8_t target[6]{ 0xA4, 0xCF, 0x12, 0x05, 0x1B, 0x65 };  //mac del gateway
//char texto[16]; //test



// === Prototipos de funciones ================================================================= //
// función de callback para cuando llega o no una confirmación de mensaje recibido (transmisor)
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);
//funcion de callback para cuando se recibe un mensaje (receptor)
void onUnicastReceiving(const char *data, const uint8_t *sender);

static void SensorTask(void *); //tarea para la toma de muestras. static -> encapsulada y no accesible por extern en otro archivo.
static void NetTask(void *); //tarea para envio y recepcion

// === Variables globales ====================================================================== //
struct Measurement{ //estructura con los datos de muestra
    float temp;
    uint16_t bat;
};

// === Setup =================================================================================== //

void setup() {
    Serial.begin(115200);
    sensor.initLM35(32); //numero de gpio asociado al adc
    sensor.initSBat(34);
    Serial.println();
    queueMuestras = xQueueCreate(8, PAYLOAD_SIZE); //creamos la queue de hasta 8 elementos de 6 bytes
    myNet.begin("ZHNetwork");   //nombre de la red y gateway=false por defecto
    myNet.setOnConfirmReceivingCallback(onConfirmReceiving);    // asignamos la función de callback
    myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
    //BaseType_t xTaskCreate(TaskFunction_t pvTaskCode, const char *pcName, uint32_t usStackDepth,
    //void *pvParameters, UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask)
    //usStackDepth -> tamaño de la pila para la tarea
    //*pvParameters -> parametros a pasar a la tarea (ej: static void SensorTask(void *); nada) 
    xTaskCreate(SensorTask, "SensorTask", 4096, nullptr, 2, nullptr);
    xTaskCreate(NetTask, "NetTask", 6144, nullptr, 3, nullptr);

}

// === Loop ==================================================================================== //

void loop() {

}

// === Tasks =================================================================================== //



static void NetTask(void *) {
    char buffer[PAYLOAD_SIZE];

    const TickType_t kMaintPeriod = pdMS_TO_TICKS(NET_MAINT_PERIOD);
    TickType_t nextMaint = xTaskGetTickCount(); //tiempo de mantenimiento

    for (;;) {
        TickType_t now = xTaskGetTickCount(); //tiempo actual
        TickType_t untilMaint = (nextMaint - now); // Cuando llega a 0 hay maintenance(). Cálculo seguro ante wrap. Único punto de delay fijo.

        /* Espera hasta que llegue un paquete o se termine el tiempo para maintenance (lo que ocurra primero)
        
        (untilMaint > 0) ? untilMaint : 0)
        Si aún falta tiempo para el próximo mantenimiento (untilMaint > 0) → esperá en la cola hasta esa cantidad de ticks.
        Si ya se cumplió o pasó el tiempo (untilMaint <= 0) → no espera nada, poné 0 → la llamada a xQueueReceive será no bloqueante 
        (devuelve de inmediato). */
        if (xQueueReceive(queueMuestras, buffer, (untilMaint > 0) ? untilMaint : 0) == pdTRUE) {
            myNet.sendUnicastMessage(buffer, target, /*confirm=*/0, /*isText=*/0);
            continue; // vuelve a evaluar tiempo restante
        }

        /* Si salimos por timeout (no llegó nada), hace maintenance */
        myNet.maintenance();
        nextMaint += kMaintPeriod; //se ajusta el nuevo tiempo de mantenimiento
    }

}


















static void SensorTask(void *){
    TickType_t last = xTaskGetTickCount();

    for(;;){
        
        vTaskDelayUntil(&last,SENSOR_PERIOD); //espera 3 segundos
        Measurement muestra;
        muestra.temp = 1.31*sensor.readTemp();
        muestra.bat = sensor.readBat();
        /*TEST*/
        /* snprintf(texto, sizeof(texto), "%.2f", muestra.temp); //convierte el float a texto
        Serial.print("Temperatura: ");
        Serial.println(texto);
        Serial.print("Batería: ");
        Serial.println(muestra.bat); */
        /* *** */
        (void)xQueueSend(queueMuestras, &muestra, 0); //no bloquea la tarea si la queue esta llena

    }

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
