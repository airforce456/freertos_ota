/**
 * @file    boot_spi.c
 * @brief   Minimal SPI1 driver for Bootloader (no HAL SPI dependency)
 *          Only implements what W25Q64 needs: TransmitReceive, Mode0, 4MHz
 */
#include "stm32f1xx_hal.h"

#define CS_PORT  GPIOA
#define CS_PIN   GPIO_PIN_4

void Boot_SPI_CS_Low(void)  { CS_PORT->BRR  = CS_PIN; }
void Boot_SPI_CS_High(void) { CS_PORT->BSRR = CS_PIN; }

/* Init SPI1 + CS pin */
void Boot_SPI_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA5=SCK, PA7=MOSI: AF Push-Pull */
    GPIO_InitTypeDef cfg = {0};
    cfg.Pin   = GPIO_PIN_5 | GPIO_PIN_7;
    cfg.Mode  = GPIO_MODE_AF_PP;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &cfg);

    /* PA6=MISO: Input floating */
    cfg.Pin  = GPIO_PIN_6;
    cfg.Mode = GPIO_MODE_INPUT;
    cfg.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &cfg);

    /* PA4=CS: Output push-pull, high */
    cfg.Pin  = CS_PIN;
    cfg.Mode = GPIO_MODE_OUTPUT_PP;
    cfg.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(CS_PORT, &cfg);
    Boot_SPI_CS_High();

    /* SPI1: Master, Mode0, BaudRate=fPCLK/2=4MHz */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE;
}

/* Send & receive one byte */
uint8_t Boot_SPI_Transfer(uint8_t tx)
{
    *(volatile uint8_t *)&SPI1->DR = tx;
    while (!(SPI1->SR & SPI_SR_RXNE));
    return *(volatile uint8_t *)&SPI1->DR;
}
