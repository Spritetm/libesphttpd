#include <esp8266.h>
#include "httpd.h"
#include "sha1.h"
#include "base64.h"

#define WS_KEY_IDENTIFIER "Sec-WebSocket-Key: "
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_RESPONSE "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
#define HTML_HEADER_LINEEND "\r\n"

#define CONN_TIMEOUT 60*60*12

/* from IEEE RFC6455 sec 5.2
      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-------+-+-------------+-------------------------------+
     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
     | |1|2|3|       |K|             |                               |
     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
     |     Extended payload length continued, if payload len == 127  |
     + - - - - - - - - - - - - - - - +-------------------------------+
     |                               |Masking-key, if MASK set to 1  |
     +-------------------------------+-------------------------------+
     | Masking-key (continued)       |          Payload Data         |
     +-------------------------------- - - - - - - - - - - - - - - - +
     :                     Payload Data continued ...                :
     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
     |                     Payload Data continued ...                |
     +---------------------------------------------------------------+
*/

#define FLAG_FIN (1 << 7)
#define FLAG_RSV1 (1 << 6)
#define FLAG_RSV2 (1 << 5)
#define FLAG_RSV3 (1 << 4)

#define OPCODE_CONTINUE 0x0
#define OPCODE_TEXT 0x1
#define OPCODE_BINARY 0x2
#define OPCODE_CLOSE 0x8
#define OPCODE_PING 0x9
#define OPCODE_PONG 0xA

#define FLAGS_MASK ((uint8_t)0xF0)
#define OPCODE_MASK ((uint8_t)0x0F)
#define IS_MASKED ((uint8_t)(1<<7))
#define PAYLOAD_MASK ((uint8_t)0x7F)

#define STATUS_OPEN 0
#define STATUS_CLOSED 1
#define STATUS_UNINITIALISED 2

#define CLOSE_MESSAGE {FLAG_FIN | OPCODE_CLOSE, IS_MASKED /* + payload = 0*/, 0 /* + masking key*/}
#define CLOSE_MESSAGE_LENGTH 3

typedef struct WSFrame WSFrame;
typedef struct Websock Websock;


struct WSFrame {
	uint8_t flags;
	uint8_t opcode;
	uint8_t isMasked;
	uint64_t payloadLength;
	uint32_t maskingKey;
	char* payloadData;
};

struct Websock {
	uint8_t status;
};

int ICACHE_FLASH_ATTR cgiWebSocketRecv(HttpdConnData *connData, char *data, int len) {
	os_printf("ToDo: cgiWebSocketRecv\n");

	return 0;
}

//Websocket 'cgi' implementation
int ICACHE_FLASH_ATTR cgiWebsocket(HttpdConnData *connData) {
	char buff[256];
	int i;
	sha1nfo s;
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		if (connData->cgiPrivData) os_free(connData->cgiPrivData);
		return HTTPD_CGI_DONE;
	}
	
	if (connData->cgiPrivData==NULL) {
		//First call here. Check if client headers are OK, send server header.
		i=httpdGetHeader(connData, "Upgrade", buff, sizeof(buff)-1);
		if (i && os_strcmp(buff, "websocket")==0) {
			i=httpdGetHeader(connData, "Sec-WebSocket-Key", buff, sizeof(buff)-1);
			if (i) {
				//Seems like a WebSocket connection. Alloc struct and reply.
				connData->cgiPrivData=os_malloc(sizeof(Websock));
				os_strcat(buff, WS_GUID);
				sha1_init(&s);
				sha1_write(&s, buff, os_strlen(buff));
				httpdStartResponse(connData, 101);
				httpdHeader(connData, "Upgrade", "websocket");
				httpdHeader(connData, "Connection", "upgrade");
				base64_encode(20, sha1_result(&s), sizeof(buff), buff);
				httpdHeader(connData, "Sec-WebSocket-Accept", buff);
				connData->recvHdl=cgiWebSocketRecv;
				return HTTPD_CGI_MORE;
			}
		}
		//No valid websocket connection
		httpdStartResponse(connData, 500);
		httpdEndHeaders(connData);
		return HTTPD_CGI_DONE;
	}
	return HTTPD_CGI_DONE;
}




