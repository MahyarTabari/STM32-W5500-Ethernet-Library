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

### 3. Initialize the W5500

Initialize the W5500 by calling the initialization function defined in `w5500_spi.h`.

```c
#include "w5500_spi.h"

// Call this function to initialize the W5500
w5500_init();
```

## Example
**main.c** file contains an example on how to use the library.

   ##### Note:
This project can be used as a ready-to-use template.


# Web server

The firmware also serves a small web interface on port 80. It needs no interrupt
line, no `accept()` and no thread: everything runs from `http_server_poll()`, so
the main loop never blocks on the network.

```c
#include "http_server.h"

w5500_init();
dynamic_host_configuration(net_info.mac);
check_cable_presence();
http_server_init();

for (;;) {
    http_server_poll();
    host_configuration_keepalive();
}
```

- `http_server_init()` resets the connection pool. It does not open sockets: the
  controller needs a valid address first, so the sockets are opened lazily from
  `http_server_poll()`.
- `http_server_poll()` serves every pending connection and must be called often
  enough. The W5500 `INTn` pin is not wired on this board, so this is the only
  thing that makes the server notice an incoming connection.
- `host_configuration_keepalive()` runs the DHCP state machine, which is what
  renews the lease. Call it from the same loop, otherwise the address silently
  expires and the device disappears from the network. It does nothing in static
  mode.

## Routes

| Method | Path         | Result                                                  |
| ------ | ------------ | ------------------------------------------------------- |
| `GET`  | `/`          | Dashboard, refreshed by JavaScript polling `/api/status` |
| `GET`  | `/api/status`| Uptime, request count, free stack, PHY, address mode     |
| `GET`  | `/api/net`   | Current MAC, IP, mask, gateway and DNS                   |
| `POST` | `/api/net`   | Writes a static address (`ip`, `sn`, `gw`, `dns`, `mac`)|
| `POST` | `/api/reboot`| Reboots after the response has been delivered            |

`POST /api/net` takes `application/x-www-form-urlencoded` fields, exactly what
the dashboard form posts. The MAC is optional: omitting it keeps the address
already in the controller, because a DHCP lease is bound to it.

Errors are reported as `400`, `404`, `405`, `413` and `500`. Every response is
sent with `Content-Length`, `Cache-Control: no-store` and `Connection: close`.

   ##### Note:
Writing an address drops any live connection, including the one that requested
the change. The server therefore applies the new address only after the
confirmation has been delivered, and re-arms the listening socket with it. Be
prepared for the browser to reload to a different URL after `POST /api/net`.


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

- `void host_configuration_keepalive()`
  - **Description**: Maintains the current address mode, call it from the main loop.
  - **Details**: In DHCP mode this runs the DHCP state machine, which is what renews the lease. Without it the address is only valid until the lease expires. Does nothing in static mode.

- `dhcp_mode host_configuration_mode()`
  - **Description**: Reports whether the address came from DHCP or was written statically.
  - **Details**: Tracked in software rather than read back from the controller, because the ioLibrary keeps the DNS server in RAM only.

### **http_server.h**
- `void http_server_init()`
  - **Description**: Resets the connection pool.
  - **Details**: Does not open sockets, they are opened lazily by `http_server_poll()` once the controller has an address.

- `void http_server_poll()`
  - **Description**: Serves all pending connections, non-blocking.
  - **Details**: Accepts connections, parses the request, and streams the response using `getSn_TX_FSR()` as the backpressure signal. Four sockets are shared, one of which is reserved for DHCP.

- `uint32_t http_server_requests()`
  - **Description**: Number of requests served since boot.
  - **Details**: Reported by `/api/status` and the dashboard.


## Notes To Consider

1. If you are configuring with cubeMX, remember adding the below line of code to the `MX_SPI1_Init()` function
    ```c
    __HAL_SPI_ENABLE(&hspi1);
    ```
2. Remember to include `dhcp.h` header to `stm32fxxxx.c` file and add the below code to `SysTick_Handler()` function:
    ```c
    static uint16_t ticks = 0;
    ticks++;
    if(ticks == 1000) {
        DHCP_time_handler();
        ticks = 0;
    }
    ```
3. Modify includes for your microcontroller in below files:
    - **w5500_phy.c**
    - **w5500_spi.c**

## License

This project is licensed under the MIT License. See the LICENSE file for details.
