/**
 * @file rfm69.cpp
 *
 * @brief RFM69 and RFM69HW library for sending and receiving packets in connection with a STM32 controller.
 * @date January, February 2015
 * @author André Heßling
 *
 * This is a protocol agnostic driver library for handling HopeRF's RFM69 433/868/915 MHz RF modules.
 * Support is also available for the +20 dBm high power modules called RFM69HW/RFM69HCW.
 *
 * A CSMA/CA (carrier sense multiple access) algorithm can be enabled to avoid collisions.
 * If you want to enable CSMA, you should initialize the random number generator before.
 *
 * This library is written for the STM32 family of controllers, but can easily be ported to other devices.
 *
 * You have to provide your own functions for rfm69hal_delay_ms() and rfm69hal_get_timer_ms().
 * Use the SysTick timer (for example) with a 1 ms resolution which is present on all ARM controllers.
 *
 * If you want to port this library to other devices, you have to provide an SPI instance
 * derived from the SPIBase class.
 */

/** @addtogroup RFM69
 * @{
 */

#include "rfm69.hpp"
#include "rfm69hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <QDebug>
#include <unistd.h>

#define TIMEOUT_MODE_READY    1000000 ///< Maximum amount of time until mode switch [ms]
#define TIMEOUT_PACKET_SENT   100 ///< Maximum amount of time until packet must be sent [ms]
#define TIMEOUT_CSMA_READY    500 ///< Maximum CSMA wait time for channel free detection [ms]
#define CSMA_RSSI_THRESHOLD   -85 ///< If RSSI value is smaller than this, consider channel as free [dBm]

/** RFM69 base configuration after init().
 *
 * Change these to your needs or call setCustomConfig() after module init.
 */
static const uint8_t rfm69_base_config[][2] =
{
    {0x01, 0x04}, // RegOpMode: Standby Mode
    {0x02, 0x00}, // RegDataModul: Packet mode, FSK, no shaping
    {0x03, 0x00}, // RegBitrateMsb: 10 kbps
    {0x04, 0x80}, // RegBitrateLsb

    {0x05, 0x10}, // RegFdevMsb: 20 kHz
	{0x06, 0x00}, // RegFdevLsb


    {0x07, 0xD9}, // RegFrfMsb: 868,15 MHz
    {0x08, 0x09}, // RegFrfMid
    {0x09, 0x9A}, // RegFrfLsb
    {0x18, 0x88}, // RegLNA: 200 Ohm impedance, gain set by AGC loop

    {0x19, 0xe8}, // RegRxBw: 25 kHz
	{0x1a, 0xe0},

    {0x2C, 0x00}, // RegPreambleMsb: 3 bytes preamble
    {0x2D, 0x03}, // RegPreambleLsb
    {0x2E, 0x88}, // RegSyncConfig: Enable sync word, 2 bytes sync word
    {0x2F, 0x41}, // RegSyncValue1: 0x4148
    {0x30, 0x48}, // RegSyncValue2
    {0x37, 0xD0}, // RegPacketConfig1: Variable length, CRC on, whitening
    {0x38, RFM69_MAX_PAYLOAD}, // RegPayloadLength: 64 bytes max payload
    {0x3C, 0x8F}, // RegFifoThresh: TxStart on FifoNotEmpty, 15 bytes FifoLevel
    {0x58, 0x1B}, // RegTestLna: Normal sensitivity mode
    {0x6F, 0x30}, // RegTestDagc: Improved margin, use if AfcLowBetaOn=0 (default)


};

// Clock constants. DO NOT CHANGE THESE!
#define RFM69_XO               32000000    ///< Internal clock frequency [Hz]
#define RFM69_FSTEP            61.03515625 ///< Step width of synthesizer [Hz]

/**
 * RFM69 default constructor. Use init() to start working with the RFM69 module.
 *
 * @param spi Pointer to a SPI device
 * @param csGPIO GPIO of /CS line (ie. GPIOA, GPIOB, ...)
 * @param csPin Pin of /CS line (eg. GPIO_Pin_1)
 * @param highPowerDevice Set to true, if this is a RFM69Hxx device (default: false)
 */
