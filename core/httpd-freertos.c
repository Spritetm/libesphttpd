/*
ESP8266 web server - platform-dependent routines, FreeRTOS version


Thanks to my collague at Espressif for writing the foundations of this code.
*/
#ifdef FREERTOS


#include <esp8266.h>
#include "httpd.h"
#include "platform.h"
#include "httpd-platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lwip/lwip/sockets.h"


#define MAX_CONN 8 //ToDo: share with httpd.c

static int httpPort;
static int httpMaxConnCt;

//ToDo: make these into a struct instead of just 2 arrays.
static int connfd[MAX_CONN]; //positive if open, -1 if closed
static int needWriteDoneNotif[MAX_CONN]; //True if a write is done and a sentCb should be done.

void ICACHE_FLASH_ATTR httpdPlatSendData(ConnTypePtr conn, char *buff, int len) {
	int x=0;
	for (x=0; x<MAX_CONN; x++) if (connfd[x]==*conn) needWriteDoneNotif[x]=1;
	write(*conn, buff, len);
}

void ICACHE_FLASH_ATTR httpdPlatDisconnect(ConnTypePtr conn) {
	int x=0;
	close(*conn);
	for (x=0; x<MAX_CONN; x++) {
		if (connfd[x]==*conn) connfd[x]=-1;
	}
}

