/*
Cgi/template routines for the /wifi url.
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
#include "cgiwifi.h"

//Enable this to disallow any changes in AP settings
//#define DEMO_MODE

//Scan result
typedef struct {
	char scanInProgress; //if 1, don't access the underlying stuff from the webpage.
	ApData **apData;
	int noAps;
} ScanResultData;

//Static scan status storage.
static ScanResultData cgiWifiAps;

//Callback for outside scan clients
static void (*scanCallback)(void *data, int count) = NULL;
static void *scanCallbackData;

#ifndef DEMO_MODE

typedef struct {
    os_timer_t timer;
    char inProgress;
    char newMode;
    char needScan;
} NewModeData;

static NewModeData newModeData;

#endif

#define CONNTRY_IDLE 0
#define CONNTRY_WORKING 1
#define CONNTRY_SUCCESS 2
#define CONNTRY_FAIL 3
//Connection result var
static int connTryStatus=CONNTRY_IDLE;
static os_timer_t resetTimer;

//Callback the code calls when a wlan ap scan is done. Basically stores the result in
//the cgiWifiAps struct.
void ICACHE_FLASH_ATTR wifiScanDoneCb(void *arg, STATUS status) {
	int noAps, n, i;
	struct bss_info *bss_link = (struct bss_info *)arg;

	httpd_printf("wifiScanDoneCb %d\n", status);
	if (status!=OK) {
		cgiWifiAps.scanInProgress=0;
		return;
	}

	//Clear prev ap data if needed.
	if (cgiWifiAps.apData!=NULL) {
		for (n=0; n<cgiWifiAps.noAps; n++) free(cgiWifiAps.apData[n]);
		free(cgiWifiAps.apData);
	}

	//Count amount of access points found.
	noAps=0;
	while (bss_link != NULL) {
		bss_link = bss_link->next.stqe_next;
		noAps++;
	}

	//Allocate memory for access point data
	cgiWifiAps.apData=(ApData **)malloc(sizeof(ApData *)*noAps);
    memset(cgiWifiAps.apData, 0, sizeof(ApData *)*noAps);

	//Copy access point data to the static struct
	n=0;
	bss_link = (struct bss_info *)arg;
	while (bss_link != NULL) {
		if (n>=noAps) {
			//This means the bss_link changed under our nose. Shouldn't happen!
			//Break because otherwise we will write in unallocated memory.
			httpd_printf("Huh? I have more than the allocated %d aps!\n", noAps);
			break;
		}

        // check for duplicate SSIDs and keep the one with the strongest signal
        for (i = 0; i < n; ++i) {
            if (strncmp((char*)bss_link->ssid, cgiWifiAps.apData[i]->ssid, 32) == 0) {
                if (bss_link->rssi > cgiWifiAps.apData[i]->rssi) {
		            cgiWifiAps.apData[i]->rssi=bss_link->rssi;
		            cgiWifiAps.apData[i]->channel=bss_link->channel;
		            cgiWifiAps.apData[i]->enc=bss_link->authmode;
		            strncpy(cgiWifiAps.apData[i]->bssid, (char*)bss_link->bssid, 6);
                }
                break;
            }
        }

		//Save the ap data.
        if (i >= n) {
		    cgiWifiAps.apData[n]=(ApData *)malloc(sizeof(ApData));
		    cgiWifiAps.apData[n]->rssi=bss_link->rssi;
		    cgiWifiAps.apData[n]->channel=bss_link->channel;
		    cgiWifiAps.apData[n]->enc=bss_link->authmode;
		    strncpy(cgiWifiAps.apData[n]->ssid, (char*)bss_link->ssid, 32);
		    strncpy(cgiWifiAps.apData[n]->bssid, (char*)bss_link->bssid, 6);
		    n++;
        }

		bss_link = bss_link->next.stqe_next;
	}
    cgiWifiAps.noAps = n;

	//We're done.
	httpd_printf("Scan done: found %d APs\n", cgiWifiAps.noAps);
    if (scanCallback) {
        (*scanCallback)(scanCallbackData, cgiWifiAps.noAps);
        scanCallback = NULL;
    }
	cgiWifiAps.scanInProgress=0;
}

//Routine to start a WiFi access point scan.
static void ICACHE_FLASH_ATTR wifiStartScan() {
//	int x;
	if (cgiWifiAps.scanInProgress) return;
    if (newModeData.inProgress) {
        if (newModeData.newMode == STATIONAP_MODE)
            newModeData.needScan = 1;
        else
            httpd_printf("Must be in STA+AP mode to start AP scan: mode=%d\n", newModeData.newMode);
    }
    else {
        if (wifi_get_opmode() == STATIONAP_MODE) {
            httpd_printf("Starting scan...\n");
	        wifi_station_scan(NULL, wifiScanDoneCb);
 	        cgiWifiAps.scanInProgress=1;
        }
        else
            httpd_printf("Must be in STA+AP mode to start AP scan: mode=%d\n", wifi_get_opmode());
    }
}

//Routine to start a WiFi access point scan.
int ICACHE_FLASH_ATTR cgiWiFiStartScan(void (*callback)(void *data, int count), void *data) {
    if (scanCallback) return 0;
    scanCallback = callback;
    scanCallbackData = data;
    wifiStartScan();
    return 1;
}

//Routine to check the status of a WiFi access point scan.
int ICACHE_FLASH_ATTR cgiWiFiScanDone(void) {
    return cgiWifiAps.scanInProgress == 0;
}

//Routine to return a scan result.
ApData ICACHE_FLASH_ATTR *cgiWiFiScanResult(int n) {
    if (cgiWifiAps.scanInProgress)
        return NULL;
    else if (n < 0 || n >= cgiWifiAps.noAps)
        return NULL;
    return cgiWifiAps.apData[n];
}

//Routine to return the scan result count.
int cgiWifiScanResultCount(void)
{
    return cgiWifiAps.scanInProgress ? -1 : cgiWifiAps.noAps;
}
    
//This CGI is called from the bit of AJAX-code in wifi.html. It will initiate a
//scan for access points and if available will return the result of an earlier scan.
//The result is embedded in a bit of JSON parsed by the javascript in wifi.html.
int ICACHE_FLASH_ATTR cgiWiFiScan(HttpdConnData *connData) {
	int pos=(int)connData->cgiData;
	int len;
	char buff[1024];

	if (!cgiWifiAps.scanInProgress && pos!=0) {
		//Fill in json code for an access point
		if (pos-1<cgiWifiAps.noAps) {
			len=sprintf(buff, "{\"essid\": \"%s\", \"bssid\": \"" MACSTR "\", \"rssi\": \"%d\", \"enc\": \"%d\", \"channel\": \"%d\"}%s\n",
					cgiWifiAps.apData[pos-1]->ssid, MAC2STR(cgiWifiAps.apData[pos-1]->bssid), cgiWifiAps.apData[pos-1]->rssi,
					cgiWifiAps.apData[pos-1]->enc, cgiWifiAps.apData[pos-1]->channel, (pos-1==cgiWifiAps.noAps-1)?"":",");
			httpdSend(connData, buff, len);
		}
		pos++;
		if ((pos-1)>=cgiWifiAps.noAps) {
			len=sprintf(buff, "]\n}\n}\n");
			httpdSend(connData, buff, len);
			//Also start a new scan.
			wifiStartScan();
			return HTTPD_CGI_DONE;
		} else {
			connData->cgiData=(void*)pos;
			return HTTPD_CGI_MORE;
		}
	}

	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/json");
	httpdEndHeaders(connData);

	if (cgiWifiAps.scanInProgress==1) {
		//We're still scanning. Tell Javascript code that.
		len=sprintf(buff, "{\n \"result\": { \n\"inProgress\": \"1\"\n }\n}\n");
		httpdSend(connData, buff, len);
		return HTTPD_CGI_DONE;
	} else {
		//We have a scan result. Pass it on.
		len=sprintf(buff, "{\n \"result\": { \n\"inProgress\": \"0\", \n\"APs\": [\n");
		httpdSend(connData, buff, len);
		if (cgiWifiAps.apData==NULL) cgiWifiAps.noAps=0;
		connData->cgiData=(void *)1;
		return HTTPD_CGI_MORE;
	}
}

//Temp store for new ap info.
static struct station_config stconf;

//#define SWITCH_TO_STA_MODE_AFTER_CONNECT

//This routine is ran some time after a connection attempt to an access point. If
//the connect succeeds, this gets the module in STA-only mode.
static void ICACHE_FLASH_ATTR resetTimerCb(void *arg) {
	int x=wifi_station_get_connect_status();
	if (x==STATION_GOT_IP) {
		httpd_printf("Got IP address.\n");
#ifdef SWITCH_TO_STA_MODE_AFTER_CONNECT
		//Go to STA mode. This needs a reset, so do that.
		if (x!=STATION_MODE) {
            httpd_printf("Going into STA mode..\n");
		    wifi_set_opmode(STATION_MODE);
        }
#endif
	} else {
		connTryStatus=CONNTRY_FAIL;
#ifdef SWITCH_TO_STA_MODE_AFTER_CONNECT
		httpd_printf("Connect failed. Not going into STA-only mode.\n");
		//Maybe also pass this through on the webpage?
#else
        httpd_printf("Connect failed.\n");
#endif
	}
}



//Actually connect to a station. This routine is timed because I had problems
//with immediate connections earlier. It probably was something else that caused it,
//but I can't be arsed to put the code back :P
static void ICACHE_FLASH_ATTR reassTimerCb(void *arg) {
	int x;
	wifi_station_disconnect();
	wifi_station_set_config(&stconf);
	wifi_station_connect();
	x=wifi_get_opmode();
	connTryStatus=CONNTRY_WORKING;
	if (x!=STATION_MODE) {
		//Schedule disconnect/connect
		os_timer_disarm(&resetTimer);
		os_timer_setfn(&resetTimer, resetTimerCb, NULL);
		os_timer_arm(&resetTimer, 15000, 0); //time out after 15 secs of trying to connect
	}
}


//This cgi uses the routines above to connect to a specific access point with the
//given ESSID using the given password.
int ICACHE_FLASH_ATTR cgiWiFiConnect(HttpdConnData *connData) {
	char essid[128];
	char passwd[128];
	static os_timer_t reassTimer;
	
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	
	httpdFindArg(connData->post->buff, "essid", essid, sizeof(essid));
	httpdFindArg(connData->post->buff, "passwd", passwd, sizeof(passwd));

	strncpy((char*)stconf.ssid, essid, 32);
	strncpy((char*)stconf.password, passwd, 64);
	httpd_printf("Try to connect to AP %s pw %s\n", essid, passwd);

    connTryStatus=CONNTRY_IDLE;
    
	//Schedule disconnect/connect
	os_timer_disarm(&reassTimer);
	os_timer_setfn(&reassTimer, reassTimerCb, NULL);
//Set to 0 if you want to disable the actual reconnecting bit
#ifdef DEMO_MODE
	httpdRedirect(connData, "/wifi");
#else
	os_timer_arm(&reassTimer, 500, 0);
	httpdRedirect(connData, "connecting.html");
#endif
	return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR wifiJoin(char *ssid, char *passwd)
{
	static os_timer_t reassTimer;

	strncpy((char*)stconf.ssid, ssid, 32);
	strncpy((char*)stconf.password, passwd, 64);
	httpd_printf("Try to connect to AP %s pw %s\n", ssid, passwd);

    connTryStatus=CONNTRY_IDLE;
    
	//Schedule disconnect/connect
	os_timer_disarm(&reassTimer);
	os_timer_setfn(&reassTimer, reassTimerCb, NULL);
	os_timer_arm(&reassTimer, 500, 0);
	
	return 0;
}

#ifndef DEMO_MODE

static void ICACHE_FLASH_ATTR setModeCb(void *arg) {
    if (!newModeData.inProgress) return;
    switch (newModeData.newMode) {
    case STATION_MODE:
    case SOFTAP_MODE:
    case STATIONAP_MODE:
httpd_printf("setModeCb: %d\n", newModeData.newMode);
        wifi_set_opmode(newModeData.newMode);
        break;
    default:
        httpd_printf("setModeCb: invalid mode %d\n", newModeData.newMode);
        break;
    }
    if (newModeData.needScan) {
        httpd_printf("Starting deferred scan...\n");
        wifi_station_scan(NULL, wifiScanDoneCb);
        cgiWifiAps.scanInProgress=1;
    }
    newModeData.inProgress = 0;
}
#endif

//This cgi uses the routines above to connect to a specific access point with the
//given ESSID using the given password.
int ICACHE_FLASH_ATTR cgiWiFiSetMode(HttpdConnData *connData) {
	int len;
	char buff[1024];
	
	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	len=httpdFindArg(connData->getArgs, "mode", buff, sizeof(buff));
	if (len!=0) {
#ifndef DEMO_MODE
        if (os_strcmp(buff, "STA") == 0)
            newModeData.newMode = STATION_MODE;
        else if (os_strcmp(buff, "AP") == 0)
            newModeData.newMode = SOFTAP_MODE;
        else if (os_strcmp(buff, "STA+AP") == 0)
            newModeData.newMode = STATIONAP_MODE;
        else
            newModeData.newMode = atoi(buff);
httpd_printf("cgiWiFiSetMode: '%s' (%d)\n", buff, newModeData.newMode);
        newModeData.inProgress = 1;
        newModeData.needScan = 0;
        os_timer_disarm(&newModeData.timer);
        os_timer_setfn(&newModeData.timer, setModeCb, NULL);
        os_timer_arm(&newModeData.timer, 1000, 0);
#endif
	}
	httpdRedirect(connData, "/wifi");
	return HTTPD_CGI_DONE;
}

/*
 STATION_IDLE = 0,
 STATION_CONNECTING,
 STATION_WRONG_PASSWORD,
 STATION_NO_AP_FOUND,
 STATION_CONNECT_FAIL,
 STATION_GOT_IP
*/

