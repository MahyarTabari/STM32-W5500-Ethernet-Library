/*
 * w5500_host_config.h
 *
 *  Created on: Jul 16, 2024
 *      Author: mahyar
 */

#ifndef SRC_W5500_W5500_HOST_CONFIG_H_
#define SRC_W5500_W5500_HOST_CONFIG_H_

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


#endif /* SRC_W5500_W5500_HOST_CONFIG_H_ */
