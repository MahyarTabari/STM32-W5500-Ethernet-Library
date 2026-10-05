/*
 * w5500_host_config.h
 *
 *  Created on: Jul 16, 2024
 *      Author: mahyar
 */

#ifndef SRC_W5500_W5500_HOST_CONFIG_H_
#define SRC_W5500_W5500_HOST_CONFIG_H_

#include <stdint.h>
#include "wizchip_conf.h"

/* Socket reserved for the DHCP client. HTTP_SERVER_BASE_SOCKET in
   http_server.h must not collide with it. */
#define DHCP_SOCKET     0


void static_host_configuration
(
   uint8_t mac[6],  ///< Source MAC Address
   uint8_t ip[4],   ///< Source IP Address
   uint8_t sn[4],   ///< Subnet Mask
   uint8_t gw[4],   ///< Gateway IP Address
   uint8_t dns[4]   ///< DNS server IP Address
);

void dynamic_host_configuration(uint8_t mac[6]);

/**
 * @brief Address mode the host is currently running in.
 * @details Tracked here rather than read back from the controller, because the
 *          ioLibrary keeps the DNS server in RAM only and the chip register
 *          cannot be distinguished between a static and a leased address.
 * @retval NETINFO_DHCP after dynamic_host_configuration()
 * @retval NETINFO_STATIC after static_host_configuration()
 */
dhcp_mode host_configuration_mode(void);

/**
 * @brief Maintains the current address mode. Call it from the main loop.
 * @details In DHCP mode it runs the DHCP state machine, which is what renews
 *          the lease. Without this call the address is only valid until the
 *          lease expires and the device silently disappears from the network.
 *          In static mode it does nothing, so the call can be unconditional.
 * @note The ioLibrary busy waits for up to two seconds inside DHCP_run() on the
 *       DECLINE path, so a single call can delay the caller briefly.
 */
void host_configuration_keepalive(void);

#endif /* SRC_W5500_W5500_HOST_CONFIG_H_ */
