// Combined include file for esp8266
// Actually misnamed, as it also works for ESP32.
// ToDo: Figure out better name


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FREERTOS
#include <stdint.h>

#ifdef ESP32
#include <esp_common.h>
#else
#include <espressif/esp_common.h>
#endif

#else
#include <c_types.h>
#include <ip_addr.h>
#include <espconn.h>
#include <ets_sys.h>
#include <gpio.h>
#include <mem.h>
#include <osapi.h>
#include <user_interface.h>
#include <upgrade.h>
#endif

#include "platform.h"
#include "espmissingincludes.h"
#if 0
void *ets_memcpy(void *dest, const void *src, size_t n);
void *ets_memset(void *s, int c, size_t n);
int ets_strcmp(const char *s1, const char *s2);
int ets_strncmp(const char *s1, const char *s2, int len);
size_t ets_strlen(const char *s);
char *ets_strcpy(char *dest, const char *src);
char *ets_strstr(const char *haystack, const char *needle);
int ets_sprintf(char *str, const char *format, ...)  __attribute__ ((format (printf, 2, 3)));
int os_printf_plus(const char *format, ...)  __attribute__ ((format (printf, 1, 2)));
int strcasecmp(const char *a, const char *b);

void *pvPortMalloc(size_t xWantedSize, const char *file, int line);
void vPortFree(void *ptr, const char *file, int line);
#endif
