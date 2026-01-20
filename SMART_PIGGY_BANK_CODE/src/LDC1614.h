#pragma once
#include <Arduino.h>
#include <Wire.h>

class LDC1614 {
public:
  explicit LDC1614(uint8_t i2cAddr = 0x2A, TwoWire& wire = Wire);

  bool begin();                 
  bool readChannel0(uint32_t&); 

  bool readReg16(uint8_t reg, uint16_t& val);
  bool writeReg16(uint8_t reg, uint16_t val);

private:
  uint8_t _addr;
  TwoWire* _wire;

  bool readBytes(uint8_t startReg, uint8_t* buf, size_t len);
};