RFM69::RFM69(bool highPowerDevice)
{
  _init = false;
  _mode = RFM69_MODE_STANDBY;
  _highPowerDevice = highPowerDevice;
  _powerLevel = 0;
  _rssi = -127;
  _ookEnabled = false;
  _autoReadRSSI = false;
  _dataMode = RFM69_DATA_MODE_PACKET;
  _highPowerSettings = false;
  _csmaEnabled = false;
  _rxBufferLength = 0;
}

RFM69::~RFM69()
{

}

/**
 * Reset the RFM69 module using the external reset line.
 *
 * @note Use setResetPin() before calling this function.
 */
void RFM69::reset()
{
  _init = false;

  // generate reset impulse
  rfm69hal_enable(true);
  rfm69hal_delay_ms(1);
  rfm69hal_enable(false);

  // wait until module is ready
  rfm69hal_delay_ms(10);

  _mode = RFM69_MODE_STANDBY;
}

/**
 * Initialize the RFM69 module.
 * A base configuration is set and the module is put in standby mode.
 *
 * @return Always true
 */
bool RFM69::init()
{
  rfm69hal_init();
  // set base configuration
  setCustomConfig(rfm69_base_config, sizeof(rfm69_base_config) / 2);

  // set PA and OCP settings according to RF module (normal/high power)
  setPASettings();

  // clear FIFO and flags
  clearFIFO();

  _init = true;

  return _init;
}

/**
 * Set the carrier frequency in Hz.
 * After calling this function, the module is in standby mode.
 *
 * @param frequency Carrier frequency in Hz
 */
void RFM69::setFrequency(unsigned int frequency)
{
  // switch to standby if TX/RX was active
  if (RFM69_MODE_RX == _mode || RFM69_MODE_TX == _mode)
    setMode(RFM69_MODE_STANDBY);

  // calculate register value
  frequency /= RFM69_FSTEP;

  // set new frequency
  writeRegister(0x07, frequency >> 16);
  writeRegister(0x08, frequency >> 8);
  writeRegister(0x09, frequency);
}

/**
 * Set the FSK frequency deviation in Hz.
 * After calling this function, the module is in standby mode.
 *
 * @param frequency Frequency deviation in Hz
 */
void RFM69::setFrequencyDeviation(unsigned int frequency)
{
  // switch to standby if TX/RX was active
  if (RFM69_MODE_RX == _mode || RFM69_MODE_TX == _mode)
    setMode(RFM69_MODE_STANDBY);

  // calculate register value
  frequency /= RFM69_FSTEP;

  // set new frequency
  writeRegister(0x05, frequency >> 8);
  writeRegister(0x06, frequency);
}

/**
 * Set the bitrate in bits per second.
 * After calling this function, the module is in standby mode.
 *
 * @param bitrate Bitrate in bits per second
 */
void RFM69::setBitrate(unsigned int bitrate)
{
  // switch to standby if TX/RX was active
  if (RFM69_MODE_RX == _mode || RFM69_MODE_TX == _mode)
    setMode(RFM69_MODE_STANDBY);

  // calculate register value
  bitrate = RFM69_XO / bitrate;

  // set new bitrate
  writeRegister(0x03, bitrate >> 8);
  writeRegister(0x04, bitrate);
}

/**
 * Read a RFM69 register value.
 *
 * @param reg The register to be read
 * @return The value of the register
 */
uint8_t RFM69::readRegister(uint8_t reg)
{
  // sanity check
  if (reg > 0x7f)
    return 0;

  // read value from register
  chipSelect();

  unsigned char thedata[2];
  thedata[0] = reg & 0x7F;
  thedata[1] = 0;

  rfm69hal_transfer(thedata, 2);

  chipUnselect();

  return thedata[1];
}

/**
 * Write a RFM69 register value.
 *
 * @param reg The register to be written
 * @param value The value of the register to be set
 */
void RFM69::writeRegister(uint8_t reg, uint8_t value)
{
  // sanity check
  if (reg > 0x7f)
    return;

  // transfer value to register and set the write flag
  chipSelect();

  unsigned char thedata[2];
  thedata[0] = reg | 0x80;
  thedata[1] = value;

  rfm69hal_transfer(thedata, 2);

  chipUnselect();
}

/**
 * Acquire the chip.
 */
void RFM69::chipSelect()
{
    rfm69hal_enable(true);
}

RFM69Mode RFM69::setMode(RFM69Mode mode)
{
  if ((mode == _mode) || (mode > RFM69_MODE_RX))
    return _mode;

  // set new mode
  writeRegister(0x01, mode << 2);

  // set special registers if this is a high power device (RFM69HW)
  if (true == _highPowerDevice)
  {
    switch (mode)
    {
    case RFM69_MODE_RX:
      // normal RX mode
      if (true == _highPowerSettings)
        setHighPowerSettings(false);
      break;

    case RFM69_MODE_TX:
      // +20dBm operation on PA_BOOST
      if (true == _highPowerSettings)
        setHighPowerSettings(true);
      break;

    default:
      break;
    }
  }

  _mode = mode;

  return _mode;
}

/**
 * Release the chip.
 */
void RFM69::chipUnselect()
{
    rfm69hal_enable(false);
}

/**
 * Enable/disable the power amplifier(s) of the RFM69 module.
 *
 * PA0 for regular devices is enabled and PA1 is used for high power devices (default).
 *
 * @note Use this function if you want to manually override the PA settings.
 * @note PA0 can only be used with regular devices (not the high power ones!)
 * @note PA1 and PA2 can only be used with high power devices (not the regular ones!)
 *
 * @param forcePA If this is 0, default values are used. Otherwise, PA settings are forced.
 *                0x01 for PA0, 0x02 for PA1, 0x04 for PA2, 0x08 for +20 dBm high power settings.
 */
void RFM69::setPASettings(uint8_t forcePA)
{
  // disable OCP for high power devices, enable otherwise
  writeRegister(0x13, 0x0A | (_highPowerDevice ? 0x00 : 0x10));

  if (0 == forcePA)
  {
    if (true == _highPowerDevice)
    {
      // enable PA1 only
      writeRegister(0x11, (readRegister(0x11) & 0x1F) | 0x40);
    }
    else
    {
      // enable PA0 only
      writeRegister(0x11, (readRegister(0x11) & 0x1F) | 0x80);
    }
  }
  else
  {
    // PA settings forced
    uint8_t pa = 0;

    if (forcePA & 0x01)
      pa |= 0x80;

    if (forcePA & 0x02)
      pa |= 0x40;

    if (forcePA & 0x04)
      pa |= 0x20;

    // check if high power settings are forced
    _highPowerSettings = (forcePA & 0x08) ? true : false;
    setHighPowerSettings(_highPowerSettings);

    writeRegister(0x11, (readRegister(0x11) & 0x1F) | pa);
  }
}

/**
 * Set the output power level of the RFM69 module.
 *
 * @param power Power level from 0 to 31.
 */
void RFM69::setPowerLevel(uint8_t power)
{
  if (power > 31)
    power = 31;

  writeRegister(0x11, (readRegister(0x11) & 0xE0) | power);

  _powerLevel = power;
}

/**
 * Enable the +20 dBm high power settings of RFM69Hxx modules.
 *
 * @note Enabling only works with high power devices.
 *
 * @param enable true or false
 */
void RFM69::setHighPowerSettings(bool enable)
{
  // enabling only works if this is a high power device
  if (true == enable && false == _highPowerDevice)
    enable = false;

  writeRegister(0x5A, enable ? 0x5D : 0x55);
  writeRegister(0x5C, enable ? 0x7C : 0x70);
}

/**
 * Reconfigure the RFM69 module by writing multiple registers at once.
 *
 * @param config Array of register/value tuples
 * @param length Number of elements in config array
 */
void RFM69::setCustomConfig(const uint8_t config[][2], unsigned int length)
{
	uint8_t val;
	for (unsigned int i = 0; i < length; i++)
	{
	  uint8_t set_val = config[i][1];
	  writeRegister(config[i][0], config[i][1]);

	  uint8_t val = readRegister(config[i][0]);

	  if (set_val != val){
	      qDebug() << "Unable to write params";
          }


	}
}
uint8_t syncSentence[] = {'w', 'i', 'k', 'l', 'o', 's', 'o', 'f', 't', 0, 0};


int RFM69::sendPacket(uint8_t* packet, uint16_t len)
{
    uint8_t sequence = 0;

    printf("sendPacket %d\n", len);

    uint16_t bytesToBeSent = len;
    uint8_t* data = packet;

    syncSentence[9] = len >> 8;
    syncSentence[10] = len;

    if (sendWithAck(syncSentence, 11, sequence++) <0 ) return -1;

    while(bytesToBeSent > 0){
        if (bytesToBeSent > (RFM69_MAX_PAYLOAD-1)){
            if (sendWithAck(data, RFM69_MAX_PAYLOAD-1, sequence++) <0 ) return -1;
            bytesToBeSent -= RFM69_MAX_PAYLOAD-1;
            data += RFM69_MAX_PAYLOAD-1;
        }else{
            if (sendWithAck(data, bytesToBeSent, sequence++) <0 ) return -1;
            bytesToBeSent = 0;
        }
    }


}

int RFM69::receivePacket(uint8_t* buf, uint16_t maxSize){
    uint8_t ack;
    uint8_t* bufPos = buf;

    int bytesToReceive;

    int bytesReceived = _receive(buf, RFM69_MAX_PAYLOAD, &ack);

    if (bytesReceived < 0) return 0;

    qDebug() << "header len"<< bytesReceived;
    if (bytesReceived != sizeof(syncSentence)) return -1;

    if (ack != 0 ) return -1;

    bytesToReceive = (buf[9]<<8 | buf[10]);

    bytesReceived = 0;

    send(0, 0, ack);


    qDebug() << "receivePacket to receive"<< bytesToReceive;

    if (bytesToReceive > bytesReceived){
        while(bytesToReceive > 0){
            while(!isPacketReady()); // todo add timeout

            uint8_t expectedSeqNumber = ack+1;
            uint8_t chunk[RFM69_MAX_PAYLOAD];

            int l = _receive(chunk, RFM69_MAX_PAYLOAD, &ack);

            //printf("received = %d %d\n", l, ack);
            if (expectedSeqNumber != ack){
                printf("invalid ack %d %d\n", expectedSeqNumber, ack);
            }else{
                memcpy(bufPos, chunk, l);
                bufPos += l;
                bytesToReceive -=l;
                bytesReceived += l;
            }
            send(0, 0, ack);
        }
    }
    return bytesReceived;
}


int RFM69::sendWithAck(uint8_t* data, uint16_t len, uint8_t sequence){
    printf("sendWithAck %d %d\n", len, sequence);
    uint8_t buf[20];
    for (int i=0; i<10; i++)
    {
        send(data, len, sequence);

        for (int j=0; j<10; j++){
            if (isPacketReady()) break;
            printf("waiting for ack\n");
            rfm69hal_delay_ms(5);
        }
        uint8_t ackSeq;
        int res = _receive(buf, 20, &ackSeq);
        if (res >= 0){
            printf("received ack =%d\n", ackSeq );
            return len;
        }else{
            printf("ack not received, try again %d\n", i);
        }
    }
    return -1;
}
/**
 * Send a packet over the air.
 *
 * After sending the packet, the module goes to standby mode.
 * CSMA/CA is used before sending if enabled by function setCSMA() (default: off).
 *
 * @note A maximum amount of RFM69_MAX_PAYLOAD bytes can be sent.
 * @note This function blocks until packet has been sent.
 *
 * @param data Pointer to buffer with data
 * @param dataLength Size of buffer
 *
 * @return Number of bytes that have been sent
 */
int RFM69::send(uint8_t* data, unsigned int dataLength, uint8_t sequence)
{
//printf_("send seq=%d\n", sequence);
  if (RFM69_MODE_SLEEP != _mode)
  {
    setMode(RFM69_MODE_STANDBY);
    waitForModeReady();
  }

//  clearFIFO();

  /* Wait for a free channel, if CSMA/CA algorithm is enabled.
   * This takes around 1,4 ms to finish if channel is free */
//  if (true == _csmaEnabled)
//  {
//    // Restart RX
//    writeRegister(0x3D, (readRegister(0x3D) & 0xFB) | 0x20);
//
//    // switch to RX mode
//    setMode(RFM69_MODE_RX);
//
//    // wait until RSSI sampling is done; otherwise, 0xFF (-127 dBm) is read
//
//    // RSSI sampling phase takes ~960 µs after switch from standby to RX
//    uint32_t timeEntry = mstimer_get();
//    while (((readRegister(0x23) & 0x02) == 0) && ((mstimer_get() - timeEntry) < 10));
//
//    while ((false == channelFree()) && ((mstimer_get() - timeEntry) < TIMEOUT_CSMA_READY))
//    {
//      // wait for a random time before checking again
//      delay_ms(10);
//
//      /* try to receive packets while waiting for a free channel
//       * and put them into a temporary buffer */
//      int bytesRead;
//      if ((bytesRead = _receive(_rxBuffer, RFM69_MAX_PAYLOAD)) > 0)
//      {
//        _rxBufferLength = bytesRead;
//
//        // module is in RX mode again
//
//        // Restart RX and wait until RSSI sampling is done
//        writeRegister(0x3D, (readRegister(0x3D) & 0xFB) | 0x20);
//        uint32_t timeEntry = mstimer_get();
//        while (((readRegister(0x23) & 0x02) == 0) && ((mstimer_get() - timeEntry) < 10));
//      }
//    }
//
//    setMode(RFM69_MODE_STANDBY);
//  }

  // transfer packet to FIFO
  chipSelect();

  uint8_t p[RFM69_MAX_PAYLOAD + 3];

  p[0] = 0x00 | 0x80;
  p[1] = dataLength+1;
  p[2] = sequence;

    for(int i=0; i<dataLength; i++){
        p[3+i] = data[i];
    }




  rfm69hal_transfer(p,dataLength+ 3);


  chipUnselect();
  setMode(RFM69_MODE_TX);

  waitForPacketSent();
  setMode(RFM69_MODE_STANDBY);
  return dataLength;
}

/**
 * Clear FIFO and flags of RFM69 module.
 */
void RFM69::clearFIFO()
{
  // clear flags and FIFO
  writeRegister(0x28, 0x10);
}

/**
 * Wait until the requested mode is available or timeout.
 */
void RFM69::waitForModeReady()
{
  uint32_t timeEntry = rfm69hal_get_timer_ms();

  while (((readRegister(0x27) & 0x80) == 0) && ((rfm69hal_get_timer_ms() - timeEntry) < TIMEOUT_MODE_READY));
}

/**
 * Put the RFM69 module to sleep (lowest power consumption).
 */
void RFM69::sleep()
{
  setMode(RFM69_MODE_SLEEP);
}

/**
 * Put the RFM69 module in RX mode and try to receive a packet.
 *
 * @note The module resides in RX mode.
 *
 * @param data Pointer to a receiving buffer
 * @param dataLength Maximum size of buffer
 * @return Number of received bytes; 0 if no payload is available.
 */
int RFM69::receive(uint8_t* data, unsigned int dataLength)
{
  // check if there is a packet in the internal buffer and copy it
  if (_rxBufferLength > 0)
  {
    // copy only until dataLength, even if packet in local buffer is actually larger
    memcpy(data, _rxBuffer, dataLength);

    unsigned int bytesRead = _rxBufferLength;

    // empty local buffer
    _rxBufferLength = 0;

    return bytesRead;
  }
  else
  {
    // regular receive
    return _receive(data, dataLength, 0);
  }
}

/**
 * Put the RFM69 module in RX mode and try to receive a packet.
 *
 * @note This is an internal function.
 * @note The module resides in RX mode.
 *
 * @param data Pointer to a receiving buffer
 * @param dataLength Maximum size of buffer
 * @return Number of received bytes; 0 if no payload is available.
 */
bool RFM69::isFifoNotEmpty(){
    uint8_t reg = readRegister(0x28);
    return reg & 0x40;
}

bool RFM69::isPacketReady(){
    if (RFM69_MODE_RX != _mode)
    {
        setMode(RFM69_MODE_RX);
        waitForModeReady();
    }

    uint8_t reg = readRegister(0x28);
    return reg & 0x4;
}

int RFM69::read(uint8_t* data, uint16_t dataLength)
{
    unsigned int bytesRead = 0;

    while (bytesRead < dataLength)
    {
        //wait for bytes
        while (!isFifoNotEmpty());

        data[bytesRead] = readRegister(0x00);
        bytesRead++;
    }

    return bytesRead;
}


int RFM69::_receive(uint8_t* data, unsigned int dataLength, uint8_t* sequence)
{
  // go to RX mode if not already in this mode
  if (RFM69_MODE_RX != _mode)
  {
    setMode(RFM69_MODE_RX);
    waitForModeReady();
  }

  // check for flag PayloadReady
  if (readRegister(0x28) & 0x04)
  {
    setMode(RFM69_MODE_STANDBY);
    unsigned int bytesRead = 0;
    uint8_t bytesInChunk = readRegister(0) -1; //minus seq byte
    *sequence = readRegister(0);
qDebug() << "size" << bytesInChunk;
qDebug() << "ack" << *sequence;



for(int i=0; i<bytesInChunk; i++){
	data[i] = readRegister(0);
}

    if (true == _autoReadRSSI)
    {
      readRSSI();
    }

    setMode(RFM69_MODE_RX);
    return bytesInChunk;
  }
  else
    return -1;
}

/**
 * Enable and set or disable AES hardware encryption/decryption.
 *
 * The AES encryption module will be disabled if an invalid key or key length
 * is passed to this function (aesKey = 0 or keyLength != 16).
 * Otherwise encryption will be enabled.
 *
 * The key is stored as MSB first in the RF module.
 *
 * @param aesKey Pointer to a buffer with the 16 byte AES key
 * @param keyLength Number of bytes in buffer aesKey; must be 16 bytes
 * @return State of encryption module (false = disabled; true = enabled)
 */
bool RFM69::setAESEncryption(const void* aesKey, unsigned int keyLength)
{
  bool enable = false;

  // check if encryption shall be enabled or disabled
  if ((0 != aesKey) && (16 == keyLength))
    enable = true;

  // switch to standby
  setMode(RFM69_MODE_STANDBY);

  if (true == enable)
  {
    // transfer AES key to AES key register
    chipSelect();

    // address first AES MSB register

    uint8_t p[keyLength +1];

    p[0] = 0x3E | 0x80;

    for (unsigned int i = 0; i < keyLength; i++)
        p[1+i] = (((uint8_t*)aesKey)[i]);

    rfm69hal_transfer(p, keyLength +1);

    chipUnselect();
  }

  // set/reset AesOn Bit in packet config
  writeRegister(0x3D, (readRegister(0x3D) & 0xFE) | (enable ? 1 : 0));

  return enable;
}

/**
 * Wait until packet has been sent over the air or timeout.
 */
void RFM69::waitForPacketSent()
{
  uint32_t timeEntry = rfm69hal_get_timer_ms();

  while (((readRegister(0x28) & 0x08) == 0) && ((rfm69hal_get_timer_ms() - timeEntry) < TIMEOUT_PACKET_SENT));
}

/**
 * Read the last RSSI value.
 *
 * @note Only if the last RSSI value was above the RSSI threshold, a sample can be read.
 *       Otherwise, you always get -127 dBm. Be also careful if you just switched to RX mode.
 *       You may have to wait until a RSSI sample is available.
 *
 * @return RSSI value in dBm.
 */
int RFM69::readRSSI()
{
  _rssi = -readRegister(0x24) / 2;

  return _rssi;
}

/**
 * Debug function to dump all RFM69 registers.
 *
 * Symbol 'DEBUG' has to be defined.
 */
void RFM69::dumpRegisters(void)
{
#ifdef DEBUG
  for (unsigned int i = 1; i <= 0x71; i++)
  {
    printf("[0x%X]: 0x%X\n", i, readRegister(i));
  }
#endif
}

/**
 * Enable/disable OOK modulation (On-Off-Keying).
 *
 * Default modulation is FSK.
 * The module is switched to standby mode if RX or TX was active.
 *
 * @param enable true or false
 */
void RFM69::setOOKMode(bool enable)
{
  // switch to standby if TX/RX was active
  if (RFM69_MODE_RX == _mode || RFM69_MODE_TX == _mode)
    setMode(RFM69_MODE_STANDBY);

  if (false == enable)
  {
    // FSK
    writeRegister(0x02, (readRegister(0x02) & 0xE7));
  }
  else
  {
    // OOK
    writeRegister(0x02, (readRegister(0x02) & 0xE7) | 0x08);
  }

  _ookEnabled = enable;
}

/**
 * Set the output power level in dBm.
 *
 * This function takes care of the different PA settings of the modules.
 * Depending on the requested power output setting and the available module,
 * PA0, PA1 or PA1+PA2 is enabled.
 *
 * @param dBm Output power in dBm
 * @return 0 if dBm valid; else -1.
 */
int RFM69::setPowerDBm(int8_t dBm)
{
  /* Output power of module is from -18 dBm to +13 dBm
   * in "low" power devices, -2 dBm to +20 dBm in high power devices */
  if (dBm < -18 || dBm > 20)
    return -1;

  if (false == _highPowerDevice && dBm > 13)
    return -1;

  if (true == _highPowerDevice && dBm < -2)
    return -1;

  uint8_t powerLevel = 0;

  if (false == _highPowerDevice)
  {
    // only PA0 can be used
    powerLevel = dBm + 18;

    // enable PA0 only
    writeRegister(0x11, 0x80 | powerLevel);
  }
  else
  {
    if (dBm >= -2 && dBm <= 13)
    {
      // use PA1 on pin PA_BOOST
      powerLevel = dBm + 18;

      // enable PA1 only
      writeRegister(0x11, 0x40 | powerLevel);

      // disable high power settings
      _highPowerSettings = false;
      setHighPowerSettings(_highPowerSettings);
    }
    else if (dBm > 13 && dBm <= 17)
    {
      // use PA1 and PA2 combined on pin PA_BOOST
      powerLevel = dBm + 14;

      // enable PA1+PA2
      writeRegister(0x11, 0x60 | powerLevel);

      // disable high power settings
      _highPowerSettings = false;
      setHighPowerSettings(_highPowerSettings);
    }
    else
    {
      // output power from 18 dBm to 20 dBm, use PA1+PA2 with high power settings
      powerLevel = dBm + 11;

      // enable PA1+PA2
      writeRegister(0x11, 0x60 | powerLevel);

      // enable high power settings
      _highPowerSettings = true;
      setHighPowerSettings(_highPowerSettings);
    }
  }

  return 0;
}

/**
 * Check if the channel is free using RSSI measurements.
 *
 * This function is part of the CSMA/CA algorithm.
 *
 * @return true = channel free; otherwise false.
 */
bool RFM69::channelFree()
{
  if (readRSSI() < CSMA_RSSI_THRESHOLD)
  {
    return true;
  }
  else
  {
    return false;
  }
}

/** @}
 *
 */