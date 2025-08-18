#include "sensor.h"



float Sensor::readTemp(void)
{
    uint16_t adcValue;

    adcValue= analogRead(_pinTemp);
    return (adcValue * (_vref/_resolution))/10;

}

void Sensor::initLM35(uint8_t pinADC, float adcResolution, float adcVref)
{
    _pinTemp = pinADC;
    _resolution = adcResolution;
    _vref = adcVref;
    analogReadResolution(12);
    analogSetPinAttenuation(pinADC, ADC_0db); //atenuación a 0db (default)

}

void Sensor::initSBat(uint8_t pinADC)
{
    _pinBat = pinADC;
    analogReadResolution(12);
    analogSetPinAttenuation(pinADC, ADC_6db); //atenuación a 6db 

}

uint16_t Sensor::readBat(void)
{
    uint16_t adcValue;

    adcValue= analogReadMilliVolts(_pinBat);
    return adcValue;


}