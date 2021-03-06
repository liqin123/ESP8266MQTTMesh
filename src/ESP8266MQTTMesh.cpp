/*
 *  Copyright (C) 2016 PhracturedBlue
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ESP8266MQTTMesh.h"

#define MESH_API_VER "001"

#include "Base64.h"
#include "eboot_command.h"

#include <limits.h>
extern "C" {
  #include "user_interface.h"
  extern uint32_t _SPIFFS_start;
}

#ifndef pgm_read_with_offset //Requires Arduino core 2.4.0
    #error "This version of the ESP8266 library is not supported"
#endif

enum {
    NETWORK_LAST_INDEX = -2,
    NETWORK_MESH_NODE  = -1,
};

//Define GATEWAY_ID to the value of ESP.getChipId() in order to prevent only a specific node from connecting via MQTT
#ifdef GATEWAY_ID
    #define IS_GATEWAY (ESP.getChipId() == GATEWAY_ID)
#else
    #define IS_GATEWAY (1)
#endif

#define NEXT_STATION(station_list) STAILQ_NEXT(station_list, next)

//#define EMMDBG_LEVEL (EMMDBG_WIFI | EMMDBG_MQTT | EMMDBG_OTA)
#ifndef EMMDBG_LEVEL
  #define EMMDBG_LEVEL EMMDBG_ALL_EXTRA
#endif

#define dbgPrintln(lvl, msg) if (((lvl) & (EMMDBG_LEVEL)) == (lvl)) Serial.println("[" + String(__FUNCTION__) + "] " + msg)
size_t mesh_strlcat (char *dst, const char *src, size_t len) {
    size_t slen = strlen(dst);
    return strlcpy(dst + slen, src, len - slen);
}
#define strlcat mesh_strlcat



ESP8266MQTTMesh::ESP8266MQTTMesh(const wifi_conn *networks, const char *network_password,
                    const char *mqtt_server, int mqtt_port,
                    const char *mqtt_username, const char *mqtt_password,
                    const char *firmware_ver, int firmware_id,
                    const char *_mesh_password, const char *base_ssid, int mesh_port,
#if ASYNC_TCP_SSL_ENABLED
                    bool mqtt_secure, const uint8_t *mqtt_fingerprint, bool mesh_secure,
#endif
                    const char *inTopic, const char *outTopic
                    ) :
        networks(networks),
        network_password(network_password),
        mqtt_server(mqtt_server),
        mqtt_port(mqtt_port),
        mqtt_username(mqtt_username),
        mqtt_password(mqtt_password),
        firmware_id(firmware_id),
        firmware_ver(firmware_ver),
        base_ssid(base_ssid),
        mesh_port(mesh_port),
#if ASYNC_TCP_SSL_ENABLED
        mqtt_secure(mqtt_secure),
        mqtt_fingerprint(mqtt_fingerprint),
        mesh_secure(mesh_secure),
#endif
        inTopic(inTopic),
        outTopic(outTopic),
        espServer(mesh_port)
{

    strlcpy(mesh_password, _mesh_password, 64-strlen(MESH_API_VER));
    strlcat(mesh_password, MESH_API_VER, 64);
    espClient[0] = new AsyncClient();
    mySSID[0] = 0;
#if HAS_OTA
    uint32_t usedSize = ESP.getSketchSize();
    // round one sector up
    freeSpaceStart = (usedSize + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
    //freeSpaceEnd = (uint32_t)&_SPIFFS_start - 0x40200000;
    freeSpaceEnd = ESP.getFreeSketchSpace() + freeSpaceStart;
#endif
}

ESP8266MQTTMesh::ESP8266MQTTMesh(unsigned int firmware_id, const char *firmware_ver,
                                 const wifi_conn *networks, const char *network_password, const char *mesh_password,
                                 const char *base_ssid, const char *mqtt_server, int mqtt_port, int mesh_port,
                                 const char *inTopic,   const char *outTopic
#if ASYNC_TCP_SSL_ENABLED
                                 , bool mqtt_secure, const uint8_t *mqtt_fingerprint, bool mesh_secure
#endif
                                 ) :
    ESP8266MQTTMesh(networks, network_password, mqtt_server, mqtt_port,
                    NULL, NULL,
                    firmware_ver, firmware_id,
                    mesh_password, base_ssid, mesh_port,
#if ASYNC_TCP_SSL_ENABLED
                    mqtt_secure, mqtt_fingerprint, mesh_secure,
#endif
                    inTopic, outTopic)
{
}

void ESP8266MQTTMesh::setCallback(std::function<void(const char *topic, const char *msg)> _callback) {
    callback = _callback;
}

void ESP8266MQTTMesh::begin() {
    int len = strlen(inTopic);
    if (len > 16) {
        dbgPrintln(EMMDBG_MSG, "Max inTopicLen == 16");
        die();
    }
    if (inTopic[len-1] != '/') {
        dbgPrintln(EMMDBG_MSG, "inTopic must end with '/'");
        die();
    }
    len = strlen(outTopic);
    if (len > 16) {
        dbgPrintln(EMMDBG_MSG, "Max outTopicLen == 16");
        die();
    }
    if (outTopic[len-1] != '/') {
        dbgPrintln(EMMDBG_MSG, "outTopic must end with '/'");
        die();
    }
    if (strlen(base_ssid) > 16) {
        dbgPrintln(EMMDBG_MSG, "Max base_ssid len == 16");
        die();
    }
    if (mqtt_port == 0) {
#if ASYNC_TCP_SSL_ENABLED
        mqtt_port = mqtt_secure ? 8883 : 1883;
#else
        mqtt_port = 1883;
#endif
    }
    //dbgPrintln(EMMDBG_MSG, "Server: " + mqtt_server);
    //dbgPrintln(EMMDBG_MSG, "Port: " + String(mqtt_port));
    //dbgPrintln(EMMDBG_MSG, "User: " + mqtt_username ? mqtt_username : "None");
    //dbgPrintln(EMMDBG_MSG, "PW: " + mqtt_password? mqtt_password : "None");
    //dbgPrintln(EMMDBG_MSG, "Secure: " + mqtt_secure ? "True" : "False");
    //dbgPrintln(EMMDBG_MSG, "Mesh: " + mesh_secure ? "True" : "False");
    //dbgPrintln(EMMDBG_MSG, "Port: " + String(mesh_port));

    dbgPrintln(EMMDBG_MSG_EXTRA, "Starting Firmware " + String(firmware_id, HEX) + " : " + String(firmware_ver));
#if HAS_OTA
    dbgPrintln(EMMDBG_MSG_EXTRA, "OTA Start: 0x" + String(freeSpaceStart, HEX) + " OTA End: 0x" + String(freeSpaceEnd, HEX));
#endif
    if (! SPIFFS.begin()) {
      dbgPrintln(EMMDBG_MSG_EXTRA, "Formatting FS");
      SPIFFS.format();
      if (! SPIFFS.begin()) {
        dbgPrintln(EMMDBG_MSG, "Failed to format FS");
        die();
      }
    }
    Dir dir = SPIFFS.openDir("/bssid/");
    while(dir.next()) {
      dbgPrintln(EMMDBG_FS, " ==> '" + dir.fileName() + "'");
    }
    WiFi.disconnect();
    // In the ESP8266 2.3.0 API, there seems to be a bug which prevents a node configured as
    // WIFI_AP_STA from openning a TCP connection to it's gateway if the gateway is also
    // in WIFI_AP_STA
    WiFi.mode(WIFI_STA);

    wifiConnectHandler     = WiFi.onStationModeGotIP(            [this] (const WiFiEventStationModeGotIP& e) {                this->onWifiConnect(e);    }); 
    wifiDisconnectHandler  = WiFi.onStationModeDisconnected(     [this] (const WiFiEventStationModeDisconnected& e) {         this->onWifiDisconnect(e); });
    //wifiDHCPTimeoutHandler = WiFi.onStationModeDHCPTimeout(      [this] () {                                                  this->onDHCPTimeout();     });
    wifiAPConnectHandler   = WiFi.onSoftAPModeStationConnected(  [this] (const WiFiEventSoftAPModeStationConnected& ip) {     this->onAPConnect(ip);     });
    wifiAPDisconnectHandler= WiFi.onSoftAPModeStationDisconnected([this] (const WiFiEventSoftAPModeStationDisconnected& ip) { this->onAPDisconnect(ip);  });

    espClient[0]->setNoDelay(true);
    espClient[0]->onConnect(   [this](void * arg, AsyncClient *c)                           { this->onConnect(c);         }, this);
    espClient[0]->onDisconnect([this](void * arg, AsyncClient *c)                           { this->onDisconnect(c);      }, this);
    espClient[0]->onError(     [this](void * arg, AsyncClient *c, int8_t error)             { this->onError(c, error);    }, this);
    espClient[0]->onAck(       [this](void * arg, AsyncClient *c, size_t len, uint32_t time){ this->onAck(c, len, time);  }, this);
    espClient[0]->onTimeout(   [this](void * arg, AsyncClient *c, uint32_t time)            { this->onTimeout(c, time);   }, this);
    espClient[0]->onData(      [this](void * arg, AsyncClient *c, void* data, size_t len)   { this->onData(c, data, len); }, this);

    espServer.onClient(     [this](void * arg, AsyncClient *c){ this->onClient(c);  }, this);
    espServer.setNoDelay(true);
#if ASYNC_TCP_SSL_ENABLED
    espServer.onSslFileRequest([this](void * arg, const char *filename, uint8_t **buf) -> int { return this->onSslFileRequest(filename, buf); }, this);
    if (mesh_secure) {
        dbgPrintln(EMMDBG_WIFI, "Starting secure server");
        espServer.beginSecure("/ssl/server.cer","/ssl/server.key",NULL);
    } else
#endif
    espServer.begin();

    mqttClient.onConnect(    [this] (bool sessionPresent)                    { this->onMqttConnect(sessionPresent); });
    mqttClient.onDisconnect( [this] (AsyncMqttClientDisconnectReason reason) { this->onMqttDisconnect(reason); });
    mqttClient.onSubscribe(  [this] (uint16_t packetId, uint8_t qos)         { this->onMqttSubscribe(packetId, qos); });
    mqttClient.onUnsubscribe([this] (uint16_t packetId)                      { this->onMqttUnsubscribe(packetId); });
    mqttClient.onMessage(    [this] (char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
                                                                             { this->onMqttMessage(topic, payload, properties, len, index, total); });
    mqttClient.onPublish(    [this] (uint16_t packetId)                      { this->onMqttPublish(packetId); });
    mqttClient.setServer(mqtt_server, mqtt_port);
    if (mqtt_username || mqtt_password)
        mqttClient.setCredentials(mqtt_username, mqtt_password);

#if ASYNC_TCP_SSL_ENABLED
    mqttClient.setSecure(mqtt_secure);
    if (mqtt_fingerprint) {
        mqttClient.addServerFingerprint(mqtt_fingerprint);
    }
#endif
    //mqttClient.setCallback([this] (char* topic, byte* payload, unsigned int length) { this->mqtt_callback(topic, payload, length); });


    dbgPrintln(EMMDBG_WIFI_EXTRA, WiFi.status());
    dbgPrintln(EMMDBG_MSG_EXTRA, "Setup Complete");
    ap_idx = LAST_AP;
    connect();
}

bool ESP8266MQTTMesh::isAPConnected(uint8 *mac) {
    struct station_info *station_list = wifi_softap_get_station_info();
    while (station_list != NULL) {
        if(memcmp(mac, station_list->bssid, 6) == 0) {
            return true;
        }
        station_list = NEXT_STATION(station_list);
    }
    return false;
}

void ESP8266MQTTMesh::getMAC(IPAddress ip, uint8 *mac) {
    struct station_info *station_list = wifi_softap_get_station_info();
    while (station_list != NULL) {
        if ((&station_list->ip)->addr == ip) {
            memcpy(mac, station_list->bssid, 6);
            return;
        }
        station_list = NEXT_STATION(station_list);
    }
    memset(mac, 0, 6);
}

bool ESP8266MQTTMesh::connected() {
    return wifiConnected() && ((meshConnect && espClient[0] && espClient[0]->connected()) || mqttClient.connected());
}

bool ESP8266MQTTMesh::match_bssid(const char *bssid) {
    char filename[32];
    dbgPrintln(EMMDBG_WIFI, "Trying to match known BSSIDs for " + String(bssid));
    strlcpy(filename, "/bssid/", sizeof(filename));
    strlcat(filename, bssid, sizeof(filename));
    return SPIFFS.exists(filename);
}

void ESP8266MQTTMesh::scan() {
    //Need to rescan
    if (! scanning) {
        for(int i = 0; i < LAST_AP; i++) {
            ap[i].rssi = -99999;
            ap[i].ssid_idx = NETWORK_LAST_INDEX;
        }
        ap_idx = 0;
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        dbgPrintln(EMMDBG_WIFI, "Scanning for networks");
        WiFi.scanDelete();
        WiFi.scanNetworks(true,true);
        scanning = true;
    }
    int numberOfNetworksFound = WiFi.scanComplete();
    if (numberOfNetworksFound < 0) {
        return;
    }
    scanning = false;
    dbgPrintln(EMMDBG_WIFI, "Found: " + String(numberOfNetworksFound));
    int ssid_idx;
    for(int i = 0; i < numberOfNetworksFound; i++) {
        bool found = false;
        int network_idx = NETWORK_MESH_NODE;
        int rssi = WiFi.RSSI(i);
        dbgPrintln(EMMDBG_WIFI, "Found SSID: '" + WiFi.SSID(i) + "' BSSID '" + WiFi.BSSIDstr(i) + "'" + "RSSI: " + String(rssi));
        if (IS_GATEWAY) {
            network_idx = match_networks(WiFi.SSID().c_str(), WiFi.BSSIDstr(i).c_str());
        }
        if(network_idx == NETWORK_MESH_NODE) {
            if (! WiFi.SSID(i).length()) {
                dbgPrintln(EMMDBG_WIFI, "Did not match SSID list");
                continue;
            } else {
                if (! match_bssid(WiFi.BSSIDstr(i).c_str())) {
                    dbgPrintln(EMMDBG_WIFI, "Failed to match BSSID");
                    continue;
                }
            }
        }
        //sort by RSSI
        for(int j = 0; j < LAST_AP; j++) {
            if(ap[j].ssid_idx == NETWORK_LAST_INDEX ||
               (network_idx >= 0 &&
                  (ap[j].ssid_idx == NETWORK_MESH_NODE || rssi > ap[j].rssi)) ||
               (network_idx == NETWORK_MESH_NODE && ap[j].ssid_idx == NETWORK_MESH_NODE && rssi > ap[j].rssi))
            {
                for(int k = LAST_AP -1; k > j; k--) {
                    ap[k] = ap[k-1];
                }
                ap[j].rssi = rssi;
                ap[j].ssid_idx = network_idx;
                strlcpy(ap[j].bssid, WiFi.BSSIDstr(i).c_str(), sizeof(ap[j].bssid));
                break;
            }
        }
    }
}

int ESP8266MQTTMesh::match_networks(const char *ssid, const char *bssid)
{
#if USE_EXTENDED_NETWORKS
    for(int idx = 0; networks[idx].ssid == NULL; idx++) {
        if(networks[idx].bssid) {
            if (strcmp(bssid, networks[idx].bssid) == 0) {
                if (networks[idx].hidden && ssid[0] == 0) {
                    //hidden network
                    return idx;
                }
            } else {
                //Didn't match requested bssid
                continue;
            }
        }
        if(ssid[0] != 0) {
            if (! networks[idx].hidden && strcmp(ssid, networks[idx].ssid) == 0) {
                //matched ssid (and bssid if needed)
                return idx;
            }
        }
    }
#else
    for(int idx = 0; networks[idx] != NULL && networks[idx][0] != 0; idx++) {
        if(strcmp(ssid, networks[idx]) == 0) {
            dbgPrintln(EMMDBG_WIFI, "Matched");
            return idx;
        }
    }
#endif
    return NETWORK_MESH_NODE;
}

void ESP8266MQTTMesh::schedule_connect(float delay) {
    dbgPrintln(EMMDBG_WIFI, "Scheduling reconnect for " + String(delay,2)+ " seconds from now");
    schedule.once(delay, connect, this);
}

void ESP8266MQTTMesh::connect() {
    if (WiFi.isConnected()) {
        dbgPrintln(EMMDBG_WIFI, "Called connect when already connected!");
        return;
    }
    connecting = false;
    retry_connect = 1;
    lastReconnect = millis();
    if (scanning || ap_idx >= LAST_AP ||  ap[ap_idx].ssid_idx == NETWORK_LAST_INDEX) {
        scan();
        if (ap_idx >= LAST_AP) {
            // We got a disconnect during scan, we've been rescheduled already
            return;
        }
    } if (scanning) {
        schedule_connect(0.5);
        return;
    }
    if (ap[ap_idx].ssid_idx == NETWORK_LAST_INDEX) {
        // No networks found, try again
        schedule_connect();
        return;
    }    
    for (int i = 0; i < LAST_AP; i++) {
        if (ap[i].ssid_idx == NETWORK_LAST_INDEX)
            break;
        dbgPrintln(EMMDBG_WIFI, String(i) + String(i == ap_idx ? " * " : "   ") + String(ap[i].bssid) + " " + String(ap[i].rssi));
    }
    char ssid[64];
    if (ap[ap_idx].ssid_idx == NETWORK_MESH_NODE) {
        //This is a mesh node
        char subdomain_c[8];
        char filename[32];
        strlcpy(filename, "/bssid/", sizeof(filename));
        strlcat(filename, ap[ap_idx].bssid, sizeof(filename));
        int subdomain = read_subdomain(filename);
        if (subdomain == -1) {
            ap_idx++;
            schedule_connect();
            return;
        }
        itoa(subdomain, subdomain_c, 10);
        strlcpy(ssid, base_ssid, sizeof(ssid));
        strlcat(ssid, subdomain_c, sizeof(ssid));
        meshConnect = true;
    } else {
#if USE_EXTENDED_NETWORKS
        strlcpy(ssid, networks[ap[ap_idx].ssid_idx].ssid, sizeof(ssid));
#else
        strlcpy(ssid, networks[ap[ap_idx].ssid_idx], sizeof(ssid));
#endif
        meshConnect = false;
    }
    dbgPrintln(EMMDBG_WIFI, "Connecting to SSID : '" + String(ssid) + "' BSSID '" + String(ap[ap_idx].bssid) + "'");
    const char *password = meshConnect ? mesh_password : network_password;
    //WiFi.begin(ssid.c_str(), password.c_str(), 0, WiFi.BSSID(best_match), true);
    WiFi.begin(ssid, password);
    connecting = true;
    lastStatus = lastReconnect;
}

void ESP8266MQTTMesh::parse_message(const char *topic, const char *msg) {
  int inTopicLen = strlen(inTopic);
  if (strstr(topic, inTopic) != topic) {
      return;
  }
  const char *subtopic = topic + inTopicLen;
  if (strstr(subtopic,"bssid/") == subtopic) {
      const char *bssid = subtopic + 6;
      char filename[32];
      strlcpy(filename, "/bssid/", sizeof(filename));
      strlcat(filename, bssid, sizeof(filename));
      int idx = strtoul(msg, NULL, 10);
      int subdomain = read_subdomain(filename);
      if (subdomain == idx) {
          // The new value matches the stored value
          return;
      }
      File f = SPIFFS.open(filename, "w");
      if (! f) {
          dbgPrintln(EMMDBG_MQTT, "Failed to write /" + String(bssid));
          return;
      }
      f.print(msg);
      f.print("\n");
      f.close();

      if (strcmp(WiFi.softAPmacAddress().c_str(), bssid) == 0) {
          shutdown_AP();
          setup_AP();
      }
      return;
  }
  else if (strstr(subtopic ,"ota/") == subtopic) {
#if HAS_OTA
      const char *cmd = subtopic + 4;
      handle_ota(cmd, msg);
#endif
      return;
  }
  else if (strstr(subtopic ,"fw/") == subtopic) {
      const char *cmd = subtopic + 3;
      handle_fw(cmd);
  }
  if (! callback) {
      return;
  }
  int mySSIDLen = strlen(mySSID);
  if(strstr(subtopic, mySSID) == subtopic) {
      //Only handle messages addressed to this node
      callback(subtopic + mySSIDLen, msg);
  }
  else if(strstr(subtopic, "broadcast/") == subtopic) {
      //Or messages sent to all nodes
      callback(subtopic + 10, msg);
  }
}


void ESP8266MQTTMesh::connect_mqtt() {
    dbgPrintln(EMMDBG_MQTT, "Attempting MQTT connection (" + mqtt_server + ":" + String(mqtt_port) + ")...");
    // Attempt to connect
    mqttClient.connect();
}


void ESP8266MQTTMesh::publish(const char *subtopic, const char *msg, uint8_t msgType) {
    char topic[64];
    strlcpy(topic, outTopic, sizeof(topic));
    strlcat(topic, mySSID, sizeof(topic));
    strlcat(topic, subtopic, sizeof(topic));
    dbgPrintln(EMMDBG_MQTT_EXTRA, "Sending: " + String(topic) + "=" + String(msg));
    if (! meshConnect) {
        mqtt_publish(topic, msg, msgType);
    } else {
        send_message(0, topic, msg, msgType);
    }
}

void ESP8266MQTTMesh::shutdown_AP() {
    if(! AP_ready)
        return;
    for (int i = 1; i <= ESP8266_NUM_CLIENTS; i++) {
        if(espClient[i]) {
            delete espClient[i];
            espClient[i] = NULL;
        }
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    AP_ready = false;
}

void ESP8266MQTTMesh::setup_AP() {
    if (AP_ready)
        return;
    char filename[32];
    strlcpy(filename, "/bssid/", sizeof(filename));
    strlcat(filename, WiFi.softAPmacAddress().c_str(), sizeof(filename));
    int subdomain = read_subdomain(filename);
    if (subdomain == -1) {
        return;
    }
    char subdomainStr[4];
    itoa(subdomain, subdomainStr, 10);
    strlcpy(mySSID, base_ssid, sizeof(mySSID));
    strlcat(mySSID, subdomainStr, sizeof(mySSID));
    IPAddress apIP(192, 168, subdomain, 1);
    IPAddress apGateway(192, 168, subdomain, 1);
    IPAddress apSubmask(255, 255, 255, 0);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apGateway, apSubmask);
    WiFi.softAP(mySSID, mesh_password, WiFi.channel(), 1);
    dbgPrintln(EMMDBG_WIFI, "Initialized AP as '" + String(mySSID) + "'  IP '" + apIP.toString() + "'");
    strlcat(mySSID, "/", sizeof(mySSID));
    if (meshConnect) {
        publish("mesh_cmd", "request_bssid");
    }
    connecting = false; //Connection complete
    AP_ready = true;
}
int ESP8266MQTTMesh::read_subdomain(const char *fileName) {
      char subdomain[4];
      File f = SPIFFS.open(fileName, "r");
      if (! f) {
          dbgPrintln(EMMDBG_MSG_EXTRA, "Failed to read " + String(fileName));
          return -1;
      }
      subdomain[f.readBytesUntil('\n', subdomain, sizeof(subdomain)-1)] = 0;
      f.close();
      unsigned int value = strtoul(subdomain, NULL, 10);
      if (value < 0 || value > 255) {
          dbgPrintln(EMMDBG_MSG, "Illegal value '" + String(subdomain) + "' from " + String(fileName));
          return -1;
      }
      return value;
}
void ESP8266MQTTMesh::assign_subdomain() {
    char seen[256];
    if (match_bssid(WiFi.softAPmacAddress().c_str())) {
        return;
    }
    memset(seen, 0, sizeof(seen));
    Dir dir = SPIFFS.openDir("/bssid/");
    while(dir.next()) {
      int value = read_subdomain(dir.fileName().c_str());
      if (value == -1) {
          continue;
      }
      dbgPrintln(EMMDBG_WIFI_EXTRA, "Mapping " + dir.fileName() + " to " + String(value) + " ");
      seen[value] = 1;
    }
    for (int i = 4; i < 256; i++) {
        if (! seen[i]) {
            File f = SPIFFS.open("/bssid/" +  WiFi.softAPmacAddress(), "w");
            if (! f) {
                dbgPrintln(EMMDBG_MSG, "Couldn't write "  + WiFi.softAPmacAddress());
                die();
            }
            f.print(i);
            f.print("\n");
            f.close();
            //Yes this is meant to be inTopic.  That allows all other nodes to see this message
            char topic[TOPIC_LEN];
            char msg[4];
            itoa(i, msg, 10);
            strlcpy(topic, inTopic, sizeof(topic));
            strlcat(topic, "bssid/", sizeof(topic));
            strlcat(topic, WiFi.softAPmacAddress().c_str(), sizeof(topic));
            dbgPrintln(EMMDBG_MQTT_EXTRA, "Publishing " + String(topic) + " == " + String(i));
            mqttClient.publish(topic, 0, true, msg);
            setup_AP();
            return;
        }
    }
}

bool ESP8266MQTTMesh::send_message(int index, const char *topicOrMsg, const char *msg, uint8_t msgType) {
    int topicLen = strlen(topicOrMsg);
    int msgLen = 0;
    int len = topicLen;
    char msgTypeStr[2];
    if (msgType == 0) {
        msgType = MSG_TYPE_INVALID;
    }
    msgTypeStr[0] = msgType;
    msgTypeStr[1] = 0;
    if (index == 0) {
        //We only send the msgType upstream
        espClient[index]->write(msgTypeStr,1);
    }
    espClient[index]->write(topicOrMsg);
    if (msg) {
        espClient[index]->write("=", 1);
        espClient[index]->write(msg);
    }
    espClient[index]->write("\0", 1);
    return true;
}

void ESP8266MQTTMesh::broadcast_message(const char *topicOrMsg, const char *msg) {
    for (int i = 1; i <= ESP8266_NUM_CLIENTS; i++) {
        if (espClient[i]) {
            send_message(i, topicOrMsg, msg);
        }
    }
}

void ESP8266MQTTMesh::send_bssids(int idx) {
    Dir dir = SPIFFS.openDir("/bssid/");
    char msg[TOPIC_LEN];
    char subdomainStr[4];
    while(dir.next()) {
        int subdomain = read_subdomain(dir.fileName().c_str());
        if (subdomain == -1) {
            continue;
        }
        itoa(subdomain, subdomainStr, 10);
        strlcpy(msg, inTopic, sizeof(msg));
        strlcat(msg, "bssid/", sizeof(msg));
        strlcat(msg, dir.fileName().substring(7).c_str(), sizeof(msg)); // bssid
        strlcat(msg, "=", sizeof(msg));
        strlcat(msg, subdomainStr, sizeof(msg));
        send_message(idx, msg);
    }
}


void ESP8266MQTTMesh::handle_client_data(int idx, char *rawdata) {
            dbgPrintln(EMMDBG_MQTT, "Received: msg from " + espClient[idx]->remoteIP().toString() + " on " + (idx == 0 ? "STA" : "AP"));
            const char *data = rawdata + (idx ? 1 : 0);
            dbgPrintln(EMMDBG_MQTT_EXTRA, "--> '" + String(data) + "'");
            char topic[64];
            const char *msg;
            if (! keyValue(data, '=', topic, sizeof(topic), &msg)) {
                dbgPrintln(EMMDBG_MQTT, "Failed to handle message");
                return;
            }
            if (idx == 0) {
                //This is a packet from MQTT, need to rebroadcast to each connected station
                broadcast_message(data);
                parse_message(topic, msg);
            } else {
                unsigned char msgType = rawdata[0];
                if (strstr(topic,"/mesh_cmd")  == topic + strlen(topic) - 9) {
                    // We will handle this packet locally
                    if (0 == strcmp(msg, "request_bssid")) {
                        send_bssids(idx);
                    }
                } else {
                    if (! meshConnect) {
                        mqtt_publish(topic, msg, msgType);
                    } else {
                        send_message(0, data, NULL, msgType);
                    }
                }
            }
}

uint16_t ESP8266MQTTMesh::mqtt_publish(const char *topic, const char *msg, uint8_t msgType)
{
    uint8_t qos = 0;
    bool retain = false;
    if (msgType == MSG_TYPE_RETAIN_QOS_0
        || msgType == MSG_TYPE_RETAIN_QOS_1
        || msgType == MSG_TYPE_RETAIN_QOS_2)
    {
        qos = msgType - MSG_TYPE_RETAIN_QOS_0;
        retain = true;
    }
    else if(msgType == MSG_TYPE_QOS_0
            || msgType == MSG_TYPE_QOS_1
            || msgType == MSG_TYPE_QOS_2)
    {
        qos = msgType - MSG_TYPE_QOS_0;
    }
    mqttClient.publish(topic, qos, retain, msg);
}

bool ESP8266MQTTMesh::keyValue(const char *data, char separator, char *key, int keylen, const char **value) {
  int maxIndex = strlen(data)-1;
  int i;
  for(i=0; i<=maxIndex && i <keylen-1; i++) {
      key[i] = data[i];
      if (key[i] == separator) {
          *value = data+i+1;
          key[i] = 0;
          return true;
      }
  }
  key[i] = 0;
  *value = NULL;
  return false;
}

void ESP8266MQTTMesh::get_fw_string(char *msg, int len, const char *prefix)
{
    char id[9];
    strlcpy(msg, prefix, len);
    if (strlen(prefix)) {
        strlcat(msg, " ", len);
    }
    strlcat(msg, "ChipID: ", len);
    strlcat(msg, "ChipID: 0x", len);
    itoa(ESP.getChipId(), id, 16);
    strlcat(msg, id, len);
    strlcat(msg, " FW: 0x", len);
    itoa(firmware_id, id, 16);
    strlcat(msg, id, len);
    strlcat(msg, " : ", len);
    strlcat(msg, firmware_ver, len);
}

void ESP8266MQTTMesh::handle_fw(const char *cmd) {
    int len;
    if(strstr(cmd, mySSID) == cmd) {
        len = strlen(mySSID);
    } else if (strstr(cmd, "broadcast") == cmd) {
        len = 9;
    } else {
        return;
    }
    char msg[64];
    get_fw_string(msg, sizeof(msg), "");
    publish("fw", msg);
}

ota_info_t ESP8266MQTTMesh::parse_ota_info(const char *str) {
    ota_info_t ota_info;
    memset (&ota_info, 0, sizeof(ota_info));
    char kv[64];
    while(str) {
        keyValue(str, ',', kv, sizeof(kv), &str);
        dbgPrintln(EMMDBG_OTA_EXTRA, "Key/Value: " + String(kv));
        char key[32];
        const char *value;
        if (! keyValue(kv, ':', key, sizeof(key), &value)) {
            dbgPrintln(EMMDBG_OTA, "Failed to parse Key/Value: " + String(kv));
            continue;
        }
        dbgPrintln(EMMDBG_OTA_EXTRA, "Key: " + String(key) + " Value: " + String(value));
        if (0 == strcmp(key, "len")) {
            ota_info.len = strtoul(value, NULL, 10);
        } else if (0 == strcmp(key, "md5")) {
            if(strlen(value) == 24 && base64_dec_len(value, 24) == 16) {
              base64_decode((char *)ota_info.md5, value,  24);
            } else {
              dbgPrintln(EMMDBG_OTA, "Failed to parse md5");
            }
        }
    }
    return ota_info;
}
bool ESP8266MQTTMesh::check_ota_md5() {
    uint8_t buf[128];
    File f = SPIFFS.open("/ota", "r");
    buf[f.readBytesUntil('\n', buf, sizeof(buf)-1)] = 0;
    f.close();
    dbgPrintln(EMMDBG_OTA_EXTRA, "Read /ota: " + String((char *)buf));
    ota_info_t ota_info = parse_ota_info((char *)buf);
    if (ota_info.len > freeSpaceEnd - freeSpaceStart) {
        return false;
    }
    MD5Builder _md5;
    _md5.begin();
    uint32_t address = freeSpaceStart;
    unsigned int len = ota_info.len;
    while(len) {
        int size = len > sizeof(buf) ? sizeof(buf) : len;
        if (! ESP.flashRead(address, (uint32_t *)buf, (size + 3) & ~3)) {
            return false;
        }
        _md5.add(buf, size);
        address += size;
        len -= size;
    }
    _md5.calculate();
    _md5.getBytes(buf);
    for (int i = 0; i < 16; i++) {
        if (buf[i] != ota_info.md5[i]) {
            return false;
        }
    }
    return true;
}

void ESP8266MQTTMesh::erase_sector() {
    int start = freeSpaceStart / FLASH_SECTOR_SIZE;
    //erase flash area here
    ESP.flashEraseSector(nextErase--);
    if (nextErase >= start) {
        schedule.once(0.0, erase_sector, this);
    } else {
        nextErase = 0;
        dbgPrintln(EMMDBG_OTA, "Erase complete in " +  String((micros() - startTime) / 1000000.0, 6) + " seconds");
    }
}

void ESP8266MQTTMesh::handle_ota(const char *cmd, const char *msg) {
    dbgPrintln(EMMDBG_OTA_EXTRA, "OTA cmd " + String(cmd) + " Length: " + String(strlen(msg)));
    if(strstr(cmd, mySSID) == cmd) {
        cmd += strlen(mySSID);
    } else {
        char *end;
        unsigned int id = strtoul(cmd,&end, 16);
        if (id != firmware_id || *end != '/') {
            dbgPrintln(EMMDBG_OTA, "Ignoring OTA because firmwareID did not match " + String(firmware_id, HEX));
            return;
        }
        cmd += (end - cmd) + 1; //skip ID
    }
    if(0 == strcmp(cmd, "start")) {
        dbgPrintln(EMMDBG_OTA_EXTRA, "OTA Start");
        ota_info_t ota_info = parse_ota_info(msg);
        if (ota_info.len == 0) {
            dbgPrintln(EMMDBG_OTA, "Ignoring OTA because firmware length = 0");
            return;
        }
        dbgPrintln(EMMDBG_OTA, "-> " + String(msg));
        File f = SPIFFS.open("/ota", "w");
        f.print(msg);
        f.print("\n");
        f.close();
        f = SPIFFS.open("/ota", "r");
        char buf[128];
        buf[f.readBytesUntil('\n', buf, sizeof(buf)-1)] = 0;
        f.close();
        dbgPrintln(EMMDBG_OTA, "--> " + String(buf));
        if (ota_info.len > freeSpaceEnd - freeSpaceStart) {
            dbgPrintln(EMMDBG_MSG, "Not enough space for firmware: " + String(ota_info.len) + " > " + String(freeSpaceEnd - freeSpaceStart));
            return;
        }
        uint32_t end = (freeSpaceStart + ota_info.len + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
        nextErase = end / FLASH_SECTOR_SIZE - 1;
        startTime = micros();
        dbgPrintln(EMMDBG_OTA, "Erasing " + String((end - freeSpaceStart)/ FLASH_SECTOR_SIZE) + " sectors");
        schedule.once(0.0, erase_sector, this);
    }
    else if(0 == strcmp(cmd, "check")) {
        if (strlen(msg) > 0) {
            char out[33];
            MD5Builder _md5;
            _md5.begin();
            _md5.add((uint8_t *)msg, strlen(msg));
            _md5.calculate();
            _md5.getChars(out);
            publish("check", out);
        } else {
            const char *md5ok = check_ota_md5() ? "MD5 Passed" : "MD5 Failed";
            dbgPrintln(EMMDBG_OTA, md5ok);
            publish("check", md5ok);
        }
    }
    else if(0 == strcmp(cmd, "flash")) {
        if (! check_ota_md5()) {
            dbgPrintln(EMMDBG_MSG, "Flash failed due to md5 mismatch");
            publish("flash", "Failed");
            return;
        }
        uint8_t buf[128];
        File f = SPIFFS.open("/ota", "r");
        buf[f.readBytesUntil('\n', buf, sizeof(buf)-1)] = 0;
        ota_info_t ota_info = parse_ota_info((char *)buf);
        dbgPrintln(EMMDBG_OTA, "Flashing");
        
        eboot_command ebcmd;
        ebcmd.action = ACTION_COPY_RAW;
        ebcmd.args[0] = freeSpaceStart;
        ebcmd.args[1] = 0x00000;
        ebcmd.args[2] = ota_info.len;
        eboot_command_write(&ebcmd);
        //publish("flash", "Success");

        shutdown_AP();
        mqttClient.disconnect();
        delay(100);
        ESP.restart();
        die();
    }
    else {
        char *end;
        unsigned int address = strtoul(cmd, &end, 10);
        if (address > freeSpaceEnd - freeSpaceStart || end != cmd + strlen(cmd)) {
            dbgPrintln(EMMDBG_MSG, "Illegal address " + String(address) + " specified");
            return;
        }
        int msglen = strlen(msg);
        if (msglen > 1024) {
            dbgPrintln(EMMDBG_MSG, "Message length " + String(msglen) + " too long");
            return;
        }
        byte data[768];
        long t = micros();
        int len = base64_decode((char *)data, msg, msglen);
        if (address + len > freeSpaceEnd) {
            dbgPrintln(EMMDBG_MSG, "Message length would run past end of free space");
            return;
        }
        dbgPrintln(EMMDBG_OTA_EXTRA, "Got " + String(len) + " bytes FW @ " + String(address, HEX));
        bool ok = ESP.flashWrite(freeSpaceStart + address, (uint32_t*) data, len);
        dbgPrintln(EMMDBG_OTA, "Wrote " + String(len) + " bytes in " +  String((micros() - t) / 1000000.0, 6) + " seconds");
        if (! ok) {
            dbgPrintln(EMMDBG_MSG, "Failed to write firmware at " + String(freeSpaceStart + address, HEX) + " Length: " + String(len));
        }
    }
}

void ESP8266MQTTMesh::onWifiConnect(const WiFiEventStationModeGotIP& event) {
    if (meshConnect) {
        dbgPrintln(EMMDBG_WIFI, "Connecting to mesh: " + WiFi.gatewayIP().toString() + " on port: " + String(mesh_port));
#if ASYNC_TCP_SSL_ENABLED
        espClient[0]->connect(WiFi.gatewayIP(), mesh_port, mesh_secure);
#else
        espClient[0]->connect(WiFi.gatewayIP(), mesh_port);
#endif
        bufptr[0] = inbuffer[0];
    } else {
        dbgPrintln(EMMDBG_WIFI, "Connecting to mqtt");
        connect_mqtt();
    }
}

void ESP8266MQTTMesh::onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
    //Reasons are here: ESP8266WiFiType.h-> WiFiDisconnectReason 
    dbgPrintln(EMMDBG_WIFI, "Disconnected from Wi-Fi: " + event.ssid + " because: " + String(event.reason));
    WiFi.disconnect();
    if (! connecting) {
        ap_idx = LAST_AP;
    } else if (event.reason == WIFI_DISCONNECT_REASON_ASSOC_TOOMANY  && retry_connect) {
        // If we rebooted without a clean shutdown, we may still be associated with this AP, in which case
        // we'll be booted and should try again
        retry_connect--;
    } else {
        ap_idx++;
    }
    schedule_connect();
}

//void ESP8266MQTTMesh::onDHCPTimeout() {
//    dbgPrintln(EMMDBG_WIFI, "Failed to get DHCP info");
//}

void ESP8266MQTTMesh::onAPConnect(const WiFiEventSoftAPModeStationConnected& ip) {
    dbgPrintln(EMMDBG_WIFI, "Got connection from Station");
}

void ESP8266MQTTMesh::onAPDisconnect(const WiFiEventSoftAPModeStationDisconnected& ip) {
    dbgPrintln(EMMDBG_WIFI, "Got disconnection from Station");
}

void ESP8266MQTTMesh::onMqttConnect(bool sessionPresent) {
    dbgPrintln(EMMDBG_MQTT, "MQTT Connected");
    // Once connected, publish an announcement...
    char msg[64];
    get_fw_string(msg, sizeof(msg), "Connected");
    //strlcpy(publishMsg, outTopic, sizeof(publishMsg));
    //strlcat(publishMsg, WiFi.localIP().toString().c_str(), sizeof(publishMsg));
    mqttClient.publish("connect", 0, false, msg);
    // ... and resubscribe
    char subscribe[TOPIC_LEN];
    strlcpy(subscribe, inTopic, sizeof(subscribe));
    strlcat(subscribe, "#", sizeof(subscribe));
    mqttClient.subscribe(subscribe, 0);

    if (match_bssid(WiFi.softAPmacAddress().c_str())) {
        setup_AP();
    } else {
        //If we don't get a mapping for our BSSID within 10 seconds, define one
        schedule.once(10.0, assign_subdomain, this);
    }
}

void ESP8266MQTTMesh::onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    int r = (int8_t)reason;
    dbgPrintln(EMMDBG_MQTT, "Disconnected from MQTT: " + String(r));
#if ASYNC_TCP_SSL_ENABLED
    if (reason == AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT) {
        dbgPrintln(EMMDBG_MQTT, "Bad MQTT server fingerprint.");
        if (WiFi.isConnected()) {
            WiFi.disconnect();
        }
        return;
    }
#endif
    shutdown_AP();
    if (WiFi.isConnected()) {
        connect_mqtt();
    }
}

void ESP8266MQTTMesh::onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void ESP8266MQTTMesh::onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void ESP8266MQTTMesh::onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  memcpy(inbuffer[0], payload, len);
  inbuffer[0][len]= 0;
  dbgPrintln(EMMDBG_MQTT_EXTRA, "Message arrived [" + String(topic) + "] '" + String(inbuffer[0]) + "'");
  broadcast_message(topic, inbuffer[0]);
  parse_message(topic, inbuffer[0]);
}

void ESP8266MQTTMesh::onMqttPublish(uint16_t packetId) {
  //Serial.println("Publish acknowledged.");
  //Serial.print("  packetId: ");
  //Serial.println(packetId);
}

#if ASYNC_TCP_SSL_ENABLED
int ESP8266MQTTMesh::onSslFileRequest(const char *filename, uint8_t **buf) {
    File file = SPIFFS.open(filename, "r");
    if(file){
      size_t size = file.size();
      uint8_t * nbuf = (uint8_t*)malloc(size);
      if(nbuf){
        size = file.read(nbuf, size);
        file.close();
        *buf = nbuf;
        dbgPrintln(EMMDBG_WIFI, "SSL File: " + filename + " Size: " + String(size));
        return size;
      }
      file.close();
    }
    *buf = 0;
    dbgPrintln(EMMDBG_WIFI, "Error reading SSL File: " + filename);
    return 0;
}
#endif
void ESP8266MQTTMesh::onClient(AsyncClient* c) {
    dbgPrintln(EMMDBG_WIFI, "Got client connection from: " + c->remoteIP().toString());
    for (int i = 1; i <= ESP8266_NUM_CLIENTS; i++) {
        if (! espClient[i]) {
            espClient[i] = c;
            espClient[i]->onDisconnect([this](void * arg, AsyncClient *c)                           { this->onDisconnect(c);      }, this);
            espClient[i]->onError(     [this](void * arg, AsyncClient *c, int8_t error)             { this->onError(c, error);    }, this);
            espClient[i]->onAck(       [this](void * arg, AsyncClient *c, size_t len, uint32_t time){ this->onAck(c, len, time);  }, this);
            espClient[i]->onTimeout(   [this](void * arg, AsyncClient *c, uint32_t time)            { this->onTimeout(c, time);   }, this);
            espClient[i]->onData(      [this](void * arg, AsyncClient *c, void* data, size_t len)   { this->onData(c, data, len); }, this);
            bufptr[i] = inbuffer[i];
            return;
        }
    }
    dbgPrintln(EMMDBG_WIFI, "Discarding client connection from: " + c->remoteIP().toString());
    delete c;
}

void ESP8266MQTTMesh::onConnect(AsyncClient* c) {
    dbgPrintln(EMMDBG_WIFI, "Connected to mesh");
#if ASYNC_TCP_SSL_ENABLED
    if (mesh_secure) {
        SSL* clientSsl = c->getSSL();
        bool sslFoundFingerprint = false;
        uint8_t *fingerprint;
        if (! clientSsl) {
            dbgPrintln(EMMDBG_WIFI, "Connection is not secure");
        } else if(onSslFileRequest("/ssl/fingerprint", &fingerprint)) {
            if (ssl_match_fingerprint(clientSsl, fingerprint) == SSL_OK) {
                sslFoundFingerprint = true;
            }
            free(fingerprint);
        }

        if (!sslFoundFingerprint) {
            dbgPrintln(EMMDBG_WIFI, "Couldn't match SSL fingerprint");
            c->close(true);
            return;
        }
    }
#endif

    if (match_bssid(WiFi.softAPmacAddress().c_str())) {
        setup_AP();
    }
}

void ESP8266MQTTMesh::onDisconnect(AsyncClient* c) {
    if (c == espClient[0]) {
        dbgPrintln(EMMDBG_WIFI, "Disconnected from mesh");
        shutdown_AP();
        WiFi.disconnect();
        return;
    }
    for (int i = 1; i <= ESP8266_NUM_CLIENTS; i++) {
        if (c == espClient[i]) {
            dbgPrintln(EMMDBG_WIFI, "Disconnected from AP");
            delete espClient[i];
            espClient[i] = NULL;
        }
    }
    dbgPrintln(EMMDBG_WIFI, "Disconnected unknown client");
}
void ESP8266MQTTMesh::onError(AsyncClient* c, int8_t error) {
    dbgPrintln(EMMDBG_WIFI, "Got error on " + c->remoteIP().toString() + ": " + String(error));
}
void ESP8266MQTTMesh::onAck(AsyncClient* c, size_t len, uint32_t time) {
    dbgPrintln(EMMDBG_WIFI_EXTRA, "Got ack on " + c->remoteIP().toString() + ": " + String(len) + " / " + String(time));
}

void ESP8266MQTTMesh::onTimeout(AsyncClient* c, uint32_t time) {
    dbgPrintln(EMMDBG_WIFI, "Got timeout  " + c->remoteIP().toString() + ": " + String(time));
    c->close();
}

void ESP8266MQTTMesh::onData(AsyncClient* c, void* data, size_t len) {
    dbgPrintln(EMMDBG_WIFI_EXTRA, "Got data from " + c->remoteIP().toString());
    for (int idx = meshConnect ? 0 : 1; idx <= ESP8266_NUM_CLIENTS; idx++) {
        if (espClient[idx] == c) {
            char *dptr = (char *)data;
            for (int i = 0; i < len; i++) {
                *bufptr[idx]++ = dptr[i];
                if(! dptr[i]) {
                    handle_client_data(idx, inbuffer[idx]);
                    bufptr[idx] = inbuffer[idx];
                }
            }
            return;
        }
    }
    dbgPrintln(EMMDBG_WIFI, "Could not find client");
}