int ICACHE_FLASH_ATTR cgiWiFiConnStatus(HttpdConnData *connData) {
	char buff[1024];
	int len;
	struct ip_info info;
	int st=wifi_station_get_connect_status();
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/json");
	httpdEndHeaders(connData);
	if (connTryStatus==CONNTRY_IDLE) {
		len=sprintf(buff, "{\n \"status\": \"idle\"\n }\n");
	} else if (connTryStatus==CONNTRY_WORKING || connTryStatus==CONNTRY_SUCCESS) {
		if (st==STATION_GOT_IP) {
			wifi_get_ip_info(0, &info);
			len=sprintf(buff, "{\n \"status\": \"success\",\n \"ip\": \"%d.%d.%d.%d\" }\n", 
				(info.ip.addr>>0)&0xff, (info.ip.addr>>8)&0xff, 
				(info.ip.addr>>16)&0xff, (info.ip.addr>>24)&0xff);
			//Reset into AP-only mode sooner.
			os_timer_disarm(&resetTimer);
			os_timer_setfn(&resetTimer, resetTimerCb, NULL);
			os_timer_arm(&resetTimer, 1000, 0);
		} else {
			len=sprintf(buff, "{\n \"status\": \"working\"\n }\n");
		}
	} else {
		len=sprintf(buff, "{\n \"status\": \"fail\"\n }\n");
	}

	httpdSend(connData, buff, len);
	return HTTPD_CGI_DONE;
}

