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
#define MAX_ATTEMPT_HTTP_LOAD 5 // número maximo de intentos de subida http antes de reiniciar wifi
#define ATTEMPTS_ANNOUNC 10
const uint16_t V_FULL  = 1600;   // a reposo / carga completa
const uint16_t V_EMPTY = 1200;   // bajo carga ligera (protección antes de 3.0V)

// === Constantes y configuraciones iniciales ================================================== //

static const TickType_t NET_MAINT_PERIOD = pdMS_TO_TICKS(50); // 50 ms */
static const TickType_t LIMIT_SILENCE = pdMS_TO_TICKS(5000); // Tiempo maximo admisible sin mensajes 
static QueueHandle_t queuePackets = nullptr;                  // declaración del handle de la queue a nivel global
const char ssid[12] = "CLARO2GHz";                            //
const uint8_t sizeid = sizeof(ssid);
const char *password = "sMoaBFxYV56AMz4j";
static WiFiClientSecure client;
static HTTPClient http;
TaskHandle_t xHandleMaint;
TaskHandle_t xHandleBridge;
TaskHandle_t xHandleControl;
uint8_t channelwifi = 1;

// === Prototipos de funciones ================================================================= //

void onUnicastReceiving(const char *data, const uint8_t *sender);
static void ControlTask(void *);
static void MaintTask(void *);                                                 // Ejecuta maintenance que coloca paquetes en cola
static void BridgeTask(void *);
static void MonitorTask(void *);                                                // Saca paquetes de la cola y los envia por HTTP
uint16_t sendToDatacake(const uint8_t *mac, float temperature, float battery, uint32_t seq); // conversión a JSON y envío POST
uint8_t findChannel(void);

// === Variables globales ====================================================================== //

ZHNetwork myNet;
char texto[16];
struct Pkt
{ // estructura con los datos de un paquete
    float temp;
    uint32_t seq;
    uint8_t bat;
    uint8_t mac[6];
};

enum
{
    BRIDGE_NET_READY = 1,
    BRIDGE_NET_WAIT
}; //

// === Setup =================================================================================== //

void setup()
{

    Serial.begin(115200);
    myNet.begin(MODE_AP, "ZHNetwork");              //(nombre de la red, modo AP al inicio para channel 1)
    myNet.setOnUnicastReceivingCallback(onUnicastReceiving); // asignamos la funcion de callback para
    queuePackets = xQueueCreate(16, sizeof(Pkt));            // cuando llega un mensaje (recibiendo)
    xTaskCreate(MaintTask, "MaintTask", 3268, nullptr, 4, &xHandleMaint);
    xTaskCreate(BridgeTask, "BridgeTask", 5352, nullptr, 5, &xHandleBridge);
    xTaskCreate(ControlTask, "ControlTask", 2083, nullptr, 3, &xHandleControl);
    xTaskCreate(MonitorTask, "MonitorTask", 2048, nullptr,2,NULL);
    
}

// === Loop ==================================================================================== //

void loop()
{
}

// === Tasks =================================================================================== //

static void MonitorTask(void *){
    
    for (;;) {
        Serial.printf("ControlTask: %u words\n",
            uxTaskGetStackHighWaterMark(xHandleControl));
        Serial.printf("BridgeTask: %u words\n",
            uxTaskGetStackHighWaterMark(xHandleBridge));
        Serial.printf("MaintTask: %u words\n",
            uxTaskGetStackHighWaterMark(xHandleMaint));
        vTaskDelay(pdMS_TO_TICKS(7000)); // cada 7 segundos
    }

}



