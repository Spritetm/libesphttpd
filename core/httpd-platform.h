#ifndef HTTPD_PLATFORM_H
#define HTTPD_PLATFORM_H

void httpdPlatSendData(ConnTypePtr conn, char *buff, int len);
void httpdPlatDisconnect(ConnTypePtr conn);
void httpdPlatInit(int port, int maxConnCt);

#endif