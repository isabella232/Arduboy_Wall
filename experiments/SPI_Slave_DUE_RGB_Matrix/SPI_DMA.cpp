// SPI slave for DUE, hack
//  master will lower CS and manage CLK
// MISO to MISO  MOSI to MOSI   CLK to CLK   CS to CS, common ground
//   http://forum.arduino.cc/index.php/topic,157203.0.html
// core hardware/arduino/sam/system/libsam/source/spi.c
// TODO:  C class, interrupt, FIFO/16-bit, DMA?
#include "SPI_DMA.h"
#include <Arduino.h>
#include <SPI.h>
#define NUM_DEVICES 2

// assumes MSB
static BitOrder bitOrder = MSBFIRST;


//uint8_t transmit_buffer2[SPI_BUFF_SIZE];

/** Use SAM3X DMAC if nonzero */
#define USE_SAM3X_DMAC 1
/** Use extra Bus Matrix arbitration fix if nonzero */
#define USE_SAM3X_BUS_MATRIX_FIX 0
/** Time in ms for DMA receive timeout */
//#define SAM3X_DMA_TIMEOUT 100
#define SAM3X_DMA_TIMEOUT 1

/** chip select register number */
#define SPI_CHIP_SEL 0
/** DMAC receive channel */
#define SPI_DMAC_RX_CH  1
/** DMAC transmit channel */
#define SPI_DMAC_TX_CH  0
/** DMAC Channel HW Interface Number for SPI TX. */
#define SPI_TX_IDX  1
/** DMAC Channel HW Interface Number for SPI RX. */
#define SPI_RX_IDX  2
//------------------------------------------------------------------------------
/** Disable DMA Controller. */
static void dmac_disable() {
  DMAC->DMAC_EN &= (~DMAC_EN_ENABLE);
}
/** Enable DMA Controller. */
static void dmac_enable() {
  DMAC->DMAC_EN = DMAC_EN_ENABLE;
}
/** Disable DMA Channel. */
static void dmac_channel_disable(uint32_t ul_num) {
  DMAC->DMAC_CHDR = DMAC_CHDR_DIS0 << ul_num;
}
/** Enable DMA Channel. */
static void dmac_channel_enable(uint32_t ul_num) {
  DMAC->DMAC_CHER = DMAC_CHER_ENA0 << ul_num;
}
/** Poll for transfer complete. */
static bool dmac_channel_transfer_done(uint32_t ul_num) {
  return (DMAC->DMAC_CHSR & (DMAC_CHSR_ENA0 << ul_num)) ? false : true;
}

//------------------------------------------------------------------------------
// start RX DMA
void spiDmaRX(uint8_t* dst, uint16_t count) {
  dmac_channel_disable(SPI_DMAC_RX_CH);
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_SADDR = (uint32_t)&SPI0->SPI_RDR;
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_DADDR = (uint32_t)dst;
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_DSCR =  0;
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_CTRLA = count |
      DMAC_CTRLA_SRC_WIDTH_BYTE | DMAC_CTRLA_DST_WIDTH_BYTE;
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_CTRLB = DMAC_CTRLB_SRC_DSCR |
      DMAC_CTRLB_DST_DSCR | DMAC_CTRLB_FC_PER2MEM_DMA_FC |
      DMAC_CTRLB_SRC_INCR_FIXED | DMAC_CTRLB_DST_INCR_INCREMENTING;
  DMAC->DMAC_CH_NUM[SPI_DMAC_RX_CH].DMAC_CFG = DMAC_CFG_SRC_PER(SPI_RX_IDX) |
      DMAC_CFG_SRC_H2SEL | DMAC_CFG_SOD | DMAC_CFG_FIFOCFG_ASAP_CFG;
  dmac_channel_enable(SPI_DMAC_RX_CH);
}
//------------------------------------------------------------------------------
// start TX DMA
void spiDmaTX(const uint8_t* src, uint16_t count) {
  static uint8_t ff = 0XFF;
  uint32_t src_incr = DMAC_CTRLB_SRC_INCR_INCREMENTING;
  if (!src) {
    src = &ff;
    src_incr = DMAC_CTRLB_SRC_INCR_FIXED;
  }
  dmac_channel_disable(SPI_DMAC_TX_CH);
  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_SADDR = (uint32_t)src;
  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_DADDR = (uint32_t)&SPI0->SPI_TDR;
  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_DSCR =  0;
  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_CTRLA = count |
      DMAC_CTRLA_SRC_WIDTH_BYTE | DMAC_CTRLA_DST_WIDTH_BYTE;

  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_CTRLB =  DMAC_CTRLB_SRC_DSCR |
      DMAC_CTRLB_DST_DSCR | DMAC_CTRLB_FC_MEM2PER_DMA_FC |
      src_incr | DMAC_CTRLB_DST_INCR_FIXED;

  DMAC->DMAC_CH_NUM[SPI_DMAC_TX_CH].DMAC_CFG = DMAC_CFG_DST_PER(SPI_TX_IDX) |
      DMAC_CFG_DST_H2SEL | DMAC_CFG_SOD | DMAC_CFG_FIFOCFG_ALAP_CFG;

  dmac_channel_enable(SPI_DMAC_TX_CH);
}

void slaveBegin(uint8_t _pin) {
  SPI.begin(_pin);
  SPI.setClockDivider (4, 1);

  REG_SPI0_CR = SPI_CR_SWRST;     // reset SPI
  REG_SPI0_CR = SPI_CR_SPIEN;     // enable SPI
  REG_SPI0_MR = SPI_MR_MODFDIS;     // slave and no modefault
  //REG_SPI0_CSR = 0x80;    // DLYBCT=0, DLYBS=0, SCBR=0, 8 bit transfer
  REG_SPI0_CSR = 0x00;    // DLYBCT=0, DLYBS=0, SCBR=0, 8 bit transfer

#if USE_SAM3X_DMAC
  pmc_enable_periph_clk(ID_DMAC);
  dmac_disable();
  DMAC->DMAC_GCFG = DMAC_GCFG_ARB_CFG_FIXED;
  dmac_enable();
#if USE_SAM3X_BUS_MATRIX_FIX
  MATRIX->MATRIX_WPMR = 0x4d415400;
  MATRIX->MATRIX_MCFG[1] = 1;
  MATRIX->MATRIX_MCFG[2] = 1;
  MATRIX->MATRIX_SCFG[0] = 0x01000010;
  MATRIX->MATRIX_SCFG[1] = 0x01000010;
  MATRIX->MATRIX_SCFG[7] = 0x01000010;
#endif  // USE_SAM3X_BUS_MATRIX_FIX
#endif  // USE_SAM3X_DMAC

}

//------------------------------------------------------------------------------
static inline uint8_t spiTransfer(uint8_t b) {
  Spi* pSpi = SPI0;

  pSpi->SPI_TDR = b;
  while ((pSpi->SPI_SR & SPI_SR_RDRF) == 0) {}
  b = pSpi->SPI_RDR;
  return b;
}
//------------------------------------------------------------------------------
/** SPI receive a byte */
static inline uint8_t spiRec() {
  return spiTransfer(0XFF);
}
//------------------------------------------------------------------------------
/** SPI receive multiple bytes */
static uint8_t spiRec(uint8_t* buf, size_t len) {
  Spi* pSpi = SPI0;
  int rtn = 0;
#if USE_SAM3X_DMAC
  // clear overrun error
  uint32_t s = pSpi->SPI_SR;

  spiDmaRX(buf, len);
  spiDmaTX(0, len);

  uint32_t m = millis();
  while (!dmac_channel_transfer_done(SPI_DMAC_RX_CH)) {
  }
  if (pSpi->SPI_SR & SPI_SR_OVRES) rtn |= 1;
#else  // USE_SAM3X_DMAC
  for (size_t i = 0; i < len; i++) {
    pSpi->SPI_TDR = 0XFF;
    while ((pSpi->SPI_SR & SPI_SR_RDRF) == 0) {}
    buf[i] = pSpi->SPI_RDR;
  }
#endif  // USE_SAM3X_DMAC
  return rtn;
}
//------------------------------------------------------------------------------
/** SPI send a byte */
static inline void spiSend(uint8_t b) {
  spiTransfer(b);
}
//------------------------------------------------------------------------------
static void spiSend(const uint8_t* buf, size_t len) {
  Spi* pSpi = SPI0;
#if USE_SAM3X_DMAC
  spiDmaTX(buf, len);
  while (!dmac_channel_transfer_done(SPI_DMAC_TX_CH)) {}
#else  // #if USE_SAM3X_DMAC
  while ((pSpi->SPI_SR & SPI_SR_TXEMPTY) == 0) {}
  for (size_t i = 0; i < len; i++) {
    pSpi->SPI_TDR = buf[i];
    while ((pSpi->SPI_SR & SPI_SR_TDRE) == 0) {}
  }
#endif  // #if USE_SAM3X_DMAC
  while ((pSpi->SPI_SR & SPI_SR_TXEMPTY) == 0) {}
  // leave RDR empty
  uint8_t b = pSpi->SPI_RDR;
}

//------------------------------------------------------------------------------
/** SPI receive multiple bytes */
uint8_t spiSendReceive(uint8_t* outBuf, uint8_t* inBuf, size_t len) {
  Spi* pSpi = SPI0;
  int rtn = 0;

  // clear overrun error
  uint32_t s = pSpi->SPI_SR;

  spiDmaRX(inBuf, len);
  spiDmaTX(outBuf, len);

  uint32_t m = millis();
  while (!dmac_channel_transfer_done(SPI_DMAC_RX_CH || !dmac_channel_transfer_done(SPI_DMAC_TX_CH))) {
  }
  
  if (pSpi->SPI_SR & SPI_SR_OVRES) {
//    Serial.println(" **** OVERRUN!!! ****");
    rtn |= 1;
  }

  while ((pSpi->SPI_SR & SPI_SR_TXEMPTY) == 0) {}
  // leave RDR empty
  uint8_t b = pSpi->SPI_RDR;

  return rtn;
}

#define PRREG(x) Serial.print(#x" 0x"); Serial.println(x,HEX)

void prregs() {
//  PRREG(REG_SPI0_MR);
//  PRREG(REG_SPI0_CSR);
//  PRREG(REG_SPI0_SR);
}

#define SS 10
void setupSlave() {


  slaveBegin(SS);
  prregs();  // debug


}

byte x = 0;
void loopz() {
//  Serial.println("Waiting");
//  spiSendReceive(transmit_buffer, receive_buffer, SPI_BUFF_SIZE);


}

