/*
 * http_server.h
 *
 * Minimal single-file HTTP/1.1 server for the W5500 port.
 *
 * The server is built on the WIZnet socket API, which has no accept(): a socket
 * that was opened with Sn_MR_LISTEN becomes the data socket as soon as the peer
 * completes the handshake, so the listening socket has to be re-created for the
 * next client. That, plus the absence of a wired INTn line, makes a polling
 * state machine the natural shape for this driver.
 */

#ifndef SRC_W5500_HTTP_SERVER_H_
#define SRC_W5500_HTTP_SERVER_H_

#include <stdint.h>

/* Listening port of the web interface. */
#define HTTP_SERVER_PORT          80

/* Socket numbers are assigned from HTTP_SERVER_BASE_SOCKET upwards. Socket 0 is
   reserved for the DHCP client (see DHCP_SOCKET in w5500_host_config.h), so the
   web server must not claim it. */
#define HTTP_SERVER_BASE_SOCKET   1

/* Number of sockets dedicated to HTTP. The W5500 provides 8 sockets in total, so
   keep this at or below 7. Browsers open up to six parallel connections per
   origin, so a value of 1 makes pages stall while the dashboard fetches data. */
#define HTTP_SERVER_MAX_CONN      4

/* Accumulation buffer for one request head plus body. A request that does not fit
   is answered with 413 instead of being truncated. */
#define HTTP_SERVER_RX_SIZE       1024

/* Per-connection staging area for values that change while the response is being
   transmitted (link state, uptime, socket table). Formatting those once when the
   request arrives keeps Content-Length correct even if the link drops midway
   through the response. */
#define HTTP_SERVER_SNAP_SIZE     128

/* Give up on a peer that opened a connection but stopped sending. Prevents a
   stuck socket from permanently consuming one of the eight hardware sockets. */
#define HTTP_SERVER_IDLE_MS       5000

/* Bounded so that one slow client cannot starve the others. */
#define HTTP_SERVER_RX_PER_POLL   4

/**
 * @brief Opens the listening sockets and resets the server statistics.
 * @note Must be called after the host has an IP address, otherwise the WIZnet
 *       socket layer refuses to open a TCP socket with SOCKERR_SOCKINIT.
 */
void http_server_init(void);

/**
 * @brief Advances the server state machine. Never blocks.
 * @details Call it as often as possible from the main loop. Each call moves every
 *          connection at most one step further, so a client sending a request
 *          while the link is being renegotiated cannot stall the rest.
 */
void http_server_poll(void);

/**
 * @brief Number of HTTP requests answered since http_server_init().
 */
uint32_t http_server_requests(void);

#endif /* SRC_W5500_HTTP_SERVER_H_ */
