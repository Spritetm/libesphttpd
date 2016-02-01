/*
Esp8266 http server - core routines
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */


#include <esp8266.h>
#include "httpd.h"
#include "httpd-platform.h"


//Max amount of simultaneous connections
#define MAX_CONN 8
//Max length of request head. This is statically allocated for each connection.
#define MAX_HEAD_LEN 1024
//Max post buffer len. This is dynamically malloc'ed if needed.
#define MAX_POST 1024
//Max send buffer len. This is allocated on the stack.
#define MAX_SENDBUFF_LEN 2048
//If some data can't be sent because the underlaying socket doesn't accept the data (like the nonos
//layer is prone to do), we put it in a backlog that is dynamically malloc'ed. This defines the max
//size of the backlog.
#define MAX_BACKLOG_SIZE (4*1024)

//This gets set at init time.
static HttpdBuiltInUrl *builtInUrls;

typedef struct HttpSendBacklogItem HttpSendBacklogItem;

struct HttpSendBacklogItem {
	int len;
	HttpSendBacklogItem *next;
	char data[];
};

//Private data for http connection
struct HttpdPriv {
	char head[MAX_HEAD_LEN];
	int headPos;
	char *sendBuff;
	int sendBuffLen;
	HttpSendBacklogItem *sendBacklog;
	int sendBacklogSize;
};


//Connection pool
static HttpdConnData *connData[MAX_CONN];

//Struct to keep extension->mime data in
typedef struct {
	const char *ext;
	const char *mimetype;
} MimeMap;


//#define RSTR(a) ((const char)(a))

//The mappings from file extensions to mime types. If you need an extra mime type,
//add it here.
static const ICACHE_RODATA_ATTR MimeMap mimeTypes[]={
	{"htm", "text/htm"},
	{"html", "text/html"},
	{"css", "text/css"},
	{"js", "text/javascript"},
	{"txt", "text/plain"},
	{"jpg", "image/jpeg"},
	{"jpeg", "image/jpeg"},
	{"png", "image/png"},
	{"svg", "image/svg+xml"},
	{NULL, "text/html"}, //default value
};

//Returns a static char* to a mime type for a given url to a file.
const char ICACHE_FLASH_ATTR *httpdGetMimetype(char *url) {
	int i=0;
	//Go find the extension
	char *ext=url+(strlen(url)-1);
	while (ext!=url && *ext!='.') ext--;
	if (*ext=='.') ext++;
	
	//ToDo: strcmp is case sensitive; we may want to do case-intensive matching here...
	while (mimeTypes[i].ext!=NULL && strcmp(ext, mimeTypes[i].ext)!=0) i++;
	return mimeTypes[i].mimetype;
}

//Looks up the connData info for a specific connection
static HttpdConnData ICACHE_FLASH_ATTR *httpdFindConnData(ConnTypePtr conn, char *remIp, int remPort) {
	for (int i=0; i<MAX_CONN; i++) {
		if (connData[i] && connData[i]->remote_port == remPort &&
						memcmp(connData[i]->remote_ip, remIp, 4) == 0) {
			connData[i]->conn=conn;
			return connData[i];
		}
	}
	//Shouldn't happen.
	printf("*** Unknown connection %d.%d.%d.%d:%d\n", remIp[0]&0xff, remIp[1]&0xff, remIp[2]&0xff, remIp[3]&0xff, remPort);
	httpdPlatDisconnect(conn);
	return NULL;
}

//Retires a connection for re-use
static void ICACHE_FLASH_ATTR httpdRetireConn(HttpdConnData *conn) {
	if (conn->priv->sendBacklog!=NULL) {
		HttpSendBacklogItem *i, *j;
		i=conn->priv->sendBacklog;
		do {
			j=i;
			i=i->next;
			free(j);
		} while (i!=NULL);
	}
	if (conn->post->buff!=NULL) free(conn->post->buff);
	if (conn->post!=NULL) free(conn->post);
	if (conn->priv!=NULL) free(conn->priv);
	if (conn) free(conn);
	for (int i=0; i<MAX_CONN; i++) {
		if (connData[i]==conn) connData[i]=NULL;
	}
}

