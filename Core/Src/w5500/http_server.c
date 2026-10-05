/*
 * http_server.c
 *
 * HTTP/1.1 server for the W5500 port.
 *
 * Two properties of the WIZnet socket layer shape the whole implementation:
 *
 *  - There is no accept(). A socket opened with Sn_MR_TCP and armed with
 *    listen() is converted by the chip into the data socket as soon as the peer
 *    completes the handshake, so the listening socket is consumed by the first
 *    client and has to be re-created afterwards. The server therefore keeps a
 *    small pool of always-listening sockets instead of one accept loop.
 *
 *  - The W5500 INTn line is not wired on this board, so there is no event to
 *    wait for. Everything is driven by http_server_poll() and by the socket
 *    status register.
 *
 * Responses are described as an array of segments that lives in flash. A first
 * pass over that array only counts bytes, which yields an exact Content-Length;
 * the second pass streams the same segments to the socket. That keeps large
 * HTML pages out of RAM and avoids chunked transfer encoding.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "w5500_host_config.h"
#include "http_server.h"

/* Longest request target accepted. Anything longer is a probe or a bug. */
#define HTTP_TARGET_MAX			48

/* Room for the generated response head. */
#define HTTP_HEAD_MAX			192

/* Largest formatted scalar. A colon separated MAC is the widest one at
   17 characters, and snprintf reports the length it would have needed even when
   it truncates, so the buffer has to be able to hold the full text. */
#define HTTP_FMT_MAX			20

/* Bytes pushed into one socket per poll. The transmit buffer is 2 KB per socket;
   pacing the writes keeps a large page from starving the other connections. */
#define HTTP_TX_BUDGET			1024

/* A body that does not fit the length field is a programming error in a route,
   not a client problem, so the reserved value doubles as an overflow flag. */
#define HTTP_BODY_MAX			0xFFFE
#define HTTP_BODY_OVERFLOW		0xFFFF

/* How long the last chunk of a response may stay stuck in the transmit buffer
   before the socket is recycled anyway. */
#define HTTP_DRAIN_TIMEOUT_MS	1000

/* ------------------------------------------------------------------ */
/* Response description                                               */
/* ------------------------------------------------------------------ */

/*
 * A response body is a list of segments. The list lives in flash and is shared
 * by every connection, which is why the per-connection values (addresses,
 * snapshot fragments) are addressed by kind instead of by pointer: the emitter
 * always reads them from the connection that is currently being served.
 *
 * SEG_SNAP fragments must be appended to the snapshot area in exactly the order
 * the segment list consumes them. The route handlers below keep the two in step.
 */
typedef enum {
	SEG_STR = 0,		/* NUL-terminated string, normally in flash */
	SEG_SNAP,			/* next snapshot fragment of this connection */
	SEG_MAC,			/* c->net_mac, colon separated hex */
	SEG_IP,				/* c->net_ip, dotted quad */
	SEG_MASK,			/* c->net_sn, dotted quad */
	SEG_GW,				/* c->net_gw, dotted quad */
	SEG_DNS,			/* c->net_dns, dotted quad */
	SEG_END				/* terminates a segment list */
} seg_kind_t;

/* The kind is wider than a byte on purpose: the list is walked with a plain
   comparison against the enum, so a truncated field would silently turn every
   segment into a zero length one. */
typedef struct {
	uint16_t		kind;
	const char		*str;		/* only used by SEG_STR */
} seg_t;

/* Connection life cycle. */
typedef enum {
	CONN_FREE = 0,	/* slot not armed yet */
	CONN_LISTEN,		/* armed, waiting for a peer */
	CONN_REQUEST,	/* peer is up, request is being collected */
	CONN_SEND,			/* response is being streamed out */
	CONN_CLOSE		/* response finished, socket about to be recycled */
} conn_state_t;

/* Which buffer the segment emitter is currently draining. */
typedef enum {
	SRC_NONE = 0,
	SRC_HEAD,			/* generated response head */
	SRC_FLASH,			/* string in flash */
	SRC_SNAP,			/* snapshot fragment */
	SRC_FMT				/* address rendered into c->fmt */
} src_kind_t;

typedef enum {
	METHOD_GET = 0,
	METHOD_POST,
	METHOD_UNKNOWN
} http_method_t;

typedef struct {
	uint8_t			state;
	uint8_t			sock;
	uint8_t			method;
	bool			reset_pending;	/* NVIC reset once the body has drained */
	bool			net_pending;	/* write this address once the body has drained */
	bool			snap_overflow;	/* a snapshot fragment did not fit */

	uint32_t		last_ms;		/* last progress, drives the idle timeout */
	uint8_t			peer_ip[4];

	/* request collection */
	uint16_t		rx_len;
	char			target[HTTP_TARGET_MAX];
	char			rx[HTTP_SERVER_RX_SIZE];

	/* response description */
	const seg_t		*segs;
	uint8_t			seg_idx;
	uint8_t			src;
	const char		*src_ptr;
	uint16_t		src_len;
	uint16_t		src_off;

	uint16_t		body_len;
	uint16_t		body_sent;
	uint16_t		head_sent;
	char			head[HTTP_HEAD_MAX];
	uint16_t		head_len;

	/* values captured when the request arrived */
	uint8_t			net_ip[4];
	uint8_t			net_sn[4];
	uint8_t			net_gw[4];
	uint8_t			net_dns[4];
	uint8_t			net_mac[6];
	uint16_t		snap_len;
	uint16_t		snap_off;
	char			snap[HTTP_SERVER_SNAP_SIZE];
	char			fmt[HTTP_FMT_MAX];

	/* address to be written once the confirmation has been delivered */
	uint8_t			pend_ip[4];
	uint8_t			pend_sn[4];
	uint8_t			pend_gw[4];
	uint8_t			pend_dns[4];
	uint8_t			pend_mac[6];
} http_conn_t;

static http_conn_t	g_conn[HTTP_SERVER_MAX_CONN];
static uint32_t		g_requests;
static uint8_t		g_arm_cursor;

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static uint16_t ip_text(const uint8_t *ip, char *out)
{
	return (uint16_t) snprintf(out, HTTP_FMT_MAX, "%u.%u.%u.%u",
							   ip[0], ip[1], ip[2], ip[3]);
}

static uint16_t mac_text(const uint8_t *mac, char *out)
{
	return (uint16_t) snprintf(out, HTTP_FMT_MAX, "%02X:%02X:%02X:%02X:%02X:%02X",
							   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Case-insensitive comparison over a fixed length, used for header names. */
static bool str_ieq(const char *a, const char *b, uint8_t n)
{
	while (n--) {
		char ca = *a++;
		char cb = *b++;

		if (ca >= 'A' && ca <= 'Z')
			ca = (char) (ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char) (cb - 'A' + 'a');

		if (ca != cb)
			return false;
	}

	return true;
}

static bool str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}

	return *a == *b;
}

/* Clears the per-request state. The socket number and the life cycle state are
   deliberately preserved so that this can be called on a live connection. */
static void conn_clear(http_conn_t *c)
{
	c->method			= METHOD_UNKNOWN;
	c->rx_len			= 0;
	c->target[0]		= '\0';
	c->rx[0]			= '\0';
	c->segs				= NULL;
	c->seg_idx			= 0;
	c->src				= SRC_NONE;
	c->src_ptr			= NULL;
	c->src_len			= 0;
	c->src_off			= 0;
	c->body_len			= 0;
	c->body_sent		= 0;
	c->head_sent		= 0;
	c->head[0]			= '\0';
	c->head_len			= 0;
	c->snap_len			= 0;
	c->snap_off			= 0;
	c->snap[0]			= '\0';
	c->reset_pending	= false;
	c->net_pending		= false;
	c->snap_overflow	= false;
}

/*
 * Appends one NUL-terminated fragment to the snapshot area of a connection.
 *
 * Everything whose textual length can change over time (uptime, link state,
 * counters) is rendered here when the request arrives, never while the response
 * is being transmitted. Otherwise the Content-Length computed by the counting
 * pass could disagree with the bytes actually sent, and the browser would either
 * truncate the page or wait for data that never arrives.
 */
static bool snap_add(http_conn_t *c, const char *fmt, ...)
{
	va_list	ap;
	int		n;

	if (c->snap_len >= sizeof(c->snap)) {
		/* The fragment is dropped and the route is answered with an error by
		   respond(). Leaving a truncated run behind would shift every later
		   fragment and corrupt the body. */
		c->snap_overflow	= true;
		return false;
	}

	va_start(ap, fmt);
	n = vsnprintf(c->snap + c->snap_len, sizeof(c->snap) - c->snap_len, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t) n >= sizeof(c->snap) - c->snap_len) {
		c->snap[c->snap_len]	= '\0';
		c->snap_overflow		= true;
		return false;
	}

	c->snap_len += (uint16_t) n;
	return true;
}

/* ------------------------------------------------------------------ */
/* Segment emitter                                                    */
/* ------------------------------------------------------------------ */

/* Renders an address into c->fmt and returns its length. */
static uint16_t fmt_render(http_conn_t *c, seg_kind_t kind)
{
	switch (kind) {
	case SEG_MAC:
		return mac_text(c->net_mac, c->fmt);

	case SEG_IP:
		return ip_text(c->net_ip, c->fmt);

	case SEG_MASK:
		return ip_text(c->net_sn, c->fmt);

	case SEG_GW:
		return ip_text(c->net_gw, c->fmt);

	case SEG_DNS:
		return ip_text(c->net_dns, c->fmt);

	default:
		return 0;
	}
}

/* Resolves the next segment into a contiguous source. False when the list is
   exhausted. */
static bool seg_load(http_conn_t *c)
{
	seg_kind_t kind;

	if (c->segs == NULL)
		return false;

	kind = c->segs[c->seg_idx].kind;
	if (kind == SEG_END)
		return false;

	switch (kind) {
	case SEG_STR:
		c->src_ptr	= c->segs[c->seg_idx].str;
		c->src_len	= (uint16_t) strlen(c->src_ptr);
		c->src		= SRC_FLASH;
		break;

	case SEG_SNAP:
		/* A fragment is a NUL-terminated run inside the snapshot area. The
		   cursor advances when a fragment is picked up, never while it is
		   being drained, so a fragment split across polls is not repeated. */
		if (c->snap_off >= c->snap_len) {
			c->src_ptr	= "";
			c->src_len	= 0;
		} else {
			c->src_ptr	= &c->snap[c->snap_off];
			c->src_len	= (uint16_t) strlen(c->src_ptr);
			c->snap_off += (uint16_t) (c->src_len + 1);
		}
		c->src		= SRC_SNAP;
		break;

	case SEG_MAC:
	case SEG_IP:
	case SEG_MASK:
	case SEG_GW:
	case SEG_DNS:
		c->src_len	= fmt_render(c, kind);
		c->src_ptr	= c->fmt;
		c->src		= SRC_FMT;
		break;

	default:
		c->src_ptr	= "";
		c->src_len	= 0;
		c->src		= SRC_NONE;
		break;
	}

	c->src_off = 0;
	return true;
}

/*
 * Counting pass. Walks the segment list and returns the exact body length so the
 * response head can carry a correct Content-Length.
 */
static uint16_t seg_measure(http_conn_t *c)
{
	uint32_t total = 0;

	c->seg_idx	= 0;
	c->snap_off	= 0;
	c->src		= SRC_NONE;

	while (seg_load(c)) {
		if (total + c->src_len > HTTP_BODY_MAX)
			return HTTP_BODY_OVERFLOW;

		total += c->src_len;
		c->seg_idx++;
	}

	/* Rewind for the streaming pass. */
	c->seg_idx	= 0;
	c->snap_off	= 0;
	c->src		= SRC_NONE;

	return (uint16_t) total;
}

/*
 * Streaming pass. Pushes as many bytes as the transmit buffer and the ioLibrary
 * accept, then returns so the other connections are serviced.
 */
static uint16_t seg_flush(http_conn_t *c, uint16_t budget)
{
	uint16_t	sent = 0;

	if (c->head_sent < c->head_len) {
		uint16_t	n = (uint16_t) (c->head_len - c->head_sent);
		int32_t		ret;

		if (n > budget)
			n = budget;
		if (n > getSn_TX_FSR(c->sock))
			n = getSn_TX_FSR(c->sock);
		if (n == 0)
			return 0;

		ret = send(c->sock, (uint8_t *) &c->head[c->head_sent], n);
		if (ret != (int32_t) n)
			return 0;		/* SOCK_BUSY: the previous chunk is still draining */

		c->head_sent += n;
		sent += n;
		budget -= n;
	}

	while (sent < budget) {
		uint16_t	n;
		int32_t		ret;

		if (c->src == SRC_NONE) {
			if (!seg_load(c))
				break;

			c->seg_idx++;

			if (c->src_len == 0) {
				c->src = SRC_NONE;
				continue;
			}
		}

		n = (uint16_t) (c->src_len - c->src_off);
		if (n > (uint16_t) (budget - sent))
			n = (uint16_t) (budget - sent);
		if (n > getSn_TX_FSR(c->sock))
			n = getSn_TX_FSR(c->sock);
		if (n == 0)
			break;			/* transmit buffer full, resume on the next poll */

		ret = send(c->sock, (uint8_t *) &c->src_ptr[c->src_off], n);
		if (ret != (int32_t) n)
			break;			/* SOCK_BUSY, or the peer disappeared */

		c->src_off += n;
		c->body_sent += n;
		sent += n;

		if (c->src_off == c->src_len)
			c->src = SRC_NONE;
	}

	return sent;
}

