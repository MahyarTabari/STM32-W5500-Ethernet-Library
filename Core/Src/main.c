#include "stdio.h"
#include "main.h"

//w5500 related
#include "w5500_spi.h"
#include "wizchip_conf.h"
#include "socket.h"

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart1;

// static host configuration
wiz_NetInfo net_info = {
    .mac = {0x02, 0x00, 0x00, 0xAB, 0xCD, 0xEF},  // Locally administered MAC address
    .ip = {192, 168, 1, 100},                     // IP address
    .sn = {255, 255, 255, 0},                     // Subnet mask
    .gw = {192, 168, 1, 1},                       // Gateway
    .dns = {8, 8, 8, 8},                          // DNS server
    .dhcp = NETINFO_STATIC                        // Use static IP
};


void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void write_data_uart1(const char data);

static void check_cable_presence();
static void check_phy_status();
void print_host_configuration(wiz_NetInfo current_net_info);

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  // don't buffer the output of printf
  setbuf(stdout, NULL);

  /* Configure the system clock */
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();

  // initialize w5500
  w5500_init();

  // static host configuration
  ctlnetwork(CN_SET_NETINFO, (void*) &net_info);

  check_cable_presence();

  check_phy_status();

  print_host_configuration(net_info);

  /* Infinite loop */
  while (1)
  {
	  // test printf
	  for (int i = 0; i < 100; i++)
	  {
		  printf("%i: Hello World!\r\n", i);
		  HAL_Delay(1500);
	  }
  }
}

/**
  * @brief send 1 byte of data to uart1
  * @retval None
  */
void write_data_uart1(const char data)
{
	// busy wait until transmit buffer becomes empty
	while((huart1.Instance->SR & UART_FLAG_TXE) != UART_FLAG_TXE);

	huart1.Instance->DR = data;

}

// overwrite __io_putchar function to redirect printf to UART1
int __io_putchar(int ch)
{
	write_data_uart1(ch);

	return ch;
}

// Function to check for cable presence
void check_cable_presence() {
	uint8_t phy_status;
	int error_status;

	printf("\r\nChecking ethernet cable presence ...\r\n");

	do
	{
		if((error_status = ctlwizchip(CW_GET_PHYLINK, (void*) &phy_status) == -1))
		{
			printf("Failed to get PHY link info.\r\nTrying again...\r\n");
			continue;
		}


		if(phy_status == PHY_LINK_OFF)
		{
			printf("Cable is not connected.\r\n");
			HAL_Delay(1500);
		}
	}while(error_status == -1 || phy_status == PHY_LINK_OFF);

	printf("Cable is connected.\r\n");
}


// Function to check and print the current PHY status
void check_phy_status() {
    uint8_t phy_status;

    // Get the PHY link status
    if (ctlwizchip(CW_GET_PHYLINK, (void*)&phy_status) == -1) {
        printf("Failed to get PHY link status.\r\n");
        return;
    }

    // Print the PHY link status
    if (phy_status == PHY_LINK_ON) {
        printf("PHY Link is ON.\r\n");
    } else {
        printf("PHY Link is OFF.\r\n");
    }

    // Get the PHY configuration (optional, for more detailed information)
    wiz_PhyConf phy_conf;
    if (ctlwizchip(CW_GET_PHYCONF, (void*)&phy_conf) == -1) {
        printf("Failed to get PHY configuration.\r\n");
        return;
    }

    // Print the PHY configuration
    printf("PHY Mode: ");
    switch (phy_conf.by) {
        case PHY_CONFBY_HW:
            printf("Configured by hardware.\r\n");
            break;
        case PHY_CONFBY_SW:
            printf("Configured by software.\r\n");
            break;
        default:
            printf("Unknown.\r\n");
            break;
    }

    printf("PHY Speed: ");
    switch (phy_conf.speed) {
        case PHY_SPEED_10:
            printf("10 Mbps.\r\n");
            break;
        case PHY_SPEED_100:
            printf("100 Mbps.\r\n");
            break;
        default:
            printf("Unknown.\r\n");
            break;
    }

    printf("PHY Duplex: ");
    switch (phy_conf.duplex) {
        case PHY_DUPLEX_HALF:
            printf("Half duplex.\r\n");
            break;
        case PHY_DUPLEX_FULL:
            printf("Full duplex.\r\n");
            break;
        default:
            printf("Unknown.\r\n");
            break;
    }

    // Print the PHY negotiation mode
    printf("PHY Negotiation Mode: ");
    switch (phy_conf.mode) {
        case PHY_MODE_MANUAL:
            printf("Manual.\r\n");
            break;
        case PHY_MODE_AUTONEGO:
            printf("Auto-negotiation.\r\n");
            break;
        default:
            printf("Unknown.\r\n");
            break;
    }}

void print_host_configuration(wiz_NetInfo current_net_info)
{
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", current_net_info.mac[0], current_net_info.mac[1], current_net_info.mac[2], current_net_info.mac[3], current_net_info.mac[4], current_net_info.mac[5]);
    printf("IP: %d.%d.%d.%d\r\n", current_net_info.ip[0], current_net_info.ip[1], current_net_info.ip[2], current_net_info.ip[3]);
    printf("SN: %d.%d.%d.%d\r\n", current_net_info.sn[0], current_net_info.sn[1], current_net_info.sn[2], current_net_info.sn[3]);
    printf("GW: %d.%d.%d.%d\r\n", current_net_info.gw[0], current_net_info.gw[1], current_net_info.gw[2], current_net_info.gw[3]);
    printf("DNS: %d.%d.%d.%d\r\n", current_net_info.dns[0], current_net_info.dns[1], current_net_info.dns[2], current_net_info.dns[3]);
}


/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */
  __HAL_SPI_ENABLE(&hspi1);
  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, RESET_Pin|SCS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : RESET_Pin SCS_Pin */
  GPIO_InitStruct.Pin = RESET_Pin|SCS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
