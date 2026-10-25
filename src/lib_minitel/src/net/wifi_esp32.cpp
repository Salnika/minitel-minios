#include <Arduino.h>
#include <WiFi.h>
#include "lib_minitel/net/wifi.h"

extern "C"
{
    void lm_wifi_init(void) { WiFi.mode(WIFI_STA); }
    bool lm_wifi_is_connected(void) { return WiFi.status() == WL_CONNECTED; }
    int lm_wifi_scan(lm_wifi_network_t *out, int max_items)
    {
        if (!out || max_items <= 0)
            return 0;
        int n = WiFi.scanNetworks();
        int c = (n > max_items) ? max_items : n;
        for (int i = 0; i < c; i++)
        {
            String s = WiFi.SSID(i);
            strncpy(out[i].ssid, s.c_str(), 32);
            out[i].ssid[32] = '\0';
            out[i].rssi = WiFi.RSSI(i);
            out[i].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        WiFi.scanDelete(); // results are copied: free the driver's scan list
        return c;
    }
    bool lm_wifi_connect(const char *ssid, const char *password)
    {
        return lm_wifi_connect_cancellable(ssid, password, NULL);
    }
    bool lm_wifi_connect_cancellable(const char *ssid, const char *password, bool (*cancelled)(void))
    {
        if (!ssid)
            return false;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true, true);
        delay(200);
        if (password && strlen(password) > 0)
            WiFi.begin(ssid, password);
        else
            WiFi.begin(ssid);
        // Up to 15 s, polled often enough for a cancel to answer quickly.
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 150)
        {
            if (cancelled && cancelled())
                break;
            delay(100);
            attempts++;
        }
        return WiFi.status() == WL_CONNECTED;
    }
    void lm_wifi_disconnect(void) { WiFi.disconnect(); }
    void lm_wifi_get_ip(char *buf, size_t buflen)
    {
        if (!buf || buflen == 0)
            return;
        String ip = lm_wifi_is_connected() ? WiFi.localIP().toString() : String("x.x.x.x");
        strncpy(buf, ip.c_str(), buflen - 1);
        buf[buflen - 1] = '\0';
    }
    void lm_wifi_get_ssid(char *buf, size_t buflen)
    {
        if (!buf || buflen == 0)
            return;
        String ss = lm_wifi_is_connected() ? WiFi.SSID() : String("Non connecte");
        strncpy(buf, ss.c_str(), buflen - 1);
        buf[buflen - 1] = '\0';
    }
}