/* ------------------------------------------------------------------ */
/* Request collection and parsing                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint16_t	head_len;		/* bytes up to and including the blank line */
	uint16_t	body_off;		/* first body byte */
	uint16_t	body_len;		/* announced body length */
	bool		head_found;		/* the terminating blank line is present */
	bool		head_ready;		/* the request line parsed successfully */
	bool		complete;		/* head and the whole body are present */
	bool		oversized;		/* does not fit the receive buffer */
} http_request_t;

/* Locates the end of the request head ("\r\n\r\n"). Zero while incomplete. */
static uint16_t find_head_end(const char *buf, uint16_t len)
{
	uint16_t i;

	if (len < 4)
		return 0;

	for (i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' &&
			buf[i + 2] == '\r' && buf[i + 3] == '\n')
			return (uint16_t) (i + 4);
	}

	return 0;
}

/* Scans the header block for Content-Length, matching names at line starts. */
static uint16_t parse_content_length(const char *head, uint16_t head_len)
{
	uint16_t pos = 0;

	/* Skip the request line. */
	while (pos + 1 < head_len && !(head[pos] == '\r' && head[pos + 1] == '\n'))
		pos++;
	pos += 2;

	while (pos + 1 < head_len) {
		uint16_t	line_end = pos;
		uint16_t	name_end = pos;
		bool		digit	 = false;
		uint32_t	value	 = 0;

		while (line_end + 1 < head_len &&
			   !(head[line_end] == '\r' && head[line_end + 1] == '\n'))
			line_end++;

		while (name_end < line_end && head[name_end] != ':')
			name_end++;

		if ((uint16_t) (name_end - pos) == 14 &&
			str_ieq(&head[pos], "content-length", 14)) {
			uint16_t i = (uint16_t) (name_end + 1);

			while (i < line_end && (head[i] == ' ' || head[i] == '\t'))
				i++;

			for (; i < line_end; i++) {
				if (head[i] < '0' || head[i] > '9')
					break;

				value = value * 10 + (uint32_t) (head[i] - '0');
				digit = true;

				if (value > HTTP_BODY_MAX)
					return HTTP_BODY_MAX;
			}

			if (digit)
				return (uint16_t) value;
		}

		pos = (uint16_t) (line_end + 2);
	}

	return 0;
}

/* Parses the request line: METHOD SP TARGET SP HTTP/1.x. */
static bool parse_request_line(http_conn_t *c, uint16_t head_len)
{
	const char	*buf = c->rx;
	uint16_t	sp1, sp2, i;

	sp1 = 0;
	while (sp1 < head_len && buf[sp1] != ' ')
		sp1++;
	if (sp1 == 0 || sp1 + 1 >= head_len)
		return false;

	sp2 = (uint16_t) (sp1 + 1);
	while (sp2 < head_len && buf[sp2] != ' ')
		sp2++;
	if (sp2 >= head_len)
		return false;

	/* Only the two verbs the device actually serves are accepted; anything
	   else is answered with 405 instead of being silently treated as a GET. */
	if (sp1 == 3 && str_ieq(buf, "GET", 3))
		c->method = METHOD_GET;
	else if (sp1 == 4 && str_ieq(buf, "POST", 4))
		c->method = METHOD_POST;
	else
		c->method = METHOD_UNKNOWN;

	i = (uint16_t) (sp2 - sp1 - 1);
	if (i == 0 || i >= HTTP_TARGET_MAX)
		return false;

	memcpy(c->target, &buf[sp1 + 1], i);
	c->target[i] = '\0';

	/* The version is not used, but a request without one is not HTTP/1.x and
	   browsers do send it, so only its presence is checked here. */
	return (uint16_t) (head_len - sp2 - 1) >= 8;
}

/* Strict dotted quad: four decimal octets in 0..255, separated by exactly three
   dots, with nothing before or after. The digit counter is per octet, otherwise
   a legal four octet address would be rejected once four digits are consumed. */
static bool parse_ipv4(const char *s, uint16_t len, uint8_t out[4])
{
	uint16_t	i = 0;
	uint8_t		octet;

	for (octet = 0; octet < 4; octet++) {
		uint32_t	value = 0;
		uint8_t		digits = 0;

		while (i < len && s[i] >= '0' && s[i] <= '9') {
			value = value * 10 + (uint32_t) (s[i] - '0');

			if (++digits > 3 || value > 255)
				return false;

			i++;
		}

		if (digits == 0)
			return false;

		out[octet] = (uint8_t) value;

		if (octet < 3) {
			if (i >= len || s[i] != '.')
				return false;

			i++;
		}
	}

	return i == len;
}

/* Six hex octets, separated by ':' or '-'. */
static bool parse_mac(const char *s, uint16_t len, uint8_t out[6])
{
	uint16_t	i	 = 0;
	uint8_t		octet = 0;
	uint8_t		nibble = 0;
	uint8_t		acc	 = 0;

	for (; i < len && octet < 6; i++) {
		uint8_t nib;

		if (s[i] == ':' || s[i] == '-') {
			/* A separator is only legal between two complete octets. */
			if (nibble != 0 || octet == 0 || octet >= 6)
				return false;

			continue;
		}

		if (s[i] >= '0' && s[i] <= '9')
			nib = (uint8_t) (s[i] - '0');
		else if (s[i] >= 'a' && s[i] <= 'f')
			nib = (uint8_t) (s[i] - 'a' + 10);
		else if (s[i] >= 'A' && s[i] <= 'F')
			nib = (uint8_t) (s[i] - 'A' + 10);
		else
			return false;

		if (nibble == 0)
			acc = (uint8_t) (nib << 4);
		else
			acc = (uint8_t) (acc | nib);

		if (++nibble == 2) {
			out[octet++] = acc;
			nibble = 0;
		}
	}

	/* The loop stops at six octets, so a longer string has to be rejected
	   explicitly or its tail would be accepted as an extra separator. */
	return octet == 6 && nibble == 0 && i == len;
}

/*
 * Finds "key=" inside an application/x-www-form-urlencoded body. The search is
 * anchored to the start of the body and to every '&', so "ip=" cannot be matched
 * inside "macip=" or "subnet_ip=".
 */
static const char *form_find(const http_conn_t *c, uint16_t body_off,
							 uint16_t body_len, const char *key,
							 uint16_t *val_len)
{
	uint8_t		keylen = (uint8_t) strlen(key);
	uint16_t	i	 = 0;

	while (i < body_len) {
		uint16_t	start = i;
		uint16_t	eq;

		while (i < body_len && c->rx[body_off + i] != '&')
			i++;

		eq = start;
		while (eq < i && c->rx[body_off + eq] != '=')
			eq++;

		if (eq < i && (uint16_t) (eq - start) == keylen &&
			str_ieq(&c->rx[body_off + start], key, keylen)) {
			uint16_t vs = (uint16_t) (eq + 1);
			uint16_t ve = i;

			/* Trim an optional trailing carriage return of a CRLF body. */
			while (ve > vs && (c->rx[body_off + ve - 1] == '\r' ||
							   c->rx[body_off + ve - 1] == '\n'))
				ve--;

			*val_len = (uint16_t) (ve - vs);
			return &c->rx[body_off + vs];
		}

		if (i < body_len)
			i++;
	}

	return NULL;
}

