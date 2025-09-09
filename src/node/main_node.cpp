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

//#define PAYLOAD_SIZE 10 //tamaño en bytes del payload (1 uint16_t + 1 float)

//maintenace ya envía la mac origen
#define CHANNEL_ANNOUNC  0x0A


// === Constantes y configuraciones iniciales ================================================= //
static QueueHandle_t queueMuestras = nullptr; //declaración del handle de la queue a nivel global
static const TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(5000); // 5 s
static const TickType_t NET_MAINT_PERIOD = pdMS_TO_TICKS(50); // 50 ms
ZHNetwork myNet;
Sensor sensor;
const uint8_t target[6]{ 0xA4, 0xCF, 0x12, 0x05, 0x1B, 0x65 };  //mac del gateway
//char texto[16]; //test
TaskHandle_t xHandleNet;
TaskHandle_t xHandleSensor;
TaskHandle_t xHandleSync;
uint8_t pri;
wifi_second_chan_t sec ;
 

enum
{
    SYNC_READY = 1,
    SYNC_WAIT
};



// === Prototipos de funciones ================================================================= //
// función de callback para cuando llega o no una confirmación de mensaje recibido (transmisor)
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);
//funcion de callback para cuando se recibe un mensaje (receptor)
void onUnicastReceiving(const char *data, const uint8_t *sender);
//funcion de callback para cuando se recibe un broadcast
void onBroadcastReceiving(const char *data, const uint8_t *sender);



static void SensorTask(void *); //tarea para la toma de muestras. static -> encapsulada y no accesible por extern en otro archivo.
static void NetTask(void *); //tarea para envio y recepcion
static void SyncTask (void *);

// === Variables globales ====================================================================== //
struct Measurement{ //estructura con los datos de muestra
    float temp;
    uint32_t seq;
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
    myNet.setOnBroadcastReceivingCallback(onBroadcastReceiving);

    //BaseType_t xTaskCreate(TaskFunction_t pvTaskCode, const char *pcName, uint32_t usStackDepth,
    //void *pvParameters, UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask)
    //usStackDepth -> tamaño de la pila para la tarea
    //*pvParameters -> parametros a pasar a la tarea (ej: static void SensorTask(void *); nada) 
    xTaskCreate(SensorTask, "SensorTask", 4096, nullptr, 3, &xHandleSensor);
    xTaskCreate(NetTask, "NetTask", 6144, nullptr, 4, &xHandleNet);
    xTaskCreate(SyncTask, "SyncTask", 4096, nullptr, 2, &xHandleSync);

    

    if(esp_wifi_get_channel(&pri, &sec)==ESP_OK){
        Serial.printf("channel: %u\n", pri); 
    };
      

}

// === Loop ==================================================================================== //

void loop() {
//cuando estoy esperando el mensaje en synctask hay que parar sensortask. NetTask sigue funcionando normalmente.
//esperamos el mensaje con la función de callback de broadcast. Si llega el mensaje 
}

// === Tasks =================================================================================== //

static void SyncTask(void *){
    

    for(;;){
        
        uint32_t flags = 0;
        xTaskNotifyWait(0, UINT32_MAX, &flags, portMAX_DELAY);
        if(!(flags & SYNC_READY))
        continue;
        Serial.println("Canal recibido correctamente. Se procede a la actualización.");
        
        
        if(esp_wifi_set_channel(myNet.channelNet, WIFI_SECOND_CHAN_NONE) == ESP_OK){
            Serial.println("Canal actualizado correctamente");
             if(esp_wifi_get_channel(&pri, &sec)==ESP_OK){
                Serial.printf("channel: %u\n", pri); 
            };
        }else{
            Serial.println("Error en la actualización del canal");
        }
        xTaskNotifyGive(xHandleSensor);

        
    }

}

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
    ulTaskNotifyTake(/*clearOnExit=*/pdTRUE, /*timeout=*/portMAX_DELAY);
    TickType_t last = xTaskGetTickCount();
    Serial.println("Se destrabó SensorTask");
    uint32_t actual_seq = 1;
    for(;;){
        vTaskDelayUntil(&last,SENSOR_PERIOD); //espera 3 segundos
        Measurement muestra;
        muestra.temp = 1.31*sensor.readTemp();
        muestra.seq = actual_seq++;
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

void onBroadcastReceiving(const char *data, const uint8_t *sender){

    //verificamos si se trata de un anuncio de canal
    //********FALTA HACER UNA VERIFICACION MAS PROFUNDA DE QUE ES UN MENSAJE DE CHANNEL ANOUNCEMENT SINO PUEDE HABER FALSOS POSITIVOS QUE DESCONFIGUREN LA RED************** */
    
    const uint8_t* b = reinterpret_cast<const uint8_t*>(data);
    uint8_t bufferVer = b[0];
    uint8_t bufferType = b[1];
    
    if((bufferType == CHANNEL_ANNOUNC) && (bufferVer == 1)){
        uint8_t bufferChannel = b[2];

        if(((1 <= bufferChannel) && (bufferChannel<= 13)) ){
            myNet.channelNet = bufferChannel;
            //libero synctask
            xTaskNotify(xHandleSync, SYNC_READY, eSetBits);
        }else{
            Serial.println("Warning: el canal anunciado es un número no válido o repetido");
        }
    }
    

    Serial.print("Broadcast message from MAC ");
    Serial.print(myNet.macToString(sender));
    Serial.println(" received.");
    /* Serial.print("Message: ");
    Serial.println(data); */
}

// === Funciones auxiliares internas =========================================================== //
