#include "stm32f4xx_hal.h"
#include "wizchip_conf.h"
#include "w5500_spi.h"
#include "stdio.h"

extern SPI_HandleTypeDef hspi1;

/**
 *@brief Helper driver to send and receive one byte through SPI
 *@param data   : 1 byte data to transmit
 *@return		: 1 byte data which is received
 */
uint8_t SPIReadWrite(uint8_t data)
{

	/*
	 * The transmit sequence begins when a byte is written in the Tx Buffer
	 * The data byte is parallel-loaded into the shift register
	 * (from the internal bus) during the first
	 * bit transmission and then shifted out serially
	 * to the MOSI pin MSB first or LSB first
	 * depending on the LSBFIRST bit in the SPI_CR1 register.
	 * The TXE flag is set on the transfer
	 * of data from the Tx Buffer to the shift register and an interrupt is generated if the TXEIE bit in
	 * the SPI_CR2 register is set.
	 */

	// check TXE(tx empty buffer) bit of the status register(SR),
	// and wait until the tx buffer becomes empty
	while((hspi1.Instance->SR & SPI_FLAG_TXE) != SPI_FLAG_TXE);



	// write the data to the data register to be sent
	*(__IO uint8_t*)&hspi1.Instance->DR = data;



	/*
	 * 	"Receive sequence" Page 569 of STM32F401xC reference manual
	 *
	 *	 * For the receiver,
	 *	 * when data transfer is complete:
	 *	 * The data in the shift register is transferred to the RX Buffer
	 *	 * and the RXNE flag is set
	 */

	// wait until the data from the slave is received,
	// and the RXNE flag is set
	while((hspi1.Instance->SR & SPI_FLAG_RXNE) != SPI_FLAG_RXNE);


	// return the received byte
	return 	(*(__IO uint8_t*)&hspi1.Instance->DR);
}

/**
 *@brief Helper driver to deselect wizchip
 */
void wizchip_deselect()
{
	HAL_GPIO_WritePin(SCS_PORT, SCS_PIN, GPIO_PIN_SET);
}

/**
 *@brief Helper driver to select wizchip
 */
void wizchip_select()
{
	HAL_GPIO_WritePin(SCS_PORT, SCS_PIN, GPIO_PIN_RESET);
}

/**
 *@brief Helper driver to read one byte through SPI
 *@return		: 1 byte data which is received
 */
uint8_t wizchip_read()
{
	// pass a dummy variable and read from the spi
	return SPIReadWrite(0x00);
}

/**
 *@brief Helper driver to send one byte through SPI
 *@param data   : 1 byte data to transmit
 */
void wizchip_write(uint8_t data)
{
	SPIReadWrite(data);
}

/**
 *@brief Helper driver to read burst of bytes through SPI
 *@param buff   : buffer to put the received data
 *@param len    : number of bytes to read
 */
void wizchip_read_burst(uint8_t* buff, uint16_t len)
{
	for (int i = 0; i < len; i++)
	{
		buff[i] = wizchip_read();
	}

}


/**
 *@brief Helper driver to read burst of bytes through SPI
 *@param data   : array containing data to be sent
 *@param len    : number of bytes to send
 */
void wizchip_write_burst(uint8_t* data, uint16_t len)
{
	for (int i = 0; i < len; i++)
	{
		wizchip_write(data[i]);
	}
}


/**
 *@brief initializer of PA0(RESET) and SCS(PA1)
 */

void w5500_pins_init()
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};


	// enable GPIO clock
	__HAL_RCC_GPIOA_CLK_ENABLE();

	// configuring RESET and SCS as outputs
	GPIO_InitStruct.Pin = RESET_PIN | SCS_PIN;

	// push pull(high if 1 and low if 0)
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;

	// output is NOPULL
	GPIO_InitStruct.Pull = GPIO_NOPULL;

	// mark these pins fast
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;


	// initialize the pins
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void w5500_init()
{
	// the available size of w5500 is 32KB
	// w5500 supports 8 independents sockets simultaneously
	// here 2KB is allocated for
	// each of the transmit and receive buffer of each socket
	uint8_t memory_for_each_socket[2][8] =
	{
			{2, 2, 2, 2, 2, 2, 2, 2},		// receive buffer size
			{2, 2, 2, 2, 2, 2, 2, 2}		// transmit buffer size
	};

	uint8_t tmp = 0xFF;


	// initialize RESET and SCS pins
	w5500_pins_init();

	// first deselect the chip(by setting SCS pin)
	HAL_GPIO_WritePin(SCS_PORT, SCS_PIN, GPIO_PIN_SET);


	// hard reset the wiznet chip by clearing RESET
	HAL_GPIO_WritePin(RESET_PORT, RESET_PIN, GPIO_PIN_RESET);

	// busy wait for a while and keep the RESET pin 0
	while(tmp--);


	HAL_GPIO_WritePin(RESET_PORT, RESET_PIN, GPIO_PIN_SET);

	// assign the helper drivers to complete the driver

	// callback functions for selecting and deselecting the chip
	reg_wizchip_cs_cbfunc(wizchip_select, wizchip_deselect);

	// callback functions for reading and writing bytes of data
	reg_wizchip_spi_cbfunc(wizchip_read, wizchip_write);

	// callback functions for reading and writing burst of data
	reg_wizchip_spiburst_cbfunc(wizchip_read_burst, wizchip_write_burst);



	// allocate the specified memory for each socket
	if (ctlwizchip(CW_INIT_WIZCHIP, (void*)memory_for_each_socket) == -1)
	{
		printf("WIZCHIP initialization failed\r\n");
		while(1);
	}

	printf("WIZCHIP initialization done successfully\r\n");
}

