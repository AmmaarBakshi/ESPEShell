// ============================================================================
//  MQTT pub/sub - the ONE dependency in this project not bundled with the
//  ESP32 core. Install "PubSubClient" by knolleary via Arduino Library
//  Manager before building, or this file (and the whole sketch) won't compile.
// ============================================================================
#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient s_mqttNet;
static PubSubClient s_mqtt(s_mqttNet);
static std::vector<String> s_mqttTopics;    // subscribed topics, for `mqtt status`

#define MQTT_PENDING_MAX 10
static std::vector<String> s_mqttPending;   // "topic: payload" waiting to be shown

static const char *mqttStateStr(int s) {
  switch (s) {
    case -4: return "connection timeout";
    case -3: return "connection lost";
    case -2: return "connect failed";
    case -1: return "disconnected";
    case  0: return "connected";
    case  1: return "bad protocol version";
    case  2: return "client id rejected";
    case  3: return "server unavailable";
    case  4: return "bad credentials";
    case  5: return "unauthorized";
    default: return "unknown";
  }
}

// Fires (synchronously, from mqttPoll() -> s_mqtt.loop()) for every message
// on a subscribed topic. Queues it rather than printing directly, since the
// active output stream at this moment isn't known here - ESPEShell.ino's
// loop() drains the queue into both Serial and any live Telnet session.
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg = String(topic) + ": ";
  for (unsigned int i = 0; i < length; ++i) msg += (char)payload[i];
  s_mqttPending.push_back(msg);
  if (s_mqttPending.size() > MQTT_PENDING_MAX) s_mqttPending.erase(s_mqttPending.begin());
}

void mqttPoll() {
  if (s_mqtt.connected()) s_mqtt.loop();
}

bool mqttHasPending() { return !s_mqttPending.empty(); }

String mqttPopPending() {
  if (s_mqttPending.empty()) return "";
  String m = s_mqttPending.front();
  s_mqttPending.erase(s_mqttPending.begin());
  return m;
}

static int cmd_mqtt(int argc, char **argv, ShellIO &io) {
  String sub = argc >= 2 ? String(argv[1]) : String("status");

  if (sub == "status") {
    io.out.print(F("state   : ")); io.out.println(mqttStateStr(s_mqtt.state()));
    io.out.print(F("topics  : "));
    if (s_mqttTopics.empty()) {
      io.out.println(F("(none)"));
    } else {
      for (size_t i = 0; i < s_mqttTopics.size(); ++i) { io.out.print(s_mqttTopics[i]); io.out.print(' '); }
      io.out.println();
    }
    io.out.print(F("pending : ")); io.out.println((unsigned)s_mqttPending.size());
    return 0;
  }

  if (sub == "connect") {
    if (WiFi.status() != WL_CONNECTED) { io.out.println(F("mqtt: WiFi not connected")); return 1; }
    String host = (argc >= 3) ? String(argv[2]) : String(MQTT_BROKER_HOST);
    int port = (argc >= 4) ? atoi(argv[3]) : MQTT_BROKER_PORT;
    if (host.length() == 0) { io.out.println(F("usage: mqtt connect <host> [port]   (or set MQTT_BROKER_HOST in config.h)")); return 1; }
    s_mqtt.setServer(host.c_str(), port);
    s_mqtt.setCallback(mqttCallback);
    String clientId = g_hostname + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
    io.out.printf("connecting to %s:%d as \"%s\"...\n", host.c_str(), port, clientId.c_str());
    if (s_mqtt.connect(clientId.c_str())) { io.out.println(F("connected.")); return 0; }
    io.out.printf("failed: %s (state=%d)\n", mqttStateStr(s_mqtt.state()), s_mqtt.state());
    return 1;
  }

  if (sub == "disconnect") {
    s_mqtt.disconnect();
    s_mqttTopics.clear();
    io.out.println(F("disconnected."));
    return 0;
  }

  if (!s_mqtt.connected()) {
    io.out.println(F("mqtt: not connected (try 'mqtt connect [host] [port]' first)"));
    return 1;
  }

  if (sub == "pub") {
    if (argc < 4) { io.out.println(F("usage: mqtt pub <topic> <message...> [-r]")); return 1; }
    bool retain = false;
    String topic = argv[2];
    String msg;
    for (int i = 3; i < argc; ++i) {
      if (String(argv[i]) == "-r") { retain = true; continue; }
      if (msg.length()) msg += ' ';
      msg += argv[i];
    }
    bool ok = s_mqtt.publish(topic.c_str(), msg.c_str(), retain);
    io.out.println(ok ? F("published.") : F("publish failed (message + topic must fit the ~256-byte MQTT packet buffer)."));
    return ok ? 0 : 1;
  }

  if (sub == "sub") {
    if (argc < 3) { io.out.println(F("usage: mqtt sub <topic>")); return 1; }
    String topic = argv[2];
    if (!s_mqtt.subscribe(topic.c_str())) { io.out.println(F("subscribe failed.")); return 1; }
    s_mqttTopics.push_back(topic);
    io.out.print(F("subscribed to ")); io.out.println(topic);
    io.out.println(F("(incoming messages print as '[mqtt] topic: payload' between prompts)"));
    return 0;
  }

  if (sub == "unsub") {
    if (argc < 3) { io.out.println(F("usage: mqtt unsub <topic>")); return 1; }
    String topic = argv[2];
    s_mqtt.unsubscribe(topic.c_str());
    for (size_t i = 0; i < s_mqttTopics.size(); ++i)
      if (s_mqttTopics[i] == topic) { s_mqttTopics.erase(s_mqttTopics.begin() + i); break; }
    io.out.print(F("unsubscribed from ")); io.out.println(topic);
    return 0;
  }

  io.out.println(F("usage: mqtt connect [host] [port] | status | pub <topic> <msg> [-r] | sub <topic> | unsub <topic> | disconnect"));
  return 1;
}

const Command MQTT_CMDS[] = {
  {"mqtt", cmd_mqtt, "mqtt connect|status|pub|sub|unsub|disconnect", "MQTT pub/sub (needs PubSubClient library)", G_NET},
};
const size_t MQTT_CMDS_N = sizeof(MQTT_CMDS) / sizeof(MQTT_CMDS[0]);
