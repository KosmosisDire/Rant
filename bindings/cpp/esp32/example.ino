#include <WiFi.h>

#include "rant.hpp"

static const char    *WIFI_SSID   = "ssid";
static const char    *WIFI_PASS   = "password";

static std::optional<rant::Node> g_node;
static rant::Topic               g_chat;
static uint32_t                  g_seq = 0;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\nRant esp32 pub/sub example\n");

    /* Bring WiFi up first: Rant assumes the interface already exists. */
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("connecting to %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
    Serial.printf("\nconnected, ip=%s\n", WiFi.localIP().toString().c_str());

    rant::NodeOptions opts;
    opts.max_topics = 4;
    opts.max_peers    = 8;

    g_node = rant::Node::open("esp32",
        [](const rant::MessageIn &m) {
            Serial.printf("[%.*s] %.*s > %.*s\n",
                          (int)m.publisher_name().size(),  m.publisher_name().data(),
                          (int)m.topic_name().size(), m.topic_name().data(),
                          (int)m.text().size(),         m.text().data());
        },
        [](const rant::Event &e) {
            Serial.printf("  <event> %s\n", e.to_string().c_str());
        },
        opts);
    if (!g_node) {
        Serial.println("rant_node_open failed");
        while (true) delay(1000);
    }

    rant::Qos qos;
    qos.backpressure_wait_us = 1000000; // wait for up to 1s
    qos.reliability = rant::Reliability::Reliable;

    g_chat = g_node->create_topic("chat", rant::Role::PubSub, nullptr, qos);
}

void loop()
{
  g_node->poll(10);

  char line[64];
  int  n = snprintf(line, sizeof line, "hello #%lu from esp32",
                    (unsigned long)++g_seq);
  if (g_chat.send(rant::Bytes(line, (size_t)n)) != rant::SendStatus::Ok)
      Serial.println("  (send failed)");
}