//Template code for the WLAN page.
int ICACHE_FLASH_ATTR tplWlan(HttpdConnData *connData, char *token, void **arg) {
	char buff[1024];
	int x;
	static struct station_config stconf;
	if (token==NULL) return HTTPD_CGI_DONE;
	wifi_station_get_config(&stconf);

	strcpy(buff, "Unknown");
	if (strcmp(token, "WiFiMode")==0) {
		x=wifi_get_opmode();
		if (x==1) strcpy(buff, "Station");
		else if (x==2) strcpy(buff, "SoftAP");
		else if (x==3) strcpy(buff, "Station+SoftAP");
		else strcpy(buff, "(unknown)");
	} else if (strcmp(token, "currSsid")==0) {
		strcpy(buff, (char*)stconf.ssid);
	} else if (strcmp(token, "WiFiPasswd")==0) {
		strcpy(buff, (char*)stconf.password);
	} else if (strcmp(token, "WiFiapwarn")==0) {
		x=wifi_get_opmode();
		if (x==2) {
			strcpy(buff, "<b>Can't scan in this mode.</b> Click <a href=\"setmode.cgi?mode=3\">here</a> to go to STA+AP mode.");
		} else {
			strcpy(buff, "Click <a href=\"setmode.cgi?mode=2\">here</a> to go to standalone AP mode.");
		}
	}
	httpdSend(connData, buff, -1);
	return HTTPD_CGI_DONE;
}