// encuentra la red, obtiene el channel, lo avisa a los nodos y se bloquea
static void ControlTask(void *)
{
    enum
    {
        SCAN,
        ANOUNC,
        CONMUT,
        NOMIN
    };

    struct __attribute__((packed)) PayloadSync
    {
        uint8_t ver;     // versión del protocolo: 1
        uint8_t type;    // anuncio de canal: 0x0A
        uint8_t channel; // canal real del router
        uint8_t seq;     // número de envío (0..n) //Por el momento no usado
    };
    static_assert(sizeof(PayloadSync) == 4, "El struct debe medir 4 bytes exactos");

    uint8_t state = SCAN;

    for (;;)
    {
        switch (state)
        {
        case SCAN:
        {
            // PAUSAR BRIDGETASK
            if ((channelwifi = findChannel()) != 0)
            {
                state = ANOUNC;
                
            }
            else
            {
                Serial.println("No se ha encontrado el canal tras todos los intentos. Reiniciando busqueda.");
                state = SCAN;
                // EXIT
            }
            break;
        }
        case ANOUNC:
        {
            PayloadSync payloadsync{1, 0x0A, channelwifi, 0};
            for (int i = 0; i < ATTEMPTS_ANNOUNC; i++)
            {
                payloadsync.seq = i;
                // avisamos que son bytes crudos y la cantidad de los mismos a la librería
                myNet.sendBroadcastMessage((char *)&payloadsync, false, sizeof(payloadsync));
                Serial.printf("Envio de sync message n°: %i\n", i);
                vTaskDelay(pdMS_TO_TICKS(110));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            state = CONMUT;
            break;
        }
        case CONMUT:
        {
            vTaskSuspend(xHandleMaint); // paramos maintenance()
            esp_now_deinit();           // paramos espnow
            myNet.channelNet = channelwifi;
            myNet.begin(MODE_AP_STA, "ZHNetwork"); // reiniciamos toda la red en modo AP+STA
            
            myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
            WiFi.begin(ssid, password);
            Serial.println("Conectando a la red Wi-Fi...\n");

            while (WiFi.status() != WL_CONNECTED)
            {
                Serial.print(".");
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskResume(xHandleMaint); // reiniciamos maintenance()

            if (channelwifi != WiFi.channel())
            {
                Serial.println("Error inesperado, el canal encontrado no coincide. Reiniciando el proceso...");
                state = SCAN;
                break;
            }

            Serial.println("Conexión exitosa a la red Wi-Fi");
            Serial.printf("Channel: %i", channelwifi);

            state = NOMIN;
            break;
        }
        default:
        {
            // aviso a bridgetask que empiece la conexión http persistente
            xTaskNotify(xHandleBridge, BRIDGE_NET_READY, eSetBits); 
            // Queda bloqueada hasta que alguien pida re-sincronizar
            ulTaskNotifyTake(/*clearOnExit=*/pdTRUE, /*timeout=*/portMAX_DELAY);

            state = SCAN; // cuando la “despierten”, vuelve a correr el bootstrap
            break;
        }
        }
    }
}

// maintenance -> onUnicastReceiving -> queuePackets
static void MaintTask(void *)
{
    TickType_t next = xTaskGetTickCount();
    myNet.maintenance(); // se ejecuta una vez antes de pasar al ciclo infinito.
    for (;;)
    {
        vTaskDelayUntil(&next, NET_MAINT_PERIOD); // 50 ms exactos
        myNet.maintenance();
    }
}

static void BridgeTask(void *)
{
    Pkt pkt;
    String deviceMAC;
    

    for (;;)
    {
        // espera la notificación de ControlTask para empezar la conexión http persistente
        // xTaskNotifyWait(bits a borrar antes de esperar, bits a borrar al salir, puntero donde guarda el valor recibido, cuanto tiempo espero la notificacion);
        // espera indefinidamente la notificación y no hace nada con el valor recibido. Borra al terminar.
        uint32_t flags = 0;

        xTaskNotifyWait(0, UINT32_MAX, &flags, portMAX_DELAY);
        if (!(flags & BRIDGE_NET_READY))
            continue;

        client.setInsecure();
        http.setReuse(true); // habilita HTTP keep-alive
        if (http.begin(client, "https://api.datacake.co/integrations/api/edb2f2b0-d238-4c0c-8b50-ee2c5f10c142/"))
        {
            http.addHeader("Content-Type", "application/json");
            Serial.print("***HTTP begin funciona, conexión inicializada***");
        }
        else
        {
            Serial.println("***No se pudo establecer la conexión HTTP***");
            //*********NO PUEDE ENTRAR AL FOR INTERNO SI NO HAY CONEXION HTTP*********** */
        };
        uint8_t cnt_err = 0; // contador de errores http

        for (;;)
        {
            

            // Tarea de desencolado y envíos al servidor. Entra cuando se establece HTTP persistente.

            // Espera bloqueante: despierta al llegar un paquete
            //limite de tiempo de 5 seg sin datos porque puede quedar trabada y nunca reconectar.
            if (xQueueReceive(queuePackets, &pkt, LIMIT_SILENCE) != pdTRUE)
                continue;

            if (sendToDatacake(pkt.mac, pkt.temp, pkt.bat, pkt.seq) != 200)
            {
                cnt_err++;
            }
            else
            {
                cnt_err = 0;
            }

            if (cnt_err > MAX_ATTEMPT_HTTP_LOAD)
            {
                Serial.println("Se perdió la conexión con el servidor. Se intentará reestablecer la conexión");
                //damos notificacion para resincronización a controltask
                xTaskNotifyGive(xHandleControl);
                break; //volvemos al for externo
            }
        }
    }
}

// === Implementación de funciones ============================================================= //
void onUnicastReceiving(const char *data, const uint8_t *sender)
{
    uint16_t vbat; // almacenamos el valor crudo
    uint8_t pct_bat; // almacenamos el valor en porcentaje

    Serial.print("Unicast message from MAC ");
    Serial.print(myNet.macToString(sender));
    Serial.println(" Recibido.");

    Pkt pkt;
    

    memcpy(&pkt.temp, data, sizeof(pkt.temp)); // copiamos los valores binarios del float contenido en data
    memcpy(&pkt.seq, data + sizeof(pkt.temp), sizeof(pkt.seq));
    memcpy(&vbat, data + sizeof(pkt.temp) + sizeof(pkt.seq), sizeof(vbat));

    
    pct_bat = (int)((long)(vbat - V_EMPTY) * 100 / (V_FULL - V_EMPTY));
    pct_bat = (pct_bat < 0) ? 0 : (pct_bat > 100 ? 100 : pct_bat);
    pkt.bat = pct_bat;


    
    memcpy(&pkt.mac, sender, 6);
    /* test */printf("numero de secuencia recibido: %i\n",pkt.seq);

    if (xQueueSend(queuePackets, &pkt, 0) != pdTRUE)
    {
        // Encolado incorrecto
        Serial.print("Error de encolación en xQueueSend: posible cola llena");
        UBaseType_t used = uxQueueMessagesWaiting(queuePackets);
        UBaseType_t free = uxQueueSpacesAvailable(queuePackets);
        Serial.printf("[Q] FULL! used=%u free=%u (drop)\n",
                      (unsigned)used, (unsigned)free);
    }
    else
    {
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

uint16_t sendToDatacake(const uint8_t *mac, float temperature, float battery, uint32_t seq)
{
    // Crear documento JSON
    Serial.println("Entramos a datacake");
    String macSender = myNet.macToString(mac);
    JsonDocument doc;
    doc["device"] = macSender; // enviamos la mac como serial
    doc["temperature"] = temperature;
    doc["battery"] = battery;
    doc["sequence"] = seq;
    doc["sender"] = macSender;

    String payload;
    serializeJson(doc, payload);

    // Enviar por HTTP POST

    int httpCode = http.POST(payload);
    String body = http.getString();
    Serial.println(body);
    Serial.printf("Datacake response: %d\n", httpCode);

    return httpCode;
}

uint8_t findChannel(void)
{
    uint8_t result = 0;            // valor malo por defecto
    const uint8_t maxAttempts = 3; // cant. de intentos maxima para encontrar channel
    uint8_t attempt = 0;
    bool found = false;

    while (attempt < maxAttempts && !found)
    {
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++)
        {
            if (!strcmp(ssid, WiFi.SSID(i).c_str()))
            {

                result = WiFi.channel(i);
                Serial.printf("Se encontró la ssid: %s\n", WiFi.SSID(i).c_str());
                Serial.printf("Canal: %i\n", result);
                found = true;
                break;
            }
        }
        if (!found)
        {
            Serial.printf("No se ha encontrado el canal de la red. Intento n°: %u \n", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000)); // espera 1 segundo antes de volver a intentarlo
            attempt++;
        }
    }

    return result;
}

// === Funciones auxiliares internas =========================================================== //
