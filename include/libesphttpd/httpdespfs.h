#ifndef HTTPDESPFS_H
#define HTTPDESPFS_H

#include <libesphttpd/httpd.h>

int cgiEspFsHook(HttpdConnData *connData);
int ICACHE_FLASH_ATTR cgiEspFsTemplate(HttpdConnData *connData);

#endif
