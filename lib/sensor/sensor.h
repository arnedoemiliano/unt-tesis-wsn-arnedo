#include "Arduino.h"


class Sensor
{
    public:
        void initLM35(uint8_t pinADC, float adcResolution=4095.0, float adcVref=1100.0);
        void initSBat(uint8_t pinADC);
        float readTemp(void);
        uint16_t readBat(void);
    private:
        uint8_t _pinTemp;
        uint8_t _pinBat;
        float _resolution;
        float _vref;
        
};
