#include "ZHNetwork.h"

routing_vector_t ZHNetwork::routingVector;
confirmation_vector_t ZHNetwork::confirmationVector;
incoming_queue_t ZHNetwork::queueForIncomingData;
outgoing_queue_t ZHNetwork::queueForOutgoingData;
waiting_queue_t ZHNetwork::queueForRoutingVectorWaiting;

bool ZHNetwork::criticalProcessSemaphore{false};
bool ZHNetwork::sentMessageSemaphore{false};
bool ZHNetwork::confirmReceivingSemaphore{false};
bool ZHNetwork::confirmReceiving{false};
char ZHNetwork::netName_[20]{0};
char ZHNetwork::key_[20]{0};

//uint16_t ZHNetwork::lastMessageID[10]{0};

uint8_t ZHNetwork::localMAC[6]{0};

static last_message_info_t lastMessageInfo[10]{0};

void ZHNetwork::clearIDMACHistory(void)
{
    criticalProcessSemaphore = true;
    memset(lastMessageInfo, 0, sizeof lastMessageInfo);
    criticalProcessSemaphore = false;
    Serial.println("***Se limpió el historial de IDs para deduplicación***");
    return;
}

ZHNetwork &ZHNetwork::setOnBroadcastReceivingCallback(on_message_t onBroadcastReceivingCallback)
{
    this->onBroadcastReceivingCallback = onBroadcastReceivingCallback;
    return *this;
}

ZHNetwork &ZHNetwork::setOnUnicastReceivingCallback(on_message_t onUnicastReceivingCallback)
{
    this->onUnicastReceivingCallback = onUnicastReceivingCallback;
    return *this;
}

ZHNetwork &ZHNetwork::setOnConfirmReceivingCallback(on_confirm_t onConfirmReceivingCallback)
{
    this->onConfirmReceivingCallback = onConfirmReceivingCallback;
    return *this;
}

error_code_t ZHNetwork::begin(uint8_t mode, const char *netName)
{

    randomSeed(esp_random());

    if (strlen(netName) >= 1 && strlen(netName) <= 20)
        strcpy(netName_, netName);
#ifdef PRINT_LOG
    Serial.begin(115200);
#endif

    switch (mode)
    {
    case 0:
        WiFi.mode(WIFI_STA);
        break;
    case 1:
        WiFi.mode(WIFI_AP);
        break;
    case 2:
        WiFi.mode(WIFI_AP_STA);
        break;
    default: /* ignorar o fallback */
        break;
    }

    esp_now_init();
    clearIDMACHistory();
    esp_wifi_set_channel(channelNet, WIFI_SECOND_CHAN_NONE); // forzamos canal 1

    switch (mode)
    {
    case 0:
        esp_wifi_get_mac((wifi_interface_t)ESP_IF_WIFI_STA, localMAC);
        break;
    case 1:
        esp_wifi_get_mac((wifi_interface_t)ESP_IF_WIFI_AP, localMAC);
        break;
    case 2:
        esp_wifi_get_mac((wifi_interface_t)ESP_IF_WIFI_AP, localMAC);
        break;
    default: /* ignorar o fallback */
        break;
    }
    String mac_actual;
    mac_actual = macToString(localMAC);
    Serial.print("La MAC actual es: ");
    Serial.println(mac_actual);
    // esp_wifi_get_mac(gateway ? (wifi_interface_t)ESP_IF_WIFI_AP : (wifi_interface_t)ESP_IF_WIFI_STA, localMAC);
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceive);

    return SUCCESS;
}

uint16_t ZHNetwork::sendBroadcastMessage(const char *data, const bool isText, size_t len)
{
    return broadcastMessage(data, broadcastMAC, BROADCAST, isText, len);
}

uint16_t ZHNetwork::sendUnicastMessage(const char *data, const uint8_t *target, const bool confirm, const bool isText)
{
    return unicastMessage(data, target, localMAC, confirm ? UNICAST_WITH_CONFIRM : UNICAST, isText);
}

void ZHNetwork::maintenance()
{
    if (sentMessageSemaphore && confirmReceivingSemaphore)
    {
        sentMessageSemaphore = false;
        confirmReceivingSemaphore = false;
        if (confirmReceiving) //
        {
#ifdef PRINT_LOG
            Serial.println(F("OK."));
#endif
            outgoing_data_t outgoingData = queueForOutgoingData.front();
            queueForOutgoingData.pop();
            esp_now_del_peer(outgoingData.intermediateTargetMAC);

            if (onConfirmReceivingCallback && macToString(outgoingData.transmittedData.originalSenderMAC) == macToString(localMAC) && outgoingData.transmittedData.messageType == BROADCAST)
                onConfirmReceivingCallback(outgoingData.transmittedData.originalTargetMAC, outgoingData.transmittedData.messageID, true);
            if (macToString(outgoingData.transmittedData.originalSenderMAC) == macToString(localMAC) && outgoingData.transmittedData.messageType == UNICAST_WITH_CONFIRM)
            {
                confirmation_waiting_data_t confirmationData;
                confirmationData.time = millis();
                memcpy(&confirmationData.targetMAC, &outgoingData.transmittedData.originalTargetMAC, 6);
                memcpy(&confirmationData.messageID, &outgoingData.transmittedData.messageID, 2);
                confirmationVector.push_back(confirmationData);
            }
        }
        else
        {
#ifdef PRINT_LOG
            Serial.println(F("FAULT."));
#endif
            if (numberOfAttemptsToSend < maxNumberOfAttempts_)
                ++numberOfAttemptsToSend;
            else
            {
                outgoing_data_t outgoingData = queueForOutgoingData.front();
                queueForOutgoingData.pop();
                esp_now_del_peer(outgoingData.intermediateTargetMAC);

                numberOfAttemptsToSend = 1;
                for (uint16_t i{0}; i < routingVector.size(); ++i)
                {
                    routing_table_t routingTable = routingVector[i];
                    if (macToString(routingTable.originalTargetMAC) == macToString(outgoingData.transmittedData.originalTargetMAC))
                    {
                        routingVector.erase(routingVector.begin() + i);
#ifdef PRINT_LOG
                        Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
                        Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
                        Serial.println(F(" deleted."));
#endif
                    }
                }
                waiting_data_t waitingData;
                waitingData.time = millis();
                memcpy(&waitingData.intermediateTargetMAC, &outgoingData.intermediateTargetMAC, 6);
                memcpy(&waitingData.transmittedData, &outgoingData.transmittedData, sizeof(transmitted_data_t));
                queueForRoutingVectorWaiting.push(waitingData);
                broadcastMessage("", outgoingData.transmittedData.originalTargetMAC, SEARCH_REQUEST);
            }
        }
    }
    if (!queueForOutgoingData.empty() && ((millis() - lastMessageSentTime) > maxWaitingTimeBetweenTransmissions_))
    {
        outgoing_data_t outgoingData = queueForOutgoingData.front();
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, outgoingData.intermediateTargetMAC, 6);
        peerInfo.channel = channelNet;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
        esp_now_send(outgoingData.intermediateTargetMAC, (uint8_t *)&outgoingData.transmittedData, sizeof(transmitted_data_t));

        lastMessageSentTime = millis();
        sentMessageSemaphore = true;

#ifdef PRINT_LOG
        switch (outgoingData.transmittedData.messageType)
        {
        case BROADCAST:
            Serial.print(F("BROADCAST"));
            break;
        case UNICAST:
            Serial.print(F("UNICAST"));
            break;
        case UNICAST_WITH_CONFIRM:
            Serial.print(F("UNICAST_WITH_CONFIRM"));
            break;
        case DELIVERY_CONFIRM_RESPONSE:
            Serial.print(F("DELIVERY_CONFIRM_RESPONSE"));
            break;
        case SEARCH_REQUEST:
            Serial.print(F("SEARCH_REQUEST"));
            break;
        case SEARCH_RESPONSE:
            Serial.print(F("SEARCH_RESPONSE"));
            break;
        default:
            break;
        }
        Serial.print(F(" message from MAC "));
        Serial.print(macToString(outgoingData.transmittedData.originalSenderMAC));
        Serial.print(F(" to MAC "));
        Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
        Serial.print(F(" via MAC "));
        Serial.print(macToString(outgoingData.intermediateTargetMAC));
        Serial.print(F(" sended. Status "));
#endif
    }
    if (!queueForIncomingData.empty())
    {
        criticalProcessSemaphore = true;
        incoming_data_t incomingData = queueForIncomingData.front();
        queueForIncomingData.pop();
        criticalProcessSemaphore = false;
        bool forward{false};
        bool routingUpdate{false};
        switch (incomingData.transmittedData.messageType) // decide que hacer con el mensaje entrante segun su tipo
        {
        case BROADCAST:
#ifdef PRINT_LOG
            Serial.print(F("BROADCAST message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.println(F(" received."));
#endif
            if (onBroadcastReceivingCallback)
            {
                if (key_[0])
                    for (uint8_t i{0}; i < strlen(incomingData.transmittedData.message); ++i)
                        incomingData.transmittedData.message[i] = incomingData.transmittedData.message[i] ^ key_[i % strlen(key_)];
                onBroadcastReceivingCallback(incomingData.transmittedData.message, incomingData.transmittedData.originalSenderMAC);
            }
            forward = true;
            break;
        case UNICAST:
#ifdef PRINT_LOG
            Serial.print(F("UNICAST message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalTargetMAC));
            Serial.print(F(" via MAC "));
            Serial.print(macToString(incomingData.intermediateSenderMAC));
            Serial.println(F(" received."));
#endif
            if (macToString(incomingData.transmittedData.originalTargetMAC) == macToString(localMAC))
            {
                if (onUnicastReceivingCallback)
                {
                    if (key_[0])
                        for (uint8_t i{0}; i < strlen(incomingData.transmittedData.message); ++i)
                            incomingData.transmittedData.message[i] = incomingData.transmittedData.message[i] ^ key_[i % strlen(key_)];
                    onUnicastReceivingCallback(incomingData.transmittedData.message, incomingData.transmittedData.originalSenderMAC); // da los valores reales a la funcion de callback
                }
                /* Solo para test: avisar cuando el mensaje no haya sido recibido por el originalSender (haya sido reenviado) */
                /*Solamente lo activará el gateway porque este siempre es el originalTargetMAC*/
                if (memcmp(incomingData.transmittedData.originalSenderMAC, incomingData.intermediateSenderMAC, 6) != 0)
                {
                    Serial.println("El mensaje recibido fue retransmitido al menos en el último salto");
                }
            }
            else
                unicastMessage(incomingData.transmittedData.message, incomingData.transmittedData.originalTargetMAC, incomingData.transmittedData.originalSenderMAC, UNICAST);
            break;
        case UNICAST_WITH_CONFIRM:
#ifdef PRINT_LOG
            Serial.print(F("UNICAST_WITH_CONFIRM message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalTargetMAC));
            Serial.print(F(" via MAC "));
            Serial.print(macToString(incomingData.intermediateSenderMAC));
            Serial.println(F(" received."));
#endif
            if (macToString(incomingData.transmittedData.originalTargetMAC) == macToString(localMAC))
            {
                if (onUnicastReceivingCallback)
                {
                    if (key_[0])
                        for (uint8_t i{0}; i < strlen(incomingData.transmittedData.message); ++i)
                            incomingData.transmittedData.message[i] = incomingData.transmittedData.message[i] ^ key_[i % strlen(key_)];
                    onUnicastReceivingCallback(incomingData.transmittedData.message, incomingData.transmittedData.originalSenderMAC);
                }
                confirmation_id_t id;
                memcpy(&id.messageID, &incomingData.transmittedData.messageID, 2);
                char temp[sizeof(transmitted_data_t::message)];
                memcpy(&temp, &id, sizeof(transmitted_data_t::message));
                unicastMessage(temp, incomingData.transmittedData.originalSenderMAC, localMAC, DELIVERY_CONFIRM_RESPONSE);
            }
            else
                unicastMessage(incomingData.transmittedData.message, incomingData.transmittedData.originalTargetMAC, incomingData.transmittedData.originalSenderMAC, UNICAST_WITH_CONFIRM);
            break;
        case DELIVERY_CONFIRM_RESPONSE:
#ifdef PRINT_LOG
            Serial.print(F("DELIVERY_CONFIRM_RESPONSE message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalTargetMAC));
            Serial.print(F(" via MAC "));
            Serial.print(macToString(incomingData.intermediateSenderMAC));
            Serial.println(F(" received."));
#endif
            if (macToString(incomingData.transmittedData.originalTargetMAC) == macToString(localMAC))
            {
                if (onConfirmReceivingCallback)
                {
                    confirmation_id_t id;
                    memcpy(&id.messageID, &incomingData.transmittedData.message, 2);
                    for (uint16_t i{0}; i < confirmationVector.size(); ++i)
                    {
                        confirmation_waiting_data_t confirmationData = confirmationVector[i];
                        if (confirmationData.messageID == id.messageID)
                            confirmationVector.erase(confirmationVector.begin() + i);
                    }
                    onConfirmReceivingCallback(incomingData.transmittedData.originalSenderMAC, id.messageID, true);
                }
            }
            else
                unicastMessage(incomingData.transmittedData.message, incomingData.transmittedData.originalTargetMAC, incomingData.transmittedData.originalSenderMAC, DELIVERY_CONFIRM_RESPONSE);
            break;
        case SEARCH_REQUEST:
#ifdef PRINT_LOG
            Serial.print(F("SEARCH_REQUEST message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalTargetMAC));
            Serial.println(F(" received."));
#endif
            if (macToString(incomingData.transmittedData.originalTargetMAC) == macToString(localMAC))
                broadcastMessage("", incomingData.transmittedData.originalSenderMAC, SEARCH_RESPONSE);
            else
                forward = true;
            routingUpdate = true;
            break;
        case SEARCH_RESPONSE:
#ifdef PRINT_LOG
            Serial.print(F("SEARCH_RESPONSE message from MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(incomingData.transmittedData.originalTargetMAC));
            Serial.println(F(" received."));
#endif
            if (macToString(incomingData.transmittedData.originalTargetMAC) != macToString(localMAC))
                forward = true;
            routingUpdate = true;
            break;
        default:
            break;
        }
        if (forward)
        {
            outgoing_data_t outgoingData;
            memcpy(&outgoingData.transmittedData, &incomingData.transmittedData, sizeof(transmitted_data_t));
            memcpy(&outgoingData.intermediateTargetMAC, &broadcastMAC, 6);
            queueForOutgoingData.push(outgoingData);
            delay(random(10));
        }
        if (routingUpdate)
        {
            bool routeFound{false};
            for (uint16_t i{0}; i < routingVector.size(); ++i)
            {
                routing_table_t routingTable = routingVector[i];
                if (macToString(routingTable.originalTargetMAC) == macToString(incomingData.transmittedData.originalSenderMAC))
                {
                    routeFound = true;
                    if (macToString(routingTable.intermediateTargetMAC) != macToString(incomingData.intermediateSenderMAC))
                    {
                        memcpy(&routingTable.intermediateTargetMAC, &incomingData.intermediateSenderMAC, 6);
                        routingVector.at(i) = routingTable;
#ifdef PRINT_LOG
                        Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
                        Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
                        Serial.print(F(" updated. Target is "));
                        Serial.print(macToString(incomingData.intermediateSenderMAC));
                        Serial.println(F("."));
#endif
                    }
                }
            }
            if (!routeFound)
            {
                if (macToString(incomingData.transmittedData.originalSenderMAC) != macToString(incomingData.intermediateSenderMAC))
                {
                    routing_table_t routingTable;
                    memcpy(&routingTable.originalTargetMAC, &incomingData.transmittedData.originalSenderMAC, 6);
                    memcpy(&routingTable.intermediateTargetMAC, &incomingData.intermediateSenderMAC, 6);
                    routingVector.push_back(routingTable);
#ifdef PRINT_LOG
                    Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
                    Serial.print(macToString(incomingData.transmittedData.originalSenderMAC));
                    Serial.print(F(" added. Target is "));
                    Serial.print(macToString(incomingData.intermediateSenderMAC));
                    Serial.println(F("."));
#endif
                }
            }
        }
    }
    if (!queueForRoutingVectorWaiting.empty())
    {
        waiting_data_t waitingData = queueForRoutingVectorWaiting.front();
        for (uint16_t i{0}; i < routingVector.size(); ++i)
        {
            routing_table_t routingTable = routingVector[i];
            if (macToString(routingTable.originalTargetMAC) == macToString(waitingData.transmittedData.originalTargetMAC))
            {
                queueForRoutingVectorWaiting.pop();
                outgoing_data_t outgoingData;
                memcpy(&outgoingData.transmittedData, &waitingData.transmittedData, sizeof(transmitted_data_t));
                memcpy(&outgoingData.intermediateTargetMAC, &routingTable.intermediateTargetMAC, 6);
                queueForOutgoingData.push(outgoingData);
#ifdef PRINT_LOG
                Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
                Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
                Serial.print(F(" found. Target is "));
                Serial.print(macToString(outgoingData.intermediateTargetMAC));
                Serial.println(F("."));
#endif
                return;
            }
        }
        if ((millis() - waitingData.time) > maxTimeForRoutingInfoWaiting_)
        {
            queueForRoutingVectorWaiting.pop();
#ifdef PRINT_LOG
            Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
            Serial.print(macToString(waitingData.transmittedData.originalTargetMAC));
            Serial.println(F(" not found."));
            switch (waitingData.transmittedData.messageType)
            {
            case UNICAST:
                Serial.print(F("UNICAST"));
                break;
            case UNICAST_WITH_CONFIRM:
                Serial.print(F("UNICAST_WITH_CONFIRM"));
                break;
            case DELIVERY_CONFIRM_RESPONSE:
                Serial.print(F("DELIVERY_CONFIRM_RESPONSE"));
                break;
            default:
                break;
            }
            Serial.print(F(" message from MAC "));
            Serial.print(macToString(waitingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(waitingData.transmittedData.originalTargetMAC));
            Serial.print(F(" via MAC "));
            Serial.print(macToString(waitingData.intermediateTargetMAC));
            Serial.println(F(" undelivered."));
#endif
            if (waitingData.transmittedData.messageType == UNICAST_WITH_CONFIRM && macToString(waitingData.transmittedData.originalSenderMAC) == macToString(localMAC))
                if (onConfirmReceivingCallback)
                    onConfirmReceivingCallback(waitingData.transmittedData.originalTargetMAC, waitingData.transmittedData.messageID, false);
        }
    }
    if (confirmationVector.size())
    {
        for (uint16_t i{0}; i < confirmationVector.size(); ++i)
        {
            confirmation_waiting_data_t confirmationData = confirmationVector[i];
            if ((millis() - confirmationData.time) > maxTimeForRoutingInfoWaiting_)
            {
                confirmationVector.erase(confirmationVector.begin() + i);
                broadcastMessage("", confirmationData.targetMAC, SEARCH_REQUEST);
                if (onConfirmReceivingCallback)
                    onConfirmReceivingCallback(confirmationData.targetMAC, confirmationData.messageID, false);
            }
        }
    }
}

String ZHNetwork::getNodeMac()
{
    return macToString(localMAC);
}

String ZHNetwork::getFirmwareVersion()
{
    return firmware;
}

String ZHNetwork::macToString(const uint8_t *mac)
{
    String string;
    const char baseChars[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    for (uint32_t i{0}; i < 6; ++i)
    {
        string += (char)pgm_read_byte(baseChars + (mac[i] >> 4));
        string += (char)pgm_read_byte(baseChars + mac[i] % 16);
    }
    return string;
}

uint8_t *ZHNetwork::stringToMac(const String &string, uint8_t *mac)
{
    const uint8_t baseChars[75]{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0,
                                10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0, 0, 0, 0, 0, 0,
                                10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
    for (uint32_t i = 0; i < 6; ++i)
        mac[i] = (pgm_read_byte(baseChars + string.charAt(i * 2) - '0') << 4) + pgm_read_byte(baseChars + string.charAt(i * 2 + 1) - '0');
    return mac;
}

error_code_t ZHNetwork::setCryptKey(const char *key)
{
    if (strlen(key) >= 1 && strlen(key) <= 20)
        strcpy(key_, key);
    return SUCCESS;
}

error_code_t ZHNetwork::setMaxNumberOfAttempts(const uint8_t maxNumberOfAttempts)
{
    if (maxNumberOfAttempts < 1 || maxNumberOfAttempts > 10)
        return ERROR;
    maxNumberOfAttempts_ = maxNumberOfAttempts;
    return SUCCESS;
}

uint8_t ZHNetwork::getMaxNumberOfAttempts()
{
    return maxNumberOfAttempts_;
}

error_code_t ZHNetwork::setMaxWaitingTimeBetweenTransmissions(const uint8_t maxWaitingTimeBetweenTransmissions)
{
    if (maxWaitingTimeBetweenTransmissions < 50 || maxWaitingTimeBetweenTransmissions > 250)
        return ERROR;
    maxWaitingTimeBetweenTransmissions_ = maxWaitingTimeBetweenTransmissions;
    return SUCCESS;
}

uint8_t ZHNetwork::getMaxWaitingTimeBetweenTransmissions()
{
    return maxWaitingTimeBetweenTransmissions_;
}

error_code_t ZHNetwork::setMaxWaitingTimeForRoutingInfo(const uint16_t maxTimeForRoutingInfoWaiting)
{
    if (maxTimeForRoutingInfoWaiting < 500 || maxTimeForRoutingInfoWaiting > 5000)
        return ERROR;
    maxTimeForRoutingInfoWaiting_ = maxTimeForRoutingInfoWaiting;
    return SUCCESS;
}

uint16_t ZHNetwork::getMaxWaitingTimeForRoutingInfo()
{
    return maxTimeForRoutingInfoWaiting_;
}

void IRAM_ATTR ZHNetwork::onDataSent(const uint8_t *mac, esp_now_send_status_t status)

{
    confirmReceivingSemaphore = true;
    confirmReceiving = status ? false : true;
}

// void IRAM_ATTR ZHNetwork::onDataReceive(const uint8_t *mac, const uint8_t *data, int length)
void IRAM_ATTR ZHNetwork::onDataReceive(const uint8_t *mac_addr, const uint8_t *data, int length)

{
    if (criticalProcessSemaphore)
        return;
    criticalProcessSemaphore = true;
    if (length != sizeof(transmitted_data_t))
    {
        criticalProcessSemaphore = false;
        return;
    }
    incoming_data_t incomingData;
    memcpy(&incomingData.transmittedData, data, sizeof(transmitted_data_t));
    if (macToString(incomingData.transmittedData.originalSenderMAC) == macToString(localMAC))
    {
        criticalProcessSemaphore = false;
        return;
    }
    if (String(netName_) != "")
    {
        if (String(incomingData.transmittedData.netName) != String(netName_))
        {
            criticalProcessSemaphore = false;
            return;
        }
    }

    // Busqueda de mensajes repetidos por ID confirmados por misma MAC en el array de estructuras tipo last_message_info_t
    uint8_t N = (sizeof(lastMessageInfo)) / sizeof(lastMessageInfo[0]); /*10*/

    for (size_t i = 0; i < N; i++)
    {
        if (lastMessageInfo[i].id == incomingData.transmittedData.messageID)
        {

            if (!(memcmp(lastMessageInfo[i].sender, incomingData.transmittedData.originalSenderMAC, sizeof(incomingData.transmittedData.originalSenderMAC))))
            {
                Serial.println("***Se descartó el mensaje por repetición de ID+MAC***");
                Serial.print("ID=");
                Serial.print(incomingData.transmittedData.messageID);
                Serial.print("MAC=");
                Serial.println(macToString(incomingData.transmittedData.originalSenderMAC));
                criticalProcessSemaphore = false;
                return;
            }
        }
    }

    // Rotación del array y agregado de último par ID-MAC más reciente.
    if (N > 1)
    {
        memmove(&lastMessageInfo[1], &lastMessageInfo[0],
                (N - 1) * sizeof lastMessageInfo[0]);
    }

    lastMessageInfo[0].id = incomingData.transmittedData.messageID;
    memcpy(lastMessageInfo[0].sender, incomingData.transmittedData.originalSenderMAC, 6); // hardcodeado, la mac siempre es 6 bytes

    memcpy(&incomingData.intermediateSenderMAC, mac_addr, 6);
    queueForIncomingData.push(incomingData);
    criticalProcessSemaphore = false;
}

uint16_t ZHNetwork::broadcastMessage(const char *data, const uint8_t *target, message_type_t type, const bool isText, size_t len)
{
    outgoing_data_t outgoingData;
    outgoingData.transmittedData.messageType = type;
    outgoingData.transmittedData.messageID = ((uint16_t)random(32767) << 8) | (uint16_t)random(32767);
    memcpy(&outgoingData.transmittedData.netName, &netName_, 20);
    memcpy(&outgoingData.transmittedData.originalTargetMAC, target, 6);
    memcpy(&outgoingData.transmittedData.originalSenderMAC, &localMAC, 6);

    if (!isText)
    {
        memcpy(outgoingData.transmittedData.message, data, len);
    }
    else
    {
        strcpy(outgoingData.transmittedData.message, data);
    }

    if (key_[0] && outgoingData.transmittedData.messageType == BROADCAST)
        for (uint8_t i{0}; i < strlen(outgoingData.transmittedData.message); ++i)
            outgoingData.transmittedData.message[i] = outgoingData.transmittedData.message[i] ^ key_[i % strlen(key_)];
    memcpy(&outgoingData.intermediateTargetMAC, &broadcastMAC, 6);
    queueForOutgoingData.push(outgoingData);
#ifdef PRINT_LOG
    switch (outgoingData.transmittedData.messageType)
    {
    case BROADCAST:
        Serial.print(F("BROADCAST"));
        break;
    case SEARCH_REQUEST:
        Serial.print(F("SEARCH_REQUEST"));
        break;
    case SEARCH_RESPONSE:
        Serial.print(F("SEARCH_RESPONSE"));
        break;
    default:
        break;
    }
    Serial.print(F(" message from MAC "));
    Serial.print(macToString(outgoingData.transmittedData.originalSenderMAC));
    Serial.print(F(" to MAC "));
    Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
    Serial.println(F(" added to queue."));
#endif
    return outgoingData.transmittedData.messageID;
}

uint16_t ZHNetwork::unicastMessage(const char *data, const uint8_t *target, const uint8_t *sender, message_type_t type, const bool isText)
{
    outgoing_data_t outgoingData;
    outgoingData.transmittedData.messageType = type;
    outgoingData.transmittedData.messageID = ((uint16_t)random(32767) << 8) | (uint16_t)random(32767);
    memcpy(&outgoingData.transmittedData.netName, &netName_, 20);
    memcpy(&outgoingData.transmittedData.originalTargetMAC, target, 6);
    memcpy(&outgoingData.transmittedData.originalSenderMAC, sender, 6);

    // si lo que envio es un float dentro de un string y hacemos strcpy, se puede cortar el mensaje si detecta un cero en los datos
    // por eso copiamos con memcpy cuando queremos enviar floats y no cadenas de texto.
    if (!isText)
    {
        memcpy(outgoingData.transmittedData.message, data, PAYLOAD_SIZE); // hardcodeado por el momento
    }
    else
    {
        strcpy(outgoingData.transmittedData.message, data);
    }

    if (key_[0] && macToString(outgoingData.transmittedData.originalSenderMAC) == macToString(localMAC) && outgoingData.transmittedData.messageType != DELIVERY_CONFIRM_RESPONSE)
        for (uint8_t i{0}; i < strlen(outgoingData.transmittedData.message); ++i)
            outgoingData.transmittedData.message[i] = outgoingData.transmittedData.message[i] ^ key_[i % strlen(key_)];
    for (uint16_t i{0}; i < routingVector.size(); ++i)
    {
        routing_table_t routingTable = routingVector[i];
        if (macToString(routingTable.originalTargetMAC) == macToString(target))
        {
            memcpy(&outgoingData.intermediateTargetMAC, &routingTable.intermediateTargetMAC, 6);
            queueForOutgoingData.push(outgoingData);
#ifdef PRINT_LOG
            Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
            Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
            Serial.print(F(" found. Target is "));
            Serial.print(macToString(outgoingData.intermediateTargetMAC));
            Serial.println(F("."));
            switch (outgoingData.transmittedData.messageType)
            {
            case UNICAST:
                Serial.print(F("UNICAST"));
                break;
            case UNICAST_WITH_CONFIRM:
                Serial.print(F("UNICAST_WITH_CONFIRM"));
                break;
            case DELIVERY_CONFIRM_RESPONSE:
                Serial.print(F("DELIVERY_CONFIRM_RESPONSE"));
                break;
            default:
                break;
            }
            Serial.print(F(" message from MAC "));
            Serial.print(macToString(outgoingData.transmittedData.originalSenderMAC));
            Serial.print(F(" to MAC "));
            Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
            Serial.print(F(" via MAC "));
            Serial.print(macToString(outgoingData.intermediateTargetMAC));
            Serial.println(F(" added to queue."));
#endif
            return outgoingData.transmittedData.messageID;
        }
    }
    memcpy(&outgoingData.intermediateTargetMAC, target, 6);
    queueForOutgoingData.push(outgoingData);
#ifdef PRINT_LOG
    Serial.print(F("CHECKING ROUTING TABLE... Routing to MAC "));
    Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
    Serial.print(F(" not found. Target is "));
    Serial.print(macToString(outgoingData.intermediateTargetMAC));
    Serial.println(F("."));
    switch (outgoingData.transmittedData.messageType)
    {
    case UNICAST:
        Serial.print(F("UNICAST"));
        break;
    case UNICAST_WITH_CONFIRM:
        Serial.print(F("UNICAST_WITH_CONFIRM"));
        break;
    case DELIVERY_CONFIRM_RESPONSE:
        Serial.print(F("DELIVERY_CONFIRM_RESPONSE"));
        break;
    default:
        break;
    }
    Serial.print(F(" message from MAC "));
    Serial.print(macToString(outgoingData.transmittedData.originalSenderMAC));
    Serial.print(F(" to MAC "));
    Serial.print(macToString(outgoingData.transmittedData.originalTargetMAC));
    Serial.print(F(" via MAC "));
    Serial.print(macToString(outgoingData.intermediateTargetMAC));
    Serial.println(F(" added to queue."));
#endif
    return outgoingData.transmittedData.messageID;
}

const uint8_t *ZHNetwork::getLocalMAC()
{
    return localMAC;
}
