#ifndef CGIWIFI_H
#define CGIWIFI_H

#include "httpd.h"

//WiFi access point data
typedef struct {
	char ssid[32];
	char bssid[8];
	int channel;
	char rssi;
	char enc;
} ApData;

int cgiWiFiScan(HttpdConnData *connData);
int tplWlan(HttpdConnData *connData, char *token, void **arg);
int cgiWiFi(HttpdConnData *connData);
int cgiWiFiConnect(HttpdConnData *connData);
int cgiWiFiSetMode(HttpdConnData *connData);
int cgiWiFiConnStatus(HttpdConnData *connData);

int wifiJoin(char *ssid, char *passwd);

int cgiWiFiStartScan(void (*callback)(void *data, int count), void *data);
int cgiWiFiScanDone(void);
ApData *cgiWiFiScanResult(int n);
int cgiWifiScanResultCount(void);

#endif