#define RECV_BUF_SIZE 2048
static void platHttpServerTask(void *pvParameters) {
	int32 listenfd;
	int32 remotefd;
	int32 len;
	int32 ret;
	int x;
	int maxfdp = 0;
	fd_set readset,writeset;
	struct sockaddr name;
	//struct timeval timeout;
	struct sockaddr_in server_addr;
	struct sockaddr_in remote_addr;

	for (x=0; x<MAX_CONN; x++) connfd[x]=-1;

	char *precvbuf = (char*)malloc(RECV_BUF_SIZE);
	if(precvbuf==NULL) printf("platHttpServerTask: memory exhausted!\n");
	
	/* Construct local address structure */
	memset(&server_addr, 0, sizeof(server_addr)); /* Zero out structure */
	server_addr.sin_family = AF_INET;			/* Internet address family */
	server_addr.sin_addr.s_addr = INADDR_ANY;   /* Any incoming interface */
	server_addr.sin_len = sizeof(server_addr);  
	server_addr.sin_port = htons(httpPort); /* Local port */

	/* Create socket for incoming connections */
	do{
		listenfd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenfd == -1) {
			printf("platHttpServerTask: failed to create sock!\n");
			vTaskDelay(1000/portTICK_RATE_MS);
		}
	} while(listenfd == -1);

	/* Bind to the local port */
	do{
		ret = bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
		if (ret != 0) {
			printf("platHttpServerTask: failed to bind!\n");
			vTaskDelay(1000/portTICK_RATE_MS);
		}
	} while(ret != 0);

	do{
		/* Listen to the local connection */
		ret = listen(listenfd, MAX_CONN);
		if (ret != 0) {
			printf("platHttpServerTask: failed to listen!\n");
			vTaskDelay(1000/portTICK_RATE_MS);
		}
		
	}while(ret != 0);
	
	printf("esphttpd: active and listening to connections.\n");
	while(1){
		// clear fdset, and set the select function wait time
		int socketsFull=1;
		maxfdp = 0;
		FD_ZERO(&readset);
		FD_ZERO(&writeset);
		//timeout.tv_sec = 2;
		//timeout.tv_usec = 0;
		

		//
		for(x=0; x<MAX_CONN; x++){
			if (connfd[x]!=-1) {
				FD_SET(connfd[x], &readset);
				if (needWriteDoneNotif[x]) FD_SET(connfd[x], &writeset);
				if (connfd[x]>maxfdp) maxfdp=connfd[x];
			} else {
				socketsFull=0;
			}
		}
		
		if (!socketsFull) {
			FD_SET(listenfd, &readset);
			if (listenfd>maxfdp) maxfdp=listenfd;
		}

		//polling all exist client handle,wait until readable/writable
		ret = select(maxfdp+1, &readset, &writeset, NULL, NULL);//&timeout
		if(ret > 0){
			//See if we need to accept a new connection
			if (FD_ISSET(listenfd, &readset)) {
				len=sizeof(struct sockaddr_in);
				remotefd = accept(listenfd, (struct sockaddr *)&remote_addr, (socklen_t *)&len);
				if (remotefd<0) {
					printf("platHttpServerTask: Huh? Accept failed.\n");
					continue;
				}
				for(x=0; x<MAX_CONN; x++) if (connfd[x]==-1) break;
				if (x==MAX_CONN) {
					printf("platHttpServerTask: Huh? Got accept with all slots full.\n");
					continue;
				}
				int keepAlive = 1; //enable keepalive
				int keepIdle = 60; //60s
				int keepInterval = 5; //5s
				int keepCount = 3; //retry times
					
				setsockopt(remotefd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
				setsockopt(remotefd, IPPROTO_TCP, TCP_KEEPIDLE, (void*)&keepIdle, sizeof(keepIdle));
				setsockopt(remotefd, IPPROTO_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
				setsockopt(remotefd, IPPROTO_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
				
				connfd[x]=remotefd;
				needWriteDoneNotif[x]=0;
				
				len=sizeof(name);
				getpeername(remotefd, &name, (socklen_t *)&len);
				struct sockaddr_in *piname=(struct sockaddr_in *)&name;
				httpdConnectCb(&connfd[x], (char*)&piname->sin_addr.s_addr, piname->sin_port);
				//os_timer_disarm(&connData[x].conn->stop_watch);
				//os_timer_setfn(&connData[x].conn->stop_watch, (os_timer_func_t *)httpserver_conn_watcher, connData[x].conn);
				//os_timer_arm(&connData[x].conn->stop_watch, STOP_TIMER, 0);
				printf("httpserver acpt index %d sockfd %d!\n", x, remotefd);
			}
			
			//See if anything happened on the existing connections.
			for(x=0; x < MAX_CONN; x++){
				//Skip empty slots
				if (connfd[x]==-1) continue;

				//Grab remote ip/port
				struct sockaddr_in *piname;
				len=sizeof(name);
				getpeername(connfd[x], &name, (socklen_t *)&len);
				piname=(struct sockaddr_in *)&name;
				


				if (FD_ISSET(connfd[x], &readset)){
					ret=recv(connfd[x], precvbuf, RECV_BUF_SIZE,0);
					if(ret > 0){
						printf("httpserver recv index %d sockfd %d len %d!\n", x, connfd[x], ret);
						//Data received. Pass to httpd.
						httpdRecvCb(&connfd[x], (char*)&piname->sin_addr.s_addr, piname->sin_port, precvbuf, ret);
					}else{
						//recv error,connection close
						printf("httpserver close index %d sockfd %d!\n", x, connfd[x]);
						httpdDisconCb(&connfd[x], (char*)&piname->sin_addr.s_addr, piname->sin_port);
						close(connfd[x]);
						connfd[x]=-1;
					}
				}
				
				if (needWriteDoneNotif[x] && FD_ISSET(connfd[x], &writeset)){
					needWriteDoneNotif[x]=0; //Do this first, httpdSentCb may write something making this 1 again.
					httpdSentCb(&connfd[x], (char*)&piname->sin_addr.s_addr, piname->sin_port);
				}
			}
		}
	}

#if 0
//Deinit code, not used here.
	/*release data connection*/
	for(x=0; x < MAX_CONN; x++){
		//find all valid handle 
		if(connData[x].conn == NULL) continue;
		if(connData[x].conn->sockfd >= 0){
			os_timer_disarm((os_timer_t *)&connData[x].conn->stop_watch);
			close(connData[x].conn->sockfd);
			connData[x].conn->sockfd = -1;
			connData[x].conn = NULL;
			if(connData[x].cgi!=NULL) connData[x].cgi(&connData[x]); //flush cgi data
			httpdRetireConn(&connData[x]);
		}
	}
	/*release listen socket*/
	close(listenfd);

	if(NULL != precvbuf){
		free(precvbuf);
	}
	vTaskDelete(NULL);
#endif
}



//Initialize listening socket, do general initialization
void ICACHE_FLASH_ATTR httpdPlatInit(int port, int maxConnCt) {
	httpPort=port;
	httpMaxConnCt=maxConnCt;
	xTaskCreate(platHttpServerTask, (const signed char *)"esphttpd", 3*1024, NULL, 4, NULL);
}


#endif