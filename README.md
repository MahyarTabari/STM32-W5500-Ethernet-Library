# WIZnet W5500 Library for STM32

This repository contains a library for interfacing with the WIZnet W5500 Ethernet controller using an STM32 microcontroller. The library includes functions for SPI initialization, PHY status checking, and retrieving the current network configuration.


## Usage

To use this library in your STM32 project, follow the steps below:

### 1. Pin Configuration
   - w5500 module:
        ```
        3.3V		3.3
        GND		    GND
        MISO		PA6
        RST		    PA0
        MOSI		PA7
        SCS		    PA1
        SCLK		PA5
        ```
        #### Note:
        You can change `RST` and `SCS` pins via the header file **w5500_spi.h**.
### 2. Project Setup
Copy `w5500` directory to the *Core/Src/* .

### 2. Initialize the W5500

Initialize the W5500 by calling the initialization function defined in `w5500_spi.h`.

```c
#include "w5500_spi.h"

// Call this function to initialize the W5500
w5500_init();
```
# Available API

### **w5500_spi.h**
- `void w5500_init()`
  - **Description**: Initializes the W5500 module.
  - **Details**: This function sets up the necessary SPI configuration and initializes the W5500 hardware for communication.

### **w5500_phy.h**
- `void check_cable_presence()`
  - **Description**: Checks if the Ethernet cable is connected.
  - **Details**: Verifies the physical presence of the Ethernet cable and returns the status.
  
- `void check_phy_status()`
  - **Description**: Checks the PHY status of the W5500.
  - **Details**: Retrieves and prints the current PHY status, including link status, speed, and duplex mode.
  
- `void print_current_host_configuration()`
  - **Description**: Prints the current network configuration of the host.
  - **Details**: Retrieves and displays the MAC address, IP address, subnet mask, gateway, and DNS server currently configured on the W5500.

### **w5500_host_config.h**
- `void static_host_configuration(uint8_t mac[6], uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4])`
  - **Description**: Configures the W5500 with a static IP address.
  - **Details**: Sets up the network parameters including MAC address, IP address, subnet mask, gateway, and DNS server for a static network configuration.
  
- `void dynamic_host_configuration(uint8_t mac[6])`
  - **Description**: Configures the W5500 using DHCP for dynamic IP assignment.
  - **Details**: Sets up the MAC address and initiates the DHCP process to obtain an IP address, subnet mask, gateway, and DNS server from a DHCP server.


## Notes To Consider

1. If you are configuring with cubeMX, remember adding the below line of code to the `MX_SPI1_Init()` function
    ```c
    __HAL_SPI_ENABLE(&hspi1);
    ```
2. remember to include `dhcp.h` header to `stm32fxxxx.c` file and add the below code to `SysTick_Handler()` function:
    ```c
    static uint16_t ticks = 0;
    ticks++;
    if(ticks == 1000) {
        DHCP_time_handler();
        ticks = 0;
    }
    ```
3. modify includes for your microcontroller in below files:
    - **w5500_phy.c**
    - **w5500_spi.c**
    - 
4. **main.c** file contains an example on how to use the library.
## License

This project is licensed under the MIT License. See the LICENSE file for details.
