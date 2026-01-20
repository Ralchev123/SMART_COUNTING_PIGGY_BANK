#include "LDC1614.h"

static constexpr uint8_t REG_DATA0_MSB = 0x00;
static constexpr uint8_t REG_DATA0_LSB = 0x01;

static constexpr uint8_t REG_RCOUNT0   = 0x08;
static constexpr uint8_t REG_OFFSET0   = 0x0C;
static constexpr uint8_t REG_SETTLE0   = 0x10;
static constexpr uint8_t REG_CLKDIV0   = 0x14;

static constexpr uint8_t REG_STATUS    = 0x18;
static constexpr uint8_t REG_ERRORCFG  = 0x19;
static constexpr uint8_t REG_CONFIG    = 0x1A;
static constexpr uint8_t REG_MUXCFG    = 0x1B;

static constexpr uint8_t REG_DRIVE0    = 0x1E;

LDC1614::LDC1614(uint8_t i2cAddr, TwoWire& wire)
: _addr(i2cAddr), _wire(&wire) {}

bool LDC1614::writeReg16(uint8_t reg, uint16_t val) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);                 
  _wire->write(uint8_t(val >> 8));
  _wire->write(uint8_t(val & 0xFF));
  return _wire->endTransmission() == 0;
}

bool LDC1614::readBytes(uint8_t startReg, uint8_t* buf, size_t len) {
  _wire->beginTransmission(_addr);
  _wire->write(startReg);            
  if (_wire->endTransmission(false) != 0) return false;

  size_t got = _wire->requestFrom((int)_addr, (int)len);
  if (got != len) return false;

  for (size_t i = 0; i < len; i++) buf[i] = _wire->read();
  return true;
}

bool LDC1614::readReg16(uint8_t reg, uint16_t& val) {
  uint8_t b[2];
  if (!readBytes(reg, b, 2)) return false;
  val = (uint16_t(b[0]) << 8) | b[1];
  return true;
}

bool LDC1614::begin() {
  if (!writeReg16(REG_RCOUNT0,  0x04D6)) return false;
  if (!writeReg16(REG_SETTLE0,  0x000A)) return false;
  if (!writeReg16(REG_CLKDIV0,  0x1002)) return false;
  if (!writeReg16(REG_ERRORCFG, 0x0000)) return false;
  if (!writeReg16(REG_MUXCFG,   0x020C)) return false;
  if (!writeReg16(REG_DRIVE0,   0x9000)) return false;
  if (!writeReg16(REG_OFFSET0,  0x0000)) return false;

  if (!writeReg16(REG_CONFIG,   0x1401)) return false;

  return true;
}

bool LDC1614::readChannel0(uint32_t& out) {
  uint16_t msw, lsw;
  if (!readReg16(REG_DATA0_MSB, msw)) return false;
  if (!readReg16(REG_DATA0_LSB, lsw)) return false;

  uint32_t code28 = (uint32_t(msw & 0x0FFF) << 16) | uint32_t(lsw);
  out = code28;
  return true;
}