//Stupid li'l helper function that returns the value of a hex char.
static int ICACHE_FLASH_ATTR  httpdHexVal(char c) {
	if (c>='0' && c<='9') return c-'0';
	if (c>='A' && c<='F') return c-'A'+10;
	if (c>='a' && c<='f') return c-'a'+10;
	return 0;
}

//Decode a percent-encoded value.
//Takes the valLen bytes stored in val, and converts it into at most retLen bytes that
//are stored in the ret buffer. Returns the actual amount of bytes used in ret. Also
//zero-terminates the ret buffer.
int ICACHE_FLASH_ATTR  httpdUrlDecode(char *val, int valLen, char *ret, int retLen) {
	int s=0, d=0;
	int esced=0, escVal=0;
	while (s<valLen && d<retLen) {
		if (esced==1)  {
			escVal=httpdHexVal(val[s])<<4;
			esced=2;
		} else if (esced==2) {
			escVal+=httpdHexVal(val[s]);
			ret[d++]=escVal;
			esced=0;
		} else if (val[s]=='%') {
			esced=1;
		} else if (val[s]=='+') {
			ret[d++]=' ';
		} else {
			ret[d++]=val[s];
		}
		s++;
	}
	if (d<retLen) ret[d]=0;
	return d;
}

//Find a specific arg in a string of get- or post-data.
//Line is the string of post/get-data, arg is the name of the value to find. The
//zero-terminated result is written in buff, with at most buffLen bytes used. The
//function returns the length of the result, or -1 if the value wasn't found. The 
//returned string will be urldecoded already.
int ICACHE_FLASH_ATTR httpdFindArg(char *line, char *arg, char *buff, int buffLen) {
	char *p, *e;
	if (line==NULL) return -1;
	p=line;
	while(p!=NULL && *p!='\n' && *p!='\r' && *p!=0) {
//		printf("findArg: %s\n", p);
		if (strncmp(p, arg, strlen(arg))==0 && p[strlen(arg)]=='=') {
			p+=strlen(arg)+1; //move p to start of value
			e=(char*)strstr(p, "&");
			if (e==NULL) e=p+strlen(p);
//			printf("findArg: val %s len %d\n", p, (e-p));
			return httpdUrlDecode(p, (e-p), buff, buffLen);
		}
		p=(char*)strstr(p, "&");
		if (p!=NULL) p+=1;
	}
	printf("Finding %s in %s: Not found :/\n", arg, line);
	return -1; //not found
}

//Get the value of a certain header in the HTTP client head
//Returns true when found, false when not found.
int ICACHE_FLASH_ATTR httpdGetHeader(HttpdConnData *conn, char *header, char *ret, int retLen) {
	char *p=conn->priv->head;
	p=p+strlen(p)+1; //skip GET/POST part
	p=p+strlen(p)+1; //skip HTTP part
	while (p<(conn->priv->head+conn->priv->headPos)) {
		while(*p<=32 && *p!=0) p++; //skip crap at start
		//See if this is the header
		if (strncmp(p, header, strlen(header))==0 && p[strlen(header)]==':') {
			//Skip 'key:' bit of header line
			p=p+strlen(header)+1;
			//Skip past spaces after the colon
			while(*p==' ') p++;
			//Copy from p to end
			while (*p!=0 && *p!='\r' && *p!='\n' && retLen>1) {
				*ret++=*p++;
				retLen--;
			}
			//Zero-terminate string
			*ret=0;
			//All done :)
			return 1;
		}
		p+=strlen(p)+1; //Skip past end of string and \0 terminator
	}
	return 0;
}

//Start the response headers.
void ICACHE_FLASH_ATTR httpdStartResponse(HttpdConnData *conn, int code) {
	static const char ICACHE_RODATA_ATTR respStart[]="HTTP/1.%d %d OK\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\n";
	char buff[sizeof(respStart)+10];
	int l;
	int minor=0;
	if (code==101) minor=1; //Websockets officially needs 1.1. Advertise 1.0 in all other cases.
	l=sprintf(buff, respStart, minor, code);
	httpdSend(conn, buff, l);
}

//Send a http header.
void ICACHE_FLASH_ATTR httpdHeader(HttpdConnData *conn, const char *field, const char *val) {
	char buff[128];
	int l;

	l=sprintf(buff, "%s: %s\r\n", field, val);
	httpdSend(conn, buff, l);
}

//Finish the headers.
void ICACHE_FLASH_ATTR httpdEndHeaders(HttpdConnData *conn) {
	httpdSend(conn, "\r\n", -1);
}

//ToDo: sprintf->snprintf everywhere... esp doesn't have snprintf tho' :/
//Redirect to the given URL.
void ICACHE_FLASH_ATTR httpdRedirect(HttpdConnData *conn, char *newUrl) {
	static const char ICACHE_RODATA_ATTR redirectHeader[]="HTTP/1.1 302 Found\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nLocation: %s\r\n\r\nMoved to %s\r\n";
	char buff[1024];
	int l;
	l=sprintf(buff, redirectHeader, newUrl, newUrl);
	httpdSend(conn, buff, l);
}

//Use this as a cgi function to redirect one url to another.
int ICACHE_FLASH_ATTR cgiRedirect(HttpdConnData *connData) {
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	httpdRedirect(connData, (char*)connData->cgiArg);
	return HTTPD_CGI_DONE;
}

//This CGI function redirects to a fixed url of http://[hostname]/ if hostname field of request isn't
//already that hostname. Use this in combination with a DNS server that redirects everything to the
//ESP in order to load a HTML page as soon as a phone, tablet etc connects to the ESP. Watch out:
//this will also redirect connections when the ESP is in STA mode, potentially to a hostname that is not
//in the 'official' DNS and so will fail.
int ICACHE_FLASH_ATTR cgiRedirectToHostname(HttpdConnData *connData) {
	char buff[1024];
	int isIP=0;
	int x;
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	if (connData->hostName==NULL) {
		printf("Huh? No hostname.\n");
		return HTTPD_CGI_NOTFOUND;
	}

	//Quick and dirty code to see if host is an IP
	if (strlen(connData->hostName)>8) {
		isIP=1;
		for (x=0; x<strlen(connData->hostName); x++) {
			if (connData->hostName[x]!='.' && (connData->hostName[x]<'0' || connData->hostName[x]>'9')) isIP=0;
		}
	}
	if (isIP) return HTTPD_CGI_NOTFOUND;
	//Check hostname; pass on if the same
	if (strcmp(connData->hostName, (char*)connData->cgiArg)==0) return HTTPD_CGI_NOTFOUND;
	//Not the same. Redirect to real hostname.
	sprintf(buff, "http://%s/", (char*)connData->cgiArg);
	printf("Redirecting to hostname url %s\n", buff);
	httpdRedirect(connData, buff);
	return HTTPD_CGI_DONE;
}


//Same as above, but will only redirect clients with an IP that is in the range of
//the SoftAP interface. This should preclude clients connected to the STA interface
//to be redirected to nowhere.
int ICACHE_FLASH_ATTR cgiRedirectApClientToHostname(HttpdConnData *connData) {
#ifndef FREERTOS
	uint32 *remadr;
	struct ip_info apip;
	int x=wifi_get_opmode();
	//Check if we have an softap interface; bail out if not
	if (x!=2 && x!=3) return HTTPD_CGI_NOTFOUND;
	remadr=(uint32 *)connData->remote_ip;
	wifi_get_ip_info(SOFTAP_IF, &apip);
	if ((*remadr & apip.netmask.addr) == (apip.ip.addr & apip.netmask.addr)) {
		return cgiRedirectToHostname(connData);
	} else {
		return HTTPD_CGI_NOTFOUND;
	}
#else
	return HTTPD_CGI_NOTFOUND;
#endif
}


//Add data to the send buffer. len is the length of the data. If len is -1
//the data is seen as a C-string.
//Returns 1 for success, 0 for out-of-memory.
int ICACHE_FLASH_ATTR httpdSend(HttpdConnData *conn, const char *data, int len) {
	if (conn->conn==NULL) return 0;
	if (len<0) len=strlen(data);
	if (conn->priv->sendBuffLen+len>MAX_SENDBUFF_LEN) return 0;
	memcpy(conn->priv->sendBuff+conn->priv->sendBuffLen, data, len);
	conn->priv->sendBuffLen+=len;
	return 1;
}

//Function to send any data in conn->priv->sendBuff. Do not use in CGIs unless you know what you
//are doing!
void ICACHE_FLASH_ATTR httpdFlushSendBuffer(HttpdConnData *conn) {
	int r;
	if (conn->conn==NULL) return;
	if (conn->priv->sendBuffLen!=0) {
		r=httpdPlatSendData(conn->conn, conn->priv->sendBuff, conn->priv->sendBuffLen);
		if (!r) {
			//Can't send this for some reason. Dump packet in backlog, we can send it later.
			if (conn->priv->sendBacklogSize+conn->priv->sendBuffLen>MAX_BACKLOG_SIZE) {
				printf("Httpd: Backlog: Exceeded max backlog size, dropped %d bytes instead of sending them.\n", conn->priv->sendBuffLen);
				conn->priv->sendBuffLen=0;
				return;
			}
			HttpSendBacklogItem *i=malloc(sizeof(HttpSendBacklogItem)+conn->priv->sendBuffLen);
			if (i==NULL) {
				printf("Httpd: Backlog: malloc failed, out of memory!\n");
				return;
			}
			memcpy(i->data, conn->priv->sendBuff, conn->priv->sendBuffLen);
			i->len=conn->priv->sendBuffLen;
			i->next=NULL;
			if (conn->priv->sendBacklog==NULL) {
				conn->priv->sendBacklog=i;
			} else {
				HttpSendBacklogItem *e=conn->priv->sendBacklog;
				while (e->next!=NULL) e=e->next;
				e->next=i;
			}
			conn->priv->sendBacklogSize+=conn->priv->sendBuffLen;
		}
		conn->priv->sendBuffLen=0;
	}
}

//Callback called when the data on a socket has been successfully
//sent.
void ICACHE_FLASH_ATTR httpdSentCb(ConnTypePtr rconn, char *remIp, int remPort) {
	int r;
	HttpdConnData *conn=httpdFindConnData(rconn, remIp, remPort);
	char *sendBuff=malloc(MAX_SENDBUFF_LEN);

	if (conn==NULL) return;

	if (conn->priv->sendBacklog!=NULL) {
		//We have some backlog to send first.
		HttpSendBacklogItem *next=conn->priv->sendBacklog->next;
		httpdPlatSendData(conn->conn, conn->priv->sendBacklog->data, conn->priv->sendBacklog->len);
		conn->priv->sendBacklogSize-=conn->priv->sendBacklog->len;
		free(conn->priv->sendBacklog);
		conn->priv->sendBacklog=next;
		return;
	}

	conn->priv->sendBuff=sendBuff;
	conn->priv->sendBuffLen=0;

	if (conn->cgi==NULL) { //Marked for destruction?
		printf("Pool slot %d is done. Closing.\n", conn->slot);
		httpdPlatDisconnect(conn->conn);
		return; //No need to call httpdFlushSendBuffer.
	}

	r=conn->cgi(conn); //Execute cgi fn.
	if (r==HTTPD_CGI_DONE) {
		conn->cgi=NULL; //mark for destruction.
	}
	if (r==HTTPD_CGI_NOTFOUND || r==HTTPD_CGI_AUTHENTICATED) {
		printf("ERROR! CGI fn returns code %d after sending data! Bad CGI!\n", r);
		conn->cgi=NULL; //mark for destruction.
	}
	httpdFlushSendBuffer(conn);
	free(sendBuff);
}

static const ICACHE_RODATA_ATTR char httpNotFoundHeader[]="HTTP/1.0 404 Not Found\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nNot Found.\r\n";

//This is called when the headers have been received and the connection is ready to send
//the result headers and data.
//We need to find the CGI function to call, call it, and dependent on what it returns either
//find the next cgi function, wait till the cgi data is sent or close up the connection.
static void ICACHE_FLASH_ATTR httpdProcessRequest(HttpdConnData *conn) {
	int r;
	int i=0;
	if (conn->url==NULL) {
		printf("WtF? url = NULL\n");
		return; //Shouldn't happen
	}
	//See if we can find a CGI that's happy to handle the request.
	while (1) {
		//Look up URL in the built-in URL table.
		while (builtInUrls[i].url!=NULL) {
			int match=0;
			//See if there's a literal match
			if (strcmp(builtInUrls[i].url, conn->url)==0) match=1;
			//See if there's a wildcard match
			if (builtInUrls[i].url[strlen(builtInUrls[i].url)-1]=='*' &&
					strncmp(builtInUrls[i].url, conn->url, strlen(builtInUrls[i].url)-1)==0) match=1;
			if (match) {
				printf("Is url index %d\n", i);
				conn->cgiData=NULL;
				conn->cgi=builtInUrls[i].cgiCb;
				conn->cgiArg=builtInUrls[i].cgiArg;
				break;
			}
			i++;
		}
		if (builtInUrls[i].url==NULL) {
			//Drat, we're at the end of the URL table. This usually shouldn't happen. Well, just
			//generate a built-in 404 to handle this.
			printf("%s not found. 404!\n", conn->url);
			httpdSend(conn, httpNotFoundHeader, -1);
			httpdFlushSendBuffer(conn);
			conn->cgi=NULL; //mark for destruction
			return;
		}
		
		//Okay, we have a CGI function that matches the URL. See if it wants to handle the
		//particular URL we're supposed to handle.
		r=conn->cgi(conn);
		if (r==HTTPD_CGI_MORE) {
			//Yep, it's happy to do so and has more data to send.
			httpdFlushSendBuffer(conn);
			return;
		} else if (r==HTTPD_CGI_DONE) {
			//Yep, it's happy to do so and already is done sending data.
			httpdFlushSendBuffer(conn);
			conn->cgi=NULL; //mark conn for destruction
			return;
		} else if (r==HTTPD_CGI_NOTFOUND || r==HTTPD_CGI_AUTHENTICATED) {
			//URL doesn't want to handle the request: either the data isn't found or there's no
			//need to generate a login screen.
			i++; //look at next url the next iteration of the loop.
		}
	}
}