/*
 * Reads whatever the peer has sent.
 *
 * The WIZnet recv() answers SOCK_BUSY, which is numerically zero, when a
 * non-blocking socket has no data yet, so a zero return must never be read as
 * "the peer closed the connection". A close only shows up in the socket status
 * register, and recv() reports it as the negative SOCKERR_SOCKSTATUS.
 */
static bool rx_fill(http_conn_t *c)
{
	uint8_t	drained = 0;
	bool	got		 = false;

	while (drained < HTTP_SERVER_RX_PER_POLL) {
		uint16_t	space = (uint16_t) (HTTP_SERVER_RX_SIZE - c->rx_len);
		int32_t		n;

		if (space == 0)
			break;

		n = recv(c->sock, (uint8_t *) &c->rx[c->rx_len], space);
		if (n == SOCK_BUSY)
			break;			/* nothing available yet */
		if (n < 0)
			break;			/* peer gone, the state machine will notice */

		c->rx_len += (uint16_t) n;
		got = true;
		drained++;
	}

	return got;
}

static void request_parse(http_conn_t *c, http_request_t *req)
{
	req->head_len	= find_head_end(c->rx, c->rx_len);
	req->head_found	= false;
	req->head_ready	= false;
	req->complete	= false;
	req->oversized	= false;
	req->body_off	= 0;
	req->body_len	= 0;

	if (req->head_len == 0)
		return;

	req->head_found	= true;

	if (!parse_request_line(c, req->head_len))
		return;

	req->head_ready	= true;
	req->body_off	= req->head_len;
	req->body_len	= parse_content_length(c->rx, req->head_len);

	if ((uint32_t) req->body_off + req->body_len > HTTP_SERVER_RX_SIZE) {
		req->oversized	= true;
		return;
	}

	req->complete = (c->rx_len >= (uint16_t) (req->body_off + req->body_len));
}

/* ------------------------------------------------------------------ */
/* Static responses                                                   */
/* ------------------------------------------------------------------ */

static const char *status_text(uint16_t code)
{
	switch (code) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 413: return "Payload Too Large";
	case 500: return "Internal Server Error";
	default:  return "OK";
	}
}

/* Generic page for the failures that are not worth a dedicated layout. The exact
   status is reported on the UART, which is where a developer looks anyway. */
static const char PAGE_ERROR[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Request rejected</title><style>"
"body{margin:0;padding:40px;background:#0f1419;color:#d6dde5;"
"font:15px/1.6 system-ui,sans-serif}"
"h1{font-size:22px;margin:0 0 8px}p{color:#7d8894;margin:0 0 4px}"
"a{color:#8ff0b4}</style></head><body>"
"<h1>Request rejected</h1>"
"<p>The request could not be parsed or is not allowed on this endpoint.</p>"
"<p><a href='/'>Back to the dashboard</a></p>"
"</body></html>";

static const seg_t SEG_ERROR[] = {
	{SEG_STR, PAGE_ERROR},
	{SEG_END, NULL}
};

static const char PAGE_NOT_FOUND[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<title>404 Not Found</title><style>"
"body{margin:0;padding:40px;background:#0f1419;color:#d6dde5;"
"font:15px/1.6 system-ui,sans-serif}"
"h1{font-size:22px;margin:0 0 6px}p{color:#7d8894;margin:0}"
"a{color:#8ff0b4}</style></head><body>"
"<h1>404 Not Found</h1>"
"<p>No handler is registered for this path.</p>"
"<p><a href='/'>Back to the dashboard</a></p>"
"</body></html>";

static const seg_t SEG_NOT_FOUND[] = {
	{SEG_STR, PAGE_NOT_FOUND},
	{SEG_END, NULL}
};

static const char PAGE_TOO_LARGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<title>413 Payload Too Large</title><style>"
"body{margin:0;padding:40px;background:#0f1419;color:#d6dde5;"
"font:15px/1.6 system-ui,sans-serif}"
"h1{font-size:22px;margin:0 0 6px}p{color:#7d8894;margin:0}"
"a{color:#8ff0b4}</style></head><body>"
"<h1>413 Payload Too Large</h1>"
"<p>The request does not fit the receive buffer.</p>"
"<p><a href='/'>Back to the dashboard</a></p>"
"</body></html>";

static const seg_t SEG_TOO_LARGE[] = {
	{SEG_STR, PAGE_TOO_LARGE},
	{SEG_END, NULL}
};

/* ------------------------------------------------------------------ */
/* Dashboard                                                          */
/* ------------------------------------------------------------------ */

static const char DASH_HEAD[] =
"<!doctype html>\n"
"<html lang='en'>\n<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>W5500 status</title>\n"
"<style>\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:#0f1419;color:#d6dde5;"
"font:15px/1.55 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}\n"
"header{padding:22px 24px;background:#161c23;border-bottom:1px solid #222c36}\n"
"header h1{margin:0;font-size:19px;font-weight:600}\n"
"header p{margin:5px 0 0;color:#7d8894;font-size:12.5px;"
"font-family:ui-monospace,Menlo,Consolas,monospace;word-break:break-all}\n"
"main{max-width:720px;margin:0 auto;padding:20px 18px 40px}\n"
"section{background:#161c23;border:1px solid #222c36;border-radius:9px;"
"padding:16px 18px;margin-bottom:15px}\n"
"h2{margin:0 0 12px;font-size:11.5px;text-transform:uppercase;"
"letter-spacing:.09em;color:#7d8894}\n"
"table{width:100%;border-collapse:collapse}\n"
"td{padding:7px 0;border-bottom:1px solid #1d2530;vertical-align:top}\n"
"tr:last-child td{border-bottom:0}\n"
"td:first-child{color:#7d8894;width:42%}\n"
"td:last-child{font-family:ui-monospace,Menlo,Consolas,monospace;"
"font-size:13.5px;word-break:break-all}\n"
"form{display:grid;grid-template-columns:1fr 1fr;gap:11px 14px}\n"
"label{display:block;font-size:12px;color:#7d8894;margin-bottom:4px}\n"
"input{width:100%;padding:8px 10px;border-radius:6px;border:1px solid #2b3644;"
"background:#0f1419;color:#d6dde5;font:inherit;font-size:13.5px;"
"font-family:ui-monospace,Menlo,Consolas,monospace}\n"
"input:focus{outline:none;border-color:#3f7f5e}\n"
"button{margin-top:14px;padding:9px 15px;border-radius:6px;"
"border:1px solid #2f6f4f;background:#1d4732;color:#8ff0b4;font:inherit;"
"font-size:14px;cursor:pointer}\n"
"button:hover{background:#265c41}\n"
"button.warn{border-color:#7a3b3b;background:#4a2020;color:#ffb0b0}\n"
"button.warn:hover{background:#612a2a}\n"
".note{color:#7d8894;font-size:12.5px;margin:12px 0 0}\n"
"#msg{min-height:1.5em;margin:11px 0 0;font-size:13px;color:#8ff0b4}\n"
"@media(max-width:520px){form{grid-template-columns:1fr}}\n"
"</style>\n</head>\n<body>\n";

static const char DASH_OPEN[] =
"<header><h1>W5500 Ethernet controller</h1><p>";

static const char DASH_IDLE[] =
"</p></header>\n<main>\n"
"<section><h2>Network</h2><table>\n"
"<tr><td>IP address</td><td>";

static const char DASH_ROW1[] =
"</td></tr>\n<tr><td>Subnet mask</td><td>";

static const char DASH_ROW2[] =
"</td></tr>\n<tr><td>Gateway</td><td>";

static const char DASH_ROW3[] =
"</td></tr>\n<tr><td>DNS server</td><td>";

static const char DASH_ROW4[] =
"</td></tr>\n<tr><td>MAC address</td><td>";

static const char DASH_ROW5[] =
"</td></tr>\n<tr><td>Address source</td><td id='mode'>";

static const char DASH_ROW6[] =
"</td></tr>\n</table></section>\n"
"<section><h2>Controller</h2><table>\n"
"<tr><td>PHY link</td><td id='phy'>";

static const char DASH_ROW7[] =
"</td></tr>\n<tr><td>Chip</td><td id='chip'>";

static const char DASH_ROW8[] =
"</td></tr>\n<tr><td>Uptime</td><td id='uptime'>";

static const char DASH_ROW9[] =
"</td></tr>\n<tr><td>Requests served</td><td id='reqs'>";

static const char DASH_ROW10[] =
"</td></tr>\n<tr><td>Free stack</td><td id='stack'>";

static const char DASH_FORM[] =
"</td></tr>\n</table></section>\n"
"<section><h2>Network settings</h2>\n"
"<form id='netform'>\n"
"<div><label for='f_ip'>IP address</label>"
"<input id='f_ip' name='ip' value='";

static const char DASH_FORM_IP[] =
"' required></div>\n"
"<div><label for='f_sn'>Subnet mask</label>"
"<input id='f_sn' name='sn' value='";

static const char DASH_FORM_SN[] =
"' required></div>\n"
"<div><label for='f_gw'>Gateway</label>"
"<input id='f_gw' name='gw' value='";

static const char DASH_FORM_GW[] =
"' required></div>\n"
"<div><label for='f_dns'>DNS server</label>"
"<input id='f_dns' name='dns' value='";

static const char DASH_FORM_DNS[] =
"' required></div>\n"
"</form>\n"
"<button type='button' id='apply'>Apply</button>\n"
"<p class='note'>The address is written to the controller immediately, so this\n"
"page only stays reachable at the new address. The message below reports where\n"
"to go next.</p>\n"
"<p id='msg'></p>\n"
"</section>\n"
"<section><h2>Maintenance</h2>\n"
"<button type='button' class='warn' id='reboot'>Reboot device</button>\n"
"</section>\n"
"</main>\n"
"<script>\n"
"var el=function(i){return document.getElementById(i);};\n"
"function put(i,v){var e=el(i);if(e){e.textContent=v;}}\n"
"function poll(){\n"
"  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json();})\n"
"    .then(function(j){\n"
"      put('phy',j.phy);put('chip',j.chip);\n"
"      put('uptime',j.uptime_s+' s');put('reqs',j.requests);\n"
"      put('stack',j.stack_free+' B');put('mode',j.mode);\n"
"    }).catch(function(){put('phy','unreachable');});\n"
"}\n"
"el('apply').addEventListener('click',function(){\n"
"  var q='ip='+el('f_ip').value+'&sn='+el('f_sn').value"
"+'&gw='+el('f_gw').value+'&dns='+el('f_dns').value;\n"
"  put('msg','applying...');\n"
"  fetch('/api/net',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q})\n"
"    .then(function(r){return r.json();})\n"
"    .then(function(j){put('msg',j.message);poll();})\n"
"    .catch(function(){put('msg','the request failed, the link may be down');});\n"
"});\n"
"el('reboot').addEventListener('click',function(){\n"
"  put('msg','rebooting...');\n"
"  fetch('/api/reboot',{method:'POST'}).catch(function(){});\n"
"});\n"
"poll();\n"
"setInterval(poll,3000);\n"
"</script>\n"
"</body>\n</html>\n";

/* The SEG_SNAP fragments are consumed in this order: address source, PHY link,
   chip, uptime, requests served, free stack. snapshot_dashboard() appends them
   in exactly the same order. */
static const seg_t DASH_SEGS[] = {
	{SEG_STR, DASH_HEAD},
	{SEG_STR, DASH_OPEN},
	{SEG_IP,  NULL},
	{SEG_STR, DASH_IDLE},
	{SEG_IP,  NULL},
	{SEG_STR, DASH_ROW1},
	{SEG_MASK, NULL},
	{SEG_STR, DASH_ROW2},
	{SEG_GW,  NULL},
	{SEG_STR, DASH_ROW3},
	{SEG_DNS, NULL},
	{SEG_STR, DASH_ROW4},
	{SEG_MAC, NULL},
	{SEG_STR, DASH_ROW5},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_ROW6},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_ROW7},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_ROW8},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_ROW9},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_ROW10},
	{SEG_SNAP, NULL},
	{SEG_STR, DASH_FORM},
	{SEG_IP,  NULL},
	{SEG_STR, DASH_FORM_IP},
	{SEG_MASK, NULL},
	{SEG_STR, DASH_FORM_SN},
	{SEG_GW,  NULL},
	{SEG_STR, DASH_FORM_GW},
	{SEG_DNS, NULL},
	{SEG_STR, DASH_FORM_DNS},
	{SEG_END, NULL}
};

/* ------------------------------------------------------------------ */
/* JSON API                                                           */
/* ------------------------------------------------------------------ */

static const char API_STATUS[] =
"{\"uptime_s\":";

static const char API_STATUS_1[] =
",\"requests\":";

static const char API_STATUS_2[] =
",\"stack_free\":";

static const char API_STATUS_3[] =
",\"phy\":\"";

static const char API_STATUS_4[] =
"\",\"mode\":\"";

static const char API_STATUS_5[] =
"\",\"chip\":\"";

static const char API_STATUS_6[] =
"\",\"net\":{\"mac\":\"";

static const char API_STATUS_7[] =
"\",\"ip\":\"";

static const char API_STATUS_8[] =
"\",\"mask\":\"";

static const char API_STATUS_9[] =
"\",\"gw\":\"";

static const char API_STATUS_10[] =
"\",\"dns\":\"";

static const char API_STATUS_11[] =
"\"}}\n";

/* Snapshot order: uptime, requests, free stack, PHY, mode, chip. */
static const seg_t API_SEGS[] = {
	{SEG_STR, API_STATUS},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_1},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_2},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_3},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_4},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_5},
	{SEG_SNAP, NULL},
	{SEG_STR, API_STATUS_6},
	{SEG_MAC, NULL},
	{SEG_STR, API_STATUS_7},
	{SEG_IP,  NULL},
	{SEG_STR, API_STATUS_8},
	{SEG_MASK, NULL},
	{SEG_STR, API_STATUS_9},
	{SEG_GW,  NULL},
	{SEG_STR, API_STATUS_10},
	{SEG_DNS, NULL},
	{SEG_STR, API_STATUS_11},
	{SEG_END, NULL}
};

static const char API_NET[] =
"{\"ok\":true,\"mac\":\"";

static const char API_NET_1[] =
"\",\"ip\":\"";

static const char API_NET_2[] =
"\",\"mask\":\"";

static const char API_NET_3[] =
"\",\"gw\":\"";

static const char API_NET_4[] =
"\",\"dns\":\"";

static const char API_NET_5[] =
"\"}\n";

static const seg_t API_NET_SEGS[] = {
	{SEG_STR, API_NET},
	{SEG_MAC, NULL},
	{SEG_STR, API_NET_1},
	{SEG_IP,  NULL},
	{SEG_STR, API_NET_2},
	{SEG_MASK, NULL},
	{SEG_STR, API_NET_3},
	{SEG_GW,  NULL},
	{SEG_STR, API_NET_4},
	{SEG_DNS, NULL},
	{SEG_STR, API_NET_5},
	{SEG_END, NULL}
};

static const char API_SAVED[] =
"{\"ok\":true,\"ip\":\"";

static const char API_SAVED_1[] =
"\",\"message\":\"saved. The controller is now reachable at ";

static const char API_SAVED_2[] =
".\"}\n";

static const seg_t API_SAVED_SEGS[] = {
	{SEG_STR, API_SAVED},
	{SEG_IP,  NULL},
	{SEG_STR, API_SAVED_1},
	{SEG_IP,  NULL},
	{SEG_STR, API_SAVED_2},
	{SEG_END, NULL}
};

static const char API_BAD_REQUEST[] =
"{\"ok\":false,\"message\":\"invalid address: ";

static const char API_BAD_REQUEST_1[] =
". Expected four decimal octets for every field.\"}\n";

static const seg_t API_BAD_REQUEST_SEGS[] = {
	{SEG_STR, API_BAD_REQUEST},
	{SEG_SNAP, NULL},
	{SEG_STR, API_BAD_REQUEST_1},
	{SEG_END, NULL}
};

static const char API_REBOOT[] =
"{\"ok\":true,\"message\":\"the device is rebooting.\"}\n";

static const seg_t API_REBOOT_SEGS[] = {
	{SEG_STR, API_REBOOT},
	{SEG_END, NULL}
};

/* ------------------------------------------------------------------ */
/* Status collection                                                  */
/* ------------------------------------------------------------------ */

/* The stack grows down from the top of RAM, so the distance from the current
   stack pointer to _estack is the headroom a nested call chain still has. */
static uint32_t stack_headroom(void)
{
	extern uint8_t _estack;
	uint32_t sp;

	__asm__ volatile ("mov %0, sp" : "=r" (sp));

	return (uint32_t) (&_estack) - sp;
}

static void phy_text(char *out, size_t size)
{
	uint8_t		link = 0;
	wiz_PhyConf	conf;

	/* ctlwizchip reports SOCK_OK on success and -1 on a bus failure, so the
	   test has to look for the negative value only. */
	if (ctlwizchip(CW_GET_PHYLINK, (void *) &link) < 0 || link != PHY_LINK_ON) {
		snprintf(out, size, "down");
		return;
	}

	/* Speed and duplex come from the live PHY status, not from the desired
	   configuration, so a cable forced to 10 Mbps half duplex is reported
	   correctly. */
	wizphy_getphystat(&conf);

	snprintf(out, size, "%u Mbps %s",
			 conf.speed == PHY_SPEED_100 ? 100 : 10,
			 conf.duplex == PHY_DUPLEX_FULL ? "full duplex" : "half duplex");
}

static void chip_text(char *out, size_t size)
{
	uint8_t id[8];

	memset(id, 0, sizeof(id));

	if (ctlwizchip(CW_GET_ID, (void *) id) < 0) {
		snprintf(out, size, "unknown");
		return;
	}

	snprintf(out, size, "WIZnet %s", (const char *) id);
}

/* Captures everything the response needs. Address fields are copied so that a
   configuration change from another request cannot alter a response that is
   already being transmitted. */
static void snapshot_values(http_conn_t *c)
{
	wiz_NetInfo net;

	wizchip_getnetinfo(&net);

	memcpy(c->net_ip,  net.ip,  4);
	memcpy(c->net_sn,  net.sn,  4);
	memcpy(c->net_gw,  net.gw,  4);
	memcpy(c->net_dns, net.dns, 4);
	memcpy(c->net_mac, net.mac, 6);
}

static void snapshot_dashboard(http_conn_t *c)
{
	char	phy[40];
	char	chip[16];
	char	mode[16];

	snapshot_values(c);

	phy_text(phy, sizeof(phy));
	chip_text(chip, sizeof(chip));

	snprintf(mode, sizeof(mode), "%s",
			 host_configuration_mode() == NETINFO_DHCP ? "DHCP" : "static");

	/* Order must match the SEG_SNAP entries of DASH_SEGS. */
	snap_add(c, "%s", mode);
	snap_add(c, "%s", phy);
	snap_add(c, "%s", chip);
	snap_add(c, "%lu s", (unsigned long) (HAL_GetTick() / 1000));
	snap_add(c, "%lu", (unsigned long) g_requests);
	snap_add(c, "%lu B", (unsigned long) stack_headroom());
}

static void snapshot_status(http_conn_t *c)
{
	char	phy[40];
	char	chip[16];
	char	mode[16];

	snapshot_values(c);

	phy_text(phy, sizeof(phy));
	chip_text(chip, sizeof(chip));

	snprintf(mode, sizeof(mode), "%s",
			 host_configuration_mode() == NETINFO_DHCP ? "DHCP" : "static");

	/* Order must match the SEG_SNAP entries of API_SEGS. */
	snap_add(c, "%lu", (unsigned long) (HAL_GetTick() / 1000));
	snap_add(c, "%lu", (unsigned long) g_requests);
	snap_add(c, "%lu", (unsigned long) stack_headroom());
	snap_add(c, "%s", phy);
	snap_add(c, "%s", mode);
	snap_add(c, "%s", chip);
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

static void respond(http_conn_t *c, uint16_t code, const char *ctype,
					const seg_t *segs)
{
	int n;

	c->segs		= segs;
	c->body_len	= seg_measure(c);
	c->body_sent	= 0;
	c->head_sent	= 0;
	c->last_ms	= HAL_GetTick();

	if (c->body_len == HTTP_BODY_OVERFLOW || c->snap_overflow) {
		/* Either a route produced a body the length field cannot express, or a
		   status value did not fit the snapshot area. Both are internal
		   faults, so the client is told so instead of being served a body
		   whose length lies. */
		c->snap_overflow	= false;
		c->segs				= SEG_ERROR;
		c->body_len			= seg_measure(c);
		code				= 500;
	}

	n = snprintf(c->head, sizeof(c->head),
				 "HTTP/1.1 %u %s\r\n"
				 "Content-Type: %s\r\n"
				 "Content-Length: %u\r\n"
				 "Cache-Control: no-store\r\n"
				 "Connection: close\r\n"
				 "\r\n",
				 code, status_text(code), ctype, c->body_len);

	if (n < 0 || (size_t) n >= sizeof(c->head)) {
		c->head[0]	= '\0';
		c->head_len	= 0;
	} else {
		c->head_len = (uint16_t) n;
	}

	c->state = CONN_SEND;

	g_requests++;

	printf("[http] %s %s -> %u (%u bytes)\r\n",
		   c->method == METHOD_POST ? "POST" :
		   c->method == METHOD_GET ? "GET" : "?",
		   c->target, code, c->body_len);
}

/* ------------------------------------------------------------------ */
/* Routes                                                             */
/* ------------------------------------------------------------------ */

/*
 * Applies a posted address set. Every field is validated before anything is
 * written, so a malformed form cannot leave the controller with a half applied
 * configuration.
 */
static void route_post_net(http_conn_t *c, const http_request_t *req)
{
	uint8_t			ip[4], sn[4], gw[4], dns[4];
	uint8_t			mac[6];
	const char		*val;
	uint16_t		len;

	val = form_find(c, req->body_off, req->body_len, "ip", &len);
	if (val == NULL || !parse_ipv4(val, len, ip))
		goto invalid;

	val = form_find(c, req->body_off, req->body_len, "sn", &len);
	if (val == NULL || !parse_ipv4(val, len, sn))
		goto invalid;

	val = form_find(c, req->body_off, req->body_len, "gw", &len);
	if (val == NULL || !parse_ipv4(val, len, gw))
		goto invalid;

	val = form_find(c, req->body_off, req->body_len, "dns", &len);
	if (val == NULL || !parse_ipv4(val, len, dns))
		goto invalid;

	/* The MAC is optional. When the form omits it the address already in the
	   controller is kept, because the DHCP client and any server side lease
	   state are bound to it. */
	snapshot_values(c);
	memcpy(mac, c->net_mac, 6);

	val = form_find(c, req->body_off, req->body_len, "mac", &len);
	if (val != NULL) {
		if (!parse_mac(val, len, mac))
			goto invalid;
	}

	/*
	 * The write is deferred to conn_recycle(). Writing SIPR while this very
	 * connection is still open makes the controller stop matching packets for
	 * it, so the browser would see a reset instead of the confirmation that
	 * tells it where to go next.
	 */
	memcpy(c->pend_ip, ip, 4);
	memcpy(c->pend_sn, sn, 4);
	memcpy(c->pend_gw, gw, 4);
	memcpy(c->pend_dns, dns, 4);
	memcpy(c->pend_mac, mac, 6);
	c->net_pending = true;

	/* Report the address the device is about to take. */
	memcpy(c->net_ip, ip, 4);
	memcpy(c->net_sn, sn, 4);
	memcpy(c->net_gw, gw, 4);
	memcpy(c->net_dns, dns, 4);
	memcpy(c->net_mac, mac, 6);

	respond(c, 200, "application/json", API_SAVED_SEGS);
	return;

invalid:
	snap_add(c, "one of ip, sn, gw, dns, mac");
	respond(c, 400, "application/json", API_BAD_REQUEST_SEGS);
}

static void route_reboot(http_conn_t *c)
{
	c->reset_pending = true;
	respond(c, 200, "application/json", API_REBOOT_SEGS);
}

static void route_dispatch(http_conn_t *c, const http_request_t *req)
{
	if (c->method == METHOD_UNKNOWN) {
		respond(c, 405, "text/html; charset=utf-8", SEG_ERROR);
		return;
	}

	/* A known path reached with the wrong method is 405, not 404: the client
	   can fix the request instead of looking for another resource. */
	if (c->method == METHOD_GET) {
		if (str_eq(c->target, "/")) {
			snapshot_dashboard(c);
			respond(c, 200, "text/html; charset=utf-8", DASH_SEGS);
		} else if (str_eq(c->target, "/api/status")) {
			snapshot_status(c);
			respond(c, 200, "application/json", API_SEGS);
		} else if (str_eq(c->target, "/api/net")) {
			snapshot_values(c);
			respond(c, 200, "application/json", API_NET_SEGS);
		} else if (str_eq(c->target, "/api/reboot")) {
			respond(c, 405, "text/html; charset=utf-8", SEG_ERROR);
		} else {
			respond(c, 404, "text/html; charset=utf-8", SEG_NOT_FOUND);
		}
		return;
	}

	/* POST */
	if (str_eq(c->target, "/api/net")) {
		route_post_net(c, req);
	} else if (str_eq(c->target, "/api/reboot")) {
		route_reboot(c);
	} else if (str_eq(c->target, "/") || str_eq(c->target, "/api/status")) {
		respond(c, 405, "text/html; charset=utf-8", SEG_ERROR);
	} else {
		respond(c, 404, "text/html; charset=utf-8", SEG_NOT_FOUND);
	}
}

static void conn_handle(http_conn_t *c)
{
	http_request_t	req;

	request_parse(c, &req);

	if (req.oversized) {
		respond(c, 413, "text/html; charset=utf-8", SEG_TOO_LARGE);
		return;
	}

	if (!req.head_ready) {
		/* The blank line is there but the request line is malformed, so waiting
		   for more bytes cannot help. */
		if (req.head_found) {
			respond(c, 400, "text/html; charset=utf-8", SEG_ERROR);
			return;
		}

		/* Not enough bytes for a request line yet. Once the buffer is full the
		   request can never become valid, so it is dropped. */
		if (c->rx_len >= HTTP_SERVER_RX_SIZE)
			respond(c, 400, "text/html; charset=utf-8", SEG_ERROR);

		return;
	}

	if (!req.complete)
		return;			/* body still arriving */

	route_dispatch(c, &req);
}

/* ------------------------------------------------------------------ */
/* Connection state machine                                           */
/* ------------------------------------------------------------------ */

static bool conn_arm(http_conn_t *c)
{
	/* socket() binds the local port and issues OPEN; listen() then puts the
	   socket into the listening state. The ioLibrary returns the socket number
	   on success, which is not the same as SOCK_OK. */
	if (socket(c->sock, Sn_MR_TCP, HTTP_SERVER_PORT, SF_IO_NONBLOCK) != (int8_t) c->sock)
		return false;

	if (listen(c->sock) != SOCK_OK)
		return false;

	return true;
}

/* Closes the socket and immediately re-arms it, because the listening socket is
   consumed by the client it accepted. A posted address is written here, with the
   socket already closed, so the controller never has to change its own address
   underneath a live connection. */
static void conn_recycle(http_conn_t *c)
{
	bool	apply = c->net_pending;
	uint8_t	ip[4], sn[4], gw[4], dns[4], mac[6];

	memcpy(ip,  c->pend_ip,  4);
	memcpy(sn,  c->pend_sn,  4);
	memcpy(gw,  c->pend_gw,  4);
	memcpy(dns, c->pend_dns, 4);
	memcpy(mac, c->pend_mac, 6);

	close(c->sock);

	conn_clear(c);

	if (apply) {
		static_host_configuration(mac, ip, sn, gw, dns);
		printf("[http] address set to %u.%u.%u.%u, the previous URL is gone\r\n",
			   ip[0], ip[1], ip[2], ip[3]);
	}

	if (conn_arm(c)) {
		c->state = CONN_LISTEN;
	} else {
		/* No address, or the chip is not answering. Stay idle and retry. */
		c->state = CONN_FREE;
	}

	c->last_ms = HAL_GetTick();
}

static void conn_step_listen(http_conn_t *c, uint32_t now)
{
	(void) now;

	/* The chip keeps a listening socket in SOCK_LISTEN indefinitely, so there
	   is nothing to time out here. Recycling it periodically would only drop
	   connection attempts that are still in flight. */
	if (getSn_SR(c->sock) == SOCK_ESTABLISHED) {
		getSn_DIPR(c->sock, c->peer_ip);

		conn_clear(c);
		c->state = CONN_REQUEST;
		c->last_ms = now;

		printf("[http] open %u.%u.%u.%u\r\n",
			   c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3]);
		return;
	}

	/* Only a socket the chip gave up on has to be re-armed. */
	if (getSn_SR(c->sock) == SOCK_CLOSED)
		conn_recycle(c);
}

static void conn_step_request(http_conn_t *c, uint32_t now)
{
	uint8_t sr = getSn_SR(c->sock);

	/* SOCK_CLOSE_WAIT means the peer sent a FIN but there may still be a
	   request in flight, so it is handled like an established connection. */
	if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) {
		conn_recycle(c);
		return;
	}

	if (rx_fill(c))
		c->last_ms = now;

	conn_handle(c);

	if (c->state == CONN_REQUEST && (now - c->last_ms) > HTTP_SERVER_IDLE_MS)
		conn_recycle(c);		/* client opened a connection and went silent */
}

static void conn_step_send(http_conn_t *c, uint32_t now)
{
	uint8_t sr = getSn_SR(c->sock);

	if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) {
		conn_recycle(c);
		return;
	}

	if (seg_flush(c, HTTP_TX_BUDGET) > 0) {
		c->last_ms = now;
		return;
	}

	if (c->head_sent >= c->head_len && c->body_sent >= c->body_len) {
		c->state = CONN_CLOSE;
		c->last_ms = now;
		return;
	}

	/* No progress and not finished: the peer stopped reading. */
	if (now - c->last_ms > HTTP_SERVER_IDLE_MS)
		conn_recycle(c);
}

static void conn_step_close(http_conn_t *c, uint32_t now)
{
	/* Sn_CR_CLOSE may discard whatever is still queued, so the last segment has
	   to reach the wire first. */
	if (getSn_TX_FSR(c->sock) != getSn_TxMAX(c->sock)) {
		if (now - c->last_ms <= HTTP_DRAIN_TIMEOUT_MS)
			return;
	}

	if (c->reset_pending) {
		printf("[http] rebooting on request\r\n");
		NVIC_SystemReset();
	}

	conn_recycle(c);
}

/* ------------------------------------------------------------------ */
/* Public interface                                                   */
/* ------------------------------------------------------------------ */

void http_server_init(void)
{
	uint8_t i;

	g_requests		= 0;
	g_arm_cursor	= 0;

	for (i = 0; i < HTTP_SERVER_MAX_CONN; i++) {
		http_conn_t *c = &g_conn[i];

		c->sock		= (uint8_t) (HTTP_SERVER_BASE_SOCKET + i);
		c->state	= CONN_FREE;
		conn_clear(c);
	}
}

void http_server_poll(void)
{
	uint32_t	now = HAL_GetTick();
	uint8_t		i;

	/*
	 * Arm the idle slots lazily: opening a socket needs a valid SIPR, so doing
	 * it lazily also keeps the server harmless if init was called too early.
	 *
	 * The scan starts at the rotating cursor and arms at most one slot per poll.
	 * Advancing the cursor unconditionally matters, otherwise a slot that fails
	 * to arm would be skipped forever once another slot is busy.
	 */
	for (i = 0; i < HTTP_SERVER_MAX_CONN; i++) {
		uint8_t		idx = (uint8_t) ((g_arm_cursor + i) % HTTP_SERVER_MAX_CONN);
		http_conn_t	*c	 = &g_conn[idx];

		if (c->state != CONN_FREE)
			continue;

		g_arm_cursor = (uint8_t) ((idx + 1) % HTTP_SERVER_MAX_CONN);

		if (conn_arm(c)) {
			c->state = CONN_LISTEN;
			c->last_ms = now;

			printf("[http] socket %u listening on port %u\r\n",
				   c->sock, HTTP_SERVER_PORT);
		}

		break;
	}

	for (i = 0; i < HTTP_SERVER_MAX_CONN; i++) {
		http_conn_t	*c = &g_conn[i];

		switch (c->state) {
		case CONN_LISTEN:
			conn_step_listen(c, now);
			break;

		case CONN_REQUEST:
			conn_step_request(c, now);
			break;

		case CONN_SEND:
			conn_step_send(c, now);
			break;

		case CONN_CLOSE:
			conn_step_close(c, now);
			break;

		case CONN_FREE:
		default:
			break;
		}
	}
}

uint32_t http_server_requests(void)
{
	return g_requests;
}