//Parse a line of header data and modify the connection data accordingly.
static void ICACHE_FLASH_ATTR httpdParseHeader(char *h, HttpdConnData *conn) {
	int i;
	char firstLine=0;
	
	if (strncmp(h, "GET ", 4)==0) {
		conn->requestType = HTTPD_METHOD_GET;
		firstLine=1;
	} else if (strncmp(h, "Host:", 5)==0) {
		i=5;
		while (h[i]==' ') i++;
		conn->hostName=&h[i];
	} else if (strncmp(h, "POST ", 5)==0) {
		conn->requestType = HTTPD_METHOD_POST;
		firstLine=1;
	}

	if (firstLine) {
		char *e;
		
		//Skip past the space after POST/GET
		i=0;
		while (h[i]!=' ') i++;
		conn->url=h+i+1;

		//Figure out end of url.
		e=(char*)strstr(conn->url, " ");
		if (e==NULL) return; //wtf?
		*e=0; //terminate url part

		printf("URL = %s\n", conn->url);
		//Parse out the URL part before the GET parameters.
		conn->getArgs=(char*)strstr(conn->url, "?");
		if (conn->getArgs!=0) {
			*conn->getArgs=0;
			conn->getArgs++;
			printf("GET args = %s\n", conn->getArgs);
		} else {
			conn->getArgs=NULL;
		}

	} else if (strncmp(h, "Content-Length:", 15)==0) {
		i=15;
		//Skip trailing spaces
		while (h[i]==' ') i++;
		//Get POST data length
		conn->post->len=atoi(h+i);

		// Allocate the buffer
		if (conn->post->len > MAX_POST) {
			// we'll stream this in in chunks
			conn->post->buffSize = MAX_POST;
		} else {
			conn->post->buffSize = conn->post->len;
		}
		printf("Mallocced buffer for %d + 1 bytes of post data.\n", conn->post->buffSize);
		conn->post->buff=(char*)malloc(conn->post->buffSize + 1);
		conn->post->buffLen=0;
	} else if (strncmp(h, "Content-Type: ", 14)==0) {
		if (strstr(h, "multipart/form-data")) {
			// It's multipart form data so let's pull out the boundary for future use
			char *b;
			if ((b = strstr(h, "boundary=")) != NULL) {
				conn->post->multipartBoundary = b + 7; // move the pointer 2 chars before boundary then fill them with dashes
				conn->post->multipartBoundary[0] = '-';
				conn->post->multipartBoundary[1] = '-';
				printf("boundary = %s\n", conn->post->multipartBoundary);
			}
		}
	}
}


//Callback called when there's data available on a socket.
void httpdRecvCb(ConnTypePtr rconn, char *remIp, int remPort, char *data, unsigned short len) {
	int x, r;
	char *p, *e;
	char sendBuff[MAX_SENDBUFF_LEN];
	HttpdConnData *conn=httpdFindConnData(rconn, remIp, remPort);
	if (conn==NULL) return;
	conn->priv->sendBuff=sendBuff;
	conn->priv->sendBuffLen=0;

	//This is slightly evil/dirty: we abuse conn->post->len as a state variable for where in the http communications we are:
	//<0 (-1): Post len unknown because we're still receiving headers
	//==0: No post data
	//>0: Need to receive post data
	//ToDo: See if we can use something more elegant for this.

	for (x=0; x<len; x++) {
		if (conn->post->len<0) {
			//This byte is a header byte.
			if (data[x]=='\n') {
				//Compatibility with clients that send \n only: fake a \r in front of this.
				if (conn->priv->headPos!=0 && conn->priv->head[conn->priv->headPos-1]!='\r') {
					conn->priv->head[conn->priv->headPos++]='\r';
				}
			}
			if (conn->priv->headPos!=MAX_HEAD_LEN) conn->priv->head[conn->priv->headPos++]=data[x];
			conn->priv->head[conn->priv->headPos]=0;
			//Scan for /r/n/r/n. Receiving this indicate the headers end.
			if (data[x]=='\n' && (char *)strstr(conn->priv->head, "\r\n\r\n")!=NULL) {
				//Indicate we're done with the headers.
				conn->post->len=0;
				//Reset url data
				conn->url=NULL;
				//Iterate over all received headers and parse them.
				p=conn->priv->head;
				while(p<(&conn->priv->head[conn->priv->headPos-4])) {
					e=(char *)strstr(p, "\r\n"); //Find end of header line
					if (e==NULL) break;			//Shouldn't happen.
					e[0]=0;						//Zero-terminate header
					httpdParseHeader(p, conn);	//and parse it.
					p=e+2;						//Skip /r/n (now /0/n)
				}
				//If we don't need to receive post data, we can send the response now.
				if (conn->post->len==0) {
					httpdProcessRequest(conn);
				}
			}
		} else if (conn->post->len!=0) {
			//This byte is a POST byte.
			conn->post->buff[conn->post->buffLen++]=data[x];
			conn->post->received++;
			conn->hostName=NULL;
			if (conn->post->buffLen >= conn->post->buffSize || conn->post->received == conn->post->len) {
				//Received a chunk of post data
				conn->post->buff[conn->post->buffLen]=0; //zero-terminate, in case the cgi handler knows it can use strings
				//Send the response.
				httpdProcessRequest(conn);
				conn->post->buffLen = 0;
			}
		} else {
			//Let cgi handle data if it registered a recvHdl callback. If not, ignore.
			if (conn->recvHdl) {
				r=conn->recvHdl(conn, data+x, len-x);
				if (r==HTTPD_CGI_DONE) {
					printf("Recvhdl returned DONE\n");
					httpdFlushSendBuffer(conn);
					conn->cgi=NULL; //mark conn for destruction
					//We assume the recvhdlr has sent something; we'll kill the sock in the sent callback.
				}
				break; //ignore rest of data, recvhdl has parsed it.
			}
		}
	}
	if (conn->conn) httpdFlushSendBuffer(conn);
}

//The platform layer should ALWAYS call this function, regardless if the connection is closed by the server
//or by the client.
void ICACHE_FLASH_ATTR httpdDisconCb(ConnTypePtr rconn, char *remIp, int remPort) {
	HttpdConnData *hconn=httpdFindConnData(rconn, remIp, remPort);
	if (hconn==NULL) return;
	hconn->conn=NULL; //indicate cgi the connection is gone
	if (hconn->cgi) hconn->cgi(hconn); //Execute cgi fn if needed
	httpdRetireConn(hconn);
}


int ICACHE_FLASH_ATTR httpdConnectCb(ConnTypePtr conn, char *remIp, int remPort) {
	int i;
	//Find empty conndata in pool
	for (i=0; i<MAX_CONN; i++) if (connData[i]==NULL) break;
	printf("Conn req from  %d.%d.%d.%d:%d, using pool slot %d\n", remIp[0]&0xff, remIp[1]&0xff, remIp[2]&0xff, remIp[3]&0xff, remPort, i);
	if (i==MAX_CONN) {
		printf("Aiee, conn pool overflow!\n");
		return 0;
	}
	connData[i]=malloc(sizeof(HttpdConnData));
	memset(connData[i], 0, sizeof(HttpdConnData));
	connData[i]->priv=malloc(sizeof(HttpdPriv));
	memset(connData[i]->priv, 0, sizeof(HttpdPriv));
	connData[i]->conn=conn;
	connData[i]->slot=i;
	connData[i]->priv->headPos=0;
	connData[i]->post=malloc(sizeof(HttpdPostData));
	memset(connData[i]->post, 0, sizeof(HttpdPostData));
	connData[i]->post->buff=NULL;
	connData[i]->post->buffLen=0;
	connData[i]->post->received=0;
	connData[i]->post->len=-1;
	connData[i]->hostName=NULL;
	connData[i]->remote_port=remPort;
	connData[i]->priv->sendBacklog=NULL;
	connData[i]->priv->sendBacklogSize=0;
	memcpy(connData[i]->remote_ip, remIp, 4);

	return 1;
}

//Httpd initialization routine. Call this to kick off webserver functionality.
void ICACHE_FLASH_ATTR httpdInit(HttpdBuiltInUrl *fixedUrls, int port) {
	int i;

	for (i=0; i<MAX_CONN; i++) {
		connData[i]=NULL;
	}
	builtInUrls=fixedUrls;

	httpdPlatInit(port, MAX_CONN);
	printf("Httpd init\n");
}
