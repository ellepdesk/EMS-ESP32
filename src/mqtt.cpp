/*
 * EMS-ESP - https://github.com/emsesp/EMS-ESP
 * Copyright 2020  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mqtt.h"
#include "emsesp.h"
#include "version.h"

namespace emsesp {

AsyncMqttClient * Mqtt::mqttClient_;

// static parameters we make global
std::string Mqtt::mqtt_base_;
uint8_t     Mqtt::mqtt_qos_;
bool        Mqtt::mqtt_retain_;
uint32_t    Mqtt::publish_time_boiler_;
uint32_t    Mqtt::publish_time_thermostat_;
uint32_t    Mqtt::publish_time_solar_;
uint32_t    Mqtt::publish_time_mixer_;
uint32_t    Mqtt::publish_time_sensor_;
uint32_t    Mqtt::publish_time_other_;
bool        Mqtt::mqtt_enabled_;
uint8_t     Mqtt::ha_climate_format_;
bool        Mqtt::ha_enabled_;
uint8_t     Mqtt::nested_format_;
uint8_t     Mqtt::subscribe_format_;

std::deque<Mqtt::QueuedMqttMessage> Mqtt::mqtt_messages_;
std::vector<Mqtt::MQTTSubFunction>  Mqtt::mqtt_subfunctions_;

uint16_t Mqtt::mqtt_publish_fails_ = 0;
bool     Mqtt::connecting_         = false;
bool     Mqtt::initialized_        = false;
uint8_t  Mqtt::connectcount_       = 0;
uint16_t Mqtt::mqtt_message_id_    = 0;
char     will_topic_[Mqtt::MQTT_TOPIC_MAX_SIZE]; // because MQTT library keeps only char pointer

uuid::log::Logger Mqtt::logger_{F_(mqtt), uuid::log::Facility::DAEMON};

// subscribe to an MQTT topic, and store the associated callback function
// only if it already hasn't been added
void Mqtt::subscribe(const uint8_t device_type, const std::string & topic, mqtt_sub_function_p cb) {
    // check if we already have the topic subscribed, if so don't add it again
    if (!mqtt_subfunctions_.empty()) {
        for (auto & mqtt_subfunction : mqtt_subfunctions_) {
            if ((mqtt_subfunction.device_type_ == device_type) && (strcmp(mqtt_subfunction.topic_.c_str(), topic.c_str()) == 0)) {
                // add the function (in case its not there) and quit because it already exists
                if (cb) {
                    mqtt_subfunction.mqtt_subfunction_ = cb;
                }
                return;
            }
        }
    }

    // register in our libary with the callback function.
    // We store the original topic without base
    mqtt_subfunctions_.emplace_back(device_type, std::move(topic), std::move(cb));

    if (!enabled()) {
        return;
    }

    LOG_DEBUG(F("Subscribing MQTT topic %s for device type %s"), topic.c_str(), EMSdevice::device_type_2_device_name(device_type).c_str());

    // add to MQTT queue as a subscribe operation
    queue_subscribe_message(topic);
}

// subscribe to the command topic if it doesn't exist yet
void Mqtt::register_command(const uint8_t device_type, const __FlashStringHelper * cmd, cmdfunction_p cb, uint8_t flags) {
    std::string cmd_topic = EMSdevice::device_type_2_device_name(device_type); // thermostat, boiler, etc...

    // see if we have already a handler for the device type (boiler, thermostat). If not add it
    bool exists = false;
    if (!mqtt_subfunctions_.empty()) {
        for (const auto & mqtt_subfunction : mqtt_subfunctions_) {
            if ((mqtt_subfunction.device_type_ == device_type) && (strcmp(mqtt_subfunction.topic_.c_str(), cmd_topic.c_str()) == 0)) {
                exists = true;
            }
        }
    }

    if (!exists) {
        Mqtt::subscribe(device_type, cmd_topic, nullptr); // use an empty function handler to signal this is a command function only (e.g. ems-esp/boiler)
        LOG_DEBUG(F("Registering MQTT cmd %s with topic %s"), uuid::read_flash_string(cmd).c_str(), EMSdevice::device_type_2_device_name(device_type).c_str());
    }

    if (!enabled()) {
        return;
    }

    // register the individual commands too (e.g. ems-esp/boiler/wwonetime)
    // https://github.com/emsesp/EMS-ESP32/issues/31
    if (subscribe_format_ == Subscribe_Format::INDIVIDUAL_ALL_HC && ((flags & CommandFlag::MQTT_SUB_FLAG_HC) == CommandFlag::MQTT_SUB_FLAG_HC)) {
        std::string topic(MQTT_TOPIC_MAX_SIZE, '\0');
        topic = cmd_topic + "/hc1/" + uuid::read_flash_string(cmd);
        queue_subscribe_message(topic);
        topic = cmd_topic + "/hc2/" + uuid::read_flash_string(cmd);
        queue_subscribe_message(topic);
        topic = cmd_topic + "/hc3/" + uuid::read_flash_string(cmd);
        queue_subscribe_message(topic);
        topic = cmd_topic + "/hc4/" + uuid::read_flash_string(cmd);
        queue_subscribe_message(topic);
    } else if (subscribe_format_ != Subscribe_Format::GENERAL && ((flags & CommandFlag::MQTT_SUB_FLAG_NOSUB) == CommandFlag::MQTT_SUB_FLAG_NOSUB)) {
        std::string topic(MQTT_TOPIC_MAX_SIZE, '\0');
        topic = cmd_topic + "/" + uuid::read_flash_string(cmd);
        queue_subscribe_message(topic);
    }
}

// subscribe to an MQTT topic, and store the associated callback function
// For generic functions not tied to a specific device
void Mqtt::subscribe(const std::string & topic, mqtt_sub_function_p cb) {
    subscribe(0, topic, cb); // no device_id needed if generic to EMS-ESP
}

// resubscribe to all MQTT topics
void Mqtt::resubscribe() {
    if (mqtt_subfunctions_.empty()) {
        return;
    }

    for (const auto & mqtt_subfunction : mqtt_subfunctions_) {
        queue_subscribe_message(mqtt_subfunction.topic_);
    }
    for (const auto & cf : Command::commands()) {
        std::string topic(MQTT_TOPIC_MAX_SIZE, '\0');
        if (subscribe_format_ == Subscribe_Format::INDIVIDUAL_ALL_HC && cf.has_flags(CommandFlag::MQTT_SUB_FLAG_HC)) {
            topic = EMSdevice::device_type_2_device_name(cf.device_type_) + "/hc1/" + uuid::read_flash_string(cf.cmd_);
            queue_subscribe_message(topic);
            topic = EMSdevice::device_type_2_device_name(cf.device_type_) + "/hc2/" + uuid::read_flash_string(cf.cmd_);
            queue_subscribe_message(topic);
            topic = EMSdevice::device_type_2_device_name(cf.device_type_) + "/hc3/" + uuid::read_flash_string(cf.cmd_);
            queue_subscribe_message(topic);
            topic = EMSdevice::device_type_2_device_name(cf.device_type_) + "/hc4/" + uuid::read_flash_string(cf.cmd_);
            queue_subscribe_message(topic);
        } else if (subscribe_format_ != Subscribe_Format::GENERAL && !cf.has_flags(CommandFlag::MQTT_SUB_FLAG_NOSUB)) {
            topic = EMSdevice::device_type_2_device_name(cf.device_type_) + "/" + uuid::read_flash_string(cf.cmd_);
            queue_subscribe_message(topic);
        }
    }
}

// Main MQTT loop - sends out top item on publish queue
void Mqtt::loop() {
    // exit if MQTT is not enabled or if there is no network connection
    if (!connected()) {
        return;
    }

    uint32_t currentMillis = uuid::get_uptime();

    // publish top item from MQTT queue to stop flooding
    if ((uint32_t)(currentMillis - last_mqtt_poll_) > MQTT_PUBLISH_WAIT) {
        last_mqtt_poll_ = currentMillis;
        process_queue();
    }

    // dallas publish on change
    if (!publish_time_sensor_) {
        EMSESP::publish_sensor_values(false);
    }

    if (!mqtt_messages_.empty()) {
        return;
    }

    // create publish messages for each of the EMS device values, adding to queue, only one device per loop
    if (publish_time_boiler_ && (currentMillis - last_publish_boiler_ > publish_time_boiler_)) {
        last_publish_boiler_ = (currentMillis / publish_time_boiler_) * publish_time_boiler_;
        EMSESP::publish_device_values(EMSdevice::DeviceType::BOILER);
    } else

        if (publish_time_thermostat_ && (currentMillis - last_publish_thermostat_ > publish_time_thermostat_)) {
        last_publish_thermostat_ = (currentMillis / publish_time_thermostat_) * publish_time_thermostat_;
        EMSESP::publish_device_values(EMSdevice::DeviceType::THERMOSTAT);
    } else

        if (publish_time_solar_ && (currentMillis - last_publish_solar_ > publish_time_solar_)) {
        last_publish_solar_ = (currentMillis / publish_time_solar_) * publish_time_solar_;
        EMSESP::publish_device_values(EMSdevice::DeviceType::SOLAR);
    } else

        if (publish_time_mixer_ && (currentMillis - last_publish_mixer_ > publish_time_mixer_)) {
        last_publish_mixer_ = (currentMillis / publish_time_mixer_) * publish_time_mixer_;
        EMSESP::publish_device_values(EMSdevice::DeviceType::MIXER);
    } else

        if (publish_time_other_ && (currentMillis - last_publish_other_ > publish_time_other_)) {
        last_publish_other_ = (currentMillis / publish_time_other_) * publish_time_other_;
        EMSESP::publish_other_values();
    } else

        if (publish_time_sensor_ && (currentMillis - last_publish_sensor_ > publish_time_sensor_)) {
        last_publish_sensor_ = (currentMillis / publish_time_sensor_) * publish_time_sensor_;
        EMSESP::publish_sensor_values(true);
    }
}

// print MQTT log and other stuff to console
void Mqtt::show_mqtt(uuid::console::Shell & shell) {
    shell.printfln(F("MQTT is %s"), connected() ? uuid::read_flash_string(F_(connected)).c_str() : uuid::read_flash_string(F_(disconnected)).c_str());

    shell.printfln(F("MQTT publish fails count: %lu"), mqtt_publish_fails_);
    shell.println();

    // show subscriptions
    shell.printfln(F("MQTT topic subscriptions:"));
    for (const auto & mqtt_subfunction : mqtt_subfunctions_) {
        shell.printfln(F(" %s/%s"), mqtt_base_.c_str(), mqtt_subfunction.topic_.c_str());
    }
    for (const auto & cf : Command::commands()) {
        if (subscribe_format_ == 2 && cf.has_flags(CommandFlag::MQTT_SUB_FLAG_HC)) {
            shell.printfln(F(" %s/%s/hc1/%s"),
                           mqtt_base_.c_str(),
                           EMSdevice::device_type_2_device_name(cf.device_type_).c_str(),
                           uuid::read_flash_string(cf.cmd_).c_str());
            shell.printfln(F(" %s/%s/hc2/%s"),
                           mqtt_base_.c_str(),
                           EMSdevice::device_type_2_device_name(cf.device_type_).c_str(),
                           uuid::read_flash_string(cf.cmd_).c_str());
            shell.printfln(F(" %s/%s/hc3/%s"),
                           mqtt_base_.c_str(),
                           EMSdevice::device_type_2_device_name(cf.device_type_).c_str(),
                           uuid::read_flash_string(cf.cmd_).c_str());
            shell.printfln(F(" %s/%s/hc4/%s"),
                           mqtt_base_.c_str(),
                           EMSdevice::device_type_2_device_name(cf.device_type_).c_str(),
                           uuid::read_flash_string(cf.cmd_).c_str());
        } else if (subscribe_format_ == 1 && !cf.has_flags(CommandFlag::MQTT_SUB_FLAG_NOSUB)) {
            shell.printfln(F(" %s/%s/%s"),
                           mqtt_base_.c_str(),
                           EMSdevice::device_type_2_device_name(cf.device_type_).c_str(),
                           uuid::read_flash_string(cf.cmd_).c_str());
        }
    }
    shell.println();

    // show queues
    if (mqtt_messages_.empty()) {
        shell.printfln(F("MQTT queue is empty"));
        shell.println();
        return;
    }

    shell.printfln(F("MQTT queue (%d/%d messages):"), mqtt_messages_.size(), MAX_MQTT_MESSAGES);

    for (const auto & message : mqtt_messages_) {
        auto content = message.content_;
        char topic[MQTT_TOPIC_MAX_SIZE];
        if ((strncmp(content->topic.c_str(), "homeassistant/", 13) != 0)) {
            snprintf(topic, sizeof(topic), "%s/%s", Mqtt::base().c_str(), content->topic.c_str());
        } else {
            snprintf(topic, sizeof(topic), "%s", content->topic.c_str());
        }

        if (content->operation == Operation::PUBLISH) {
            // Publish messages
            if (message.retry_count_ == 0) {
                if (message.packet_id_ == 0) {
                    shell.printfln(F(" [%02d] (Pub) topic=%s payload=%s"), message.id_, topic, content->payload.c_str());
                } else {
                    shell.printfln(F(" [%02d] (Pub) topic=%s payload=%s (pid %d)"), message.id_, topic, content->payload.c_str(), message.packet_id_);
                }
            } else {
                shell.printfln(F(" [%02d] (Pub) topic=%s payload=%s (pid %d, retry #%d)"),
                               message.id_,
                               topic,
                               content->payload.c_str(),
                               message.packet_id_,
                               message.retry_count_);
            }
        } else {
            // Subscribe messages
            shell.printfln(F(" [%02d] (Sub) topic=%s"), message.id_, topic);
        }
    }
    shell.println();
}

// simulate receiving a MQTT message, used only for testing
void Mqtt::incoming(const char * topic, const char * payload) {
    on_message(topic, payload, strlen(payload));
}

// received an MQTT message that we subscribed too
void Mqtt::on_message(const char * fulltopic, const char * payload, size_t len) {
    if (len == 0) {
        LOG_DEBUG(F("Received empty message %s"), fulltopic);
        return; // ignore empty payloads
    }
    if (strncmp(fulltopic, mqtt_base_.c_str(), strlen(mqtt_base_.c_str())) != 0) {
        LOG_DEBUG(F("Received unknown message %s - %s"), fulltopic, payload);
        return; // not for us
    }
    char topic[100];
    strlcpy(topic, &fulltopic[1 + strlen(mqtt_base_.c_str())], 100);

    // strip the topic substrings
    char * topic_end = strchr(topic, '/');
    if (topic_end != nullptr) {
        topic_end[0] = '\0';
    }

    // convert payload to a null-terminated char string
    char message[len + 2];
    strlcpy(message, payload, len + 1);

    LOG_DEBUG(F("Received %s => %s (length %d)"), topic, message, len);

    // see if we have this topic in our subscription list, then call its callback handler
    for (const auto & mf : mqtt_subfunctions_) {
        if (strcmp(topic, mf.topic_.c_str()) == 0) {
            // if we have callback function then call it
            // otherwise proceed as process as a command
            if (mf.mqtt_subfunction_) {
                if (!(mf.mqtt_subfunction_)(message)) {
                    LOG_ERROR(F("MQTT error: invalid payload %s for this topic %s"), message, topic);
                    Mqtt::publish(F_(response), "invalid");
                }
                return;
            }

            // check if it's not json, then try and extract the command from the topic name
            if (message[0] != '{') {
                // get topic with substrings again
                strlcpy(topic, &fulltopic[1 + strlen(mqtt_base_.c_str())], 100);
                char * cmd_only = strchr(topic, '/');
                if (cmd_only == NULL) {
                    return; // invalid topic name
                }
                cmd_only++; // skip the /
                // LOG_INFO(F("devicetype= %d, topic = %s, cmd = %s, message =  %s), mf.device_type_, topic, cmd_only, message);
                // call command, assume admin authentication is allowed
                uint8_t cmd_return = Command::call(mf.device_type_, cmd_only, message, true);
                if (cmd_return == CommandRet::NOT_FOUND) {
                    LOG_ERROR(F("No matching cmd (%s) in topic %s"), cmd_only, topic);
                    Mqtt::publish(F_(response), "unknown");
                } else if (cmd_return != CommandRet::OK) {
                    LOG_ERROR(F("Invalid data with cmd (%s) in topic %s"), cmd_only, topic);
                    Mqtt::publish(F_(response), "unknown");
                }
                return;
            }

            // It's a command then with the payload being JSON like {"cmd":"<cmd>", "data":<data>, "id":<n>}
            // Find the command from the json and call it directly
            StaticJsonDocument<EMSESP_JSON_SIZE_SMALL> doc;
            DeserializationError                       error = deserializeJson(doc, message);
            if (error) {
                LOG_ERROR(F("MQTT error: payload %s, error %s"), message, error.c_str());
                return;
            }

            const char * command = doc["cmd"];
            if (command == nullptr) {
                LOG_ERROR(F("MQTT error: invalid payload cmd format. message=%s"), message);
                return;
            }

            // check for hc and id, and convert to int
            int8_t n = -1; // no value
            if (doc.containsKey("hc")) {
                n = doc["hc"];
            } else if (doc.containsKey("id")) {
                n = doc["id"];
            }

            uint8_t     cmd_return = CommandRet::OK;
            JsonVariant data       = doc["data"];

            if (data.is<const char *>()) {
                cmd_return = Command::call(mf.device_type_, command, data.as<const char *>(), true, n);
            } else if (data.is<int>()) {
                char data_str[10];
                cmd_return = Command::call(mf.device_type_, command, Helpers::itoa(data_str, (int16_t)data.as<int>()), true, n);
            } else if (data.is<float>()) {
                char data_str[10];
                cmd_return = Command::call(mf.device_type_, command, Helpers::render_value(data_str, (float)data.as<float>(), 2), true, n);
            } else if (data.isNull()) {
                DynamicJsonDocument resp(EMSESP_JSON_SIZE_XLARGE_DYN);
                JsonObject          json = resp.to<JsonObject>();
                cmd_return               = Command::call(mf.device_type_, command, "", true, n, json);
                if (json.size()) {
                    Mqtt::publish(F_(response), resp.as<JsonObject>());
                    return;
                }
            }

            if (cmd_return == CommandRet::NOT_FOUND) {
                LOG_ERROR(F("No matching cmd (%s)"), command);
                Mqtt::publish(F_(response), "unknown");
            } else if (cmd_return != CommandRet::OK) {
                LOG_ERROR(F("Invalid data for cmd (%s)"), command);
                Mqtt::publish(F_(response), "unknown");
            }

            return;
        }
    }

    // if we got here we didn't find a topic match
    LOG_ERROR(F("No MQTT handler found for topic %s and payload %s"), topic, message);
}

// print all the topics related to a specific device type
void Mqtt::show_topic_handlers(uuid::console::Shell & shell, const uint8_t device_type) {
    if (std::count_if(mqtt_subfunctions_.cbegin(),
                      mqtt_subfunctions_.cend(),
                      [=](MQTTSubFunction const & mqtt_subfunction) { return device_type == mqtt_subfunction.device_type_; })
        == 0) {
        return;
    }

    shell.print(F(" Subscribed MQTT topics: "));
    for (const auto & mqtt_subfunction : mqtt_subfunctions_) {
        if (mqtt_subfunction.device_type_ == device_type) {
            shell.printf(F("%s/%s "), mqtt_base_.c_str(), mqtt_subfunction.topic_.c_str());
        }
    }
    shell.println();
}

// called when an MQTT Publish ACK is received
// its a poor man's QOS we assume the ACK represents the last Publish sent
// check if ACK matches the last Publish we sent, if not report an error. Only if qos is 1 or 2
// and always remove from queue
void Mqtt::on_publish(uint16_t packetId) {
    // find the MQTT message in the queue and remove it
    if (mqtt_messages_.empty()) {
#if defined(EMSESP_DEBUG)
        LOG_DEBUG(F("[DEBUG] No message stored for ACK pid %d"), packetId);
#endif
        return;
    }

    auto mqtt_message = mqtt_messages_.front();

    // if the last published failed, don't bother checking it. wait for the next retry
    if (mqtt_message.packet_id_ == 0) {
#if defined(EMSESP_DEBUG)
        LOG_DEBUG(F("[DEBUG] ACK for failed message pid 0"));
#endif
        return;
    }

    if (mqtt_message.packet_id_ != packetId) {
        LOG_ERROR(F("Mismatch, expecting PID %d, got %d"), mqtt_message.packet_id_, packetId);
        mqtt_publish_fails_++; // increment error count
    }

#if defined(EMSESP_DEBUG)
    LOG_DEBUG(F("[DEBUG] ACK pid %d"), packetId);
#endif

    mqtt_messages_.pop_front(); // always remove from queue, regardless if there was a successful ACK
}

// called when MQTT settings have changed via the Web forms
void Mqtt::reset_mqtt() {
    if (!mqttClient_) {
        return;
    }

    if (mqttClient_->connected()) {
        mqttClient_->disconnect(true); // force a disconnect
    }
}

void Mqtt::load_settings() {
    EMSESP::esp8266React.getMqttSettingsService()->read([&](MqttSettings & mqttSettings) {
        mqtt_base_         = mqttSettings.base.c_str(); // Convert String to std::string
        mqtt_qos_          = mqttSettings.mqtt_qos;
        mqtt_retain_       = mqttSettings.mqtt_retain;
        mqtt_enabled_      = mqttSettings.enabled;
        ha_enabled_        = mqttSettings.ha_enabled;
        ha_climate_format_ = mqttSettings.ha_climate_format;
        nested_format_     = mqttSettings.nested_format;
        subscribe_format_  = mqttSettings.subscribe_format;

        // convert to milliseconds
        publish_time_boiler_     = mqttSettings.publish_time_boiler * 1000;
        publish_time_thermostat_ = mqttSettings.publish_time_thermostat * 1000;
        publish_time_solar_      = mqttSettings.publish_time_solar * 1000;
        publish_time_mixer_      = mqttSettings.publish_time_mixer * 1000;
        publish_time_other_      = mqttSettings.publish_time_other * 1000;
        publish_time_sensor_     = mqttSettings.publish_time_sensor * 1000;
    });
}

void Mqtt::start() {
    mqttClient_ = EMSESP::esp8266React.getMqttClient();

    load_settings(); // fetch MQTT settings

    if (!mqtt_enabled_) {
        return; // quit, not using MQTT
    }

    // if already initialized, don't do it again
    if (initialized_) {
        return;
    }
    initialized_ = true;

    mqttClient_->onConnect([this](bool sessionPresent) { on_connect(); });

    mqttClient_->onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
        if (!connecting_) {
            return;
        }
        connecting_ = false;
        if (reason == AsyncMqttClientDisconnectReason::TCP_DISCONNECTED) {
            LOG_INFO(F("MQTT disconnected: TCP"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED) {
            LOG_INFO(F("MQTT disconnected: Identifier Rejected"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE) {
            LOG_INFO(F("MQTT disconnected: Server unavailable"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS) {
            LOG_INFO(F("MQTT disconnected: Malformed credentials"));
        }
        if (reason == AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED) {
            LOG_INFO(F("MQTT disconnected: Not authorized"));
        }
        // remove message with pending ack
        if (!mqtt_messages_.empty()) {
            auto mqtt_message = mqtt_messages_.front();
            if (mqtt_message.packet_id_ != 0) {
                mqtt_messages_.pop_front();
            }
        }
        // mqtt_messages_.clear();
    });

    // create will_topic with the base prefixed. It has to be static because asyncmqttclient destroys the reference
    static char will_topic[MQTT_TOPIC_MAX_SIZE];
    snprintf(will_topic, MQTT_TOPIC_MAX_SIZE, "%s/status", mqtt_base_.c_str());
    mqttClient_->setWill(will_topic, 1, true, "offline"); // with qos 1, retain true

    mqttClient_->onMessage([this](char * topic, char * payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
        // receiving mqtt
        on_message(topic, payload, len);
    });

    mqttClient_->onPublish([this](uint16_t packetId) {
        // publish
        on_publish(packetId);
    });

    // create space for command buffer, to avoid heap memory fragmentation
    mqtt_subfunctions_.reserve(5);
}

void Mqtt::set_publish_time_boiler(uint16_t publish_time) {
    publish_time_boiler_ = publish_time * 1000; // convert to milliseconds
}

void Mqtt::set_publish_time_thermostat(uint16_t publish_time) {
    publish_time_thermostat_ = publish_time * 1000; // convert to milliseconds
}

void Mqtt::set_publish_time_solar(uint16_t publish_time) {
    publish_time_solar_ = publish_time * 1000; // convert to milliseconds
}

void Mqtt::set_publish_time_mixer(uint16_t publish_time) {
    publish_time_mixer_ = publish_time * 1000; // convert to milliseconds
}

void Mqtt::set_publish_time_other(uint16_t publish_time) {
    publish_time_other_ = publish_time * 1000; // convert to milliseconds
}

void Mqtt::set_publish_time_sensor(uint16_t publish_time) {
    publish_time_sensor_ = publish_time * 1000; // convert to milliseconds
}

bool Mqtt::get_publish_onchange(uint8_t device_type) {
    if (device_type == EMSdevice::DeviceType::BOILER) {
        if (!publish_time_boiler_) {
            return true;
        }
    } else if (device_type == EMSdevice::DeviceType::THERMOSTAT) {
        if (!publish_time_thermostat_) {
            return true;
        }
    } else if (device_type == EMSdevice::DeviceType::SOLAR) {
        if (!publish_time_solar_) {
            return true;
        }
    } else if (device_type == EMSdevice::DeviceType::MIXER) {
        if (!publish_time_mixer_) {
            return true;
        }
    } else if (!publish_time_other_) {
        return true;
    }
    return false;
}

// MQTT onConnect - when an MQTT connect is established
// send out some inital MQTT messages
void Mqtt::on_connect() {
    if (connecting_) { // prevent duplicated connections
        return;
    }

    LOG_INFO(F("MQTT connected"));

    connecting_ = true;
    connectcount_++;

    load_settings(); // reload MQTT settings - in case they have changes

    // send info topic appended with the version information as JSON
    StaticJsonDocument<EMSESP_JSON_SIZE_SMALL> doc;
    // first time to connect
    if (connectcount_ == 1) {
        doc["event"] = FJSON("start");
    } else {
        doc["event"] = FJSON("reconnect");
    }

    doc["version"] = EMSESP_APP_VERSION;
#ifndef EMSESP_STANDALONE
    if (EMSESP::system_.ethernet_connected()) {
        doc["ip"] = ETH.localIP().toString();
        if (ETH.localIPv6().toString() != "0000:0000:0000:0000:0000:0000:0000:0000") {
            doc["ipv6"] = ETH.localIPv6().toString();
        }
    } else {
        doc["ip"] = WiFi.localIP().toString();
        if (WiFi.localIPv6().toString() != "0000:0000:0000:0000:0000:0000:0000:0000") {
            doc["ipv6"] = WiFi.localIPv6().toString();
        }
    }
#endif
    publish(F_(info), doc.as<JsonObject>()); // topic called "info"

    // create the EMS-ESP device in HA, which is MQTT retained
    if (ha_enabled()) {
        ha_status();
    }

    // send initial MQTT messages for some of our services
    EMSESP::shower_.send_mqtt_stat(false); // Send shower_activated as false
    EMSESP::system_.send_heartbeat();      // send heatbeat

    // re-subscribe to all MQTT topics
    resubscribe();
    EMSESP::reset_mqtt_ha(); // re-create all HA devices if there are any

    publish_retain(F("status"), "online", true); // say we're alive to the Last Will topic, with retain on

    mqtt_publish_fails_ = 0; // reset fail count to 0

    /*
    // for debugging only
    LOG_INFO("Queue size: %d", mqtt_messages_.size());
    for (const auto & message : mqtt_messages_) {
        auto content = message.content_;
        LOG_INFO(F(" [%02d] (%d) topic=%s payload=%s"), message.id_, content->operation, content->topic.c_str(), content->payload.c_str());
    }
    */
}

// Home Assistant Discovery - the main master Device called EMS-ESP
// e.g. homeassistant/sensor/ems-esp/status/config
// all the values from the heartbeat payload will be added as attributes to the entity state
void Mqtt::ha_status() {
    StaticJsonDocument<EMSESP_JSON_SIZE_HA_CONFIG> doc;

    doc["uniq_id"] = FJSON("ems-esp-system");
    doc["~"]       = mqtt_base_; // default ems-esp
    // doc["avty_t"]      = FJSON("~/status"); // commented out, as it causes errors in HA sometimes
    // doc["json_attr_t"] = FJSON("~/heartbeat"); // store also as HA attributes
    doc["stat_t"]  = FJSON("~/heartbeat");
    doc["name"]    = FJSON("EMS-ESP status");
    doc["ic"]      = F_(icondevice);
    doc["val_tpl"] = FJSON("{{value_json['status']}}");

    JsonObject dev = doc.createNestedObject("dev");
    dev["name"]    = F_(EMSESP); // "EMS-ESP"
    dev["sw"]      = EMSESP_APP_VERSION;
    dev["mf"]      = FJSON("proddy");
    dev["mdl"]     = F_(EMSESP); // "EMS-ESP"
    JsonArray ids  = dev.createNestedArray("ids");
    ids.add("ems-esp");

    char topic[MQTT_TOPIC_MAX_SIZE];
    snprintf(topic, sizeof(topic), "sensor/%s/system/config", mqtt_base_.c_str());
    Mqtt::publish_ha(topic, doc.as<JsonObject>()); // publish the config payload with retain flag

    // create the sensors - must match the MQTT payload keys
    if (!EMSESP::system_.ethernet_connected()) {
        publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("WiFi RSSI"), EMSdevice::DeviceType::SYSTEM, F("rssi"), DeviceValueUOM::DBM);
        publish_ha_sensor(DeviceValueType::INT,
                          DeviceValueTAG::TAG_HEARTBEAT,
                          F("WiFi strength"),
                          EMSdevice::DeviceType::SYSTEM,
                          F("wifistrength"),
                          DeviceValueUOM::PERCENT);
    }

    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("Uptime"), EMSdevice::DeviceType::SYSTEM, F("uptime"));
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("Uptime (sec)"), EMSdevice::DeviceType::SYSTEM, F("uptime_sec"), DeviceValueUOM::SECONDS);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("Free memory"), EMSdevice::DeviceType::SYSTEM, F("freemem"), DeviceValueUOM::KB);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# MQTT fails"), EMSdevice::DeviceType::SYSTEM, F("mqttfails"), DeviceValueUOM::NONE);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# Rx received"), EMSdevice::DeviceType::SYSTEM, F("rxreceived"), DeviceValueUOM::NONE);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# Rx fails"), EMSdevice::DeviceType::SYSTEM, F("rxfails"), DeviceValueUOM::NONE);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# Tx reads"), EMSdevice::DeviceType::SYSTEM, F("txread"), DeviceValueUOM::NONE);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# Tx writes"), EMSdevice::DeviceType::SYSTEM, F("txwrite"), DeviceValueUOM::NONE);
    publish_ha_sensor(DeviceValueType::INT, DeviceValueTAG::TAG_HEARTBEAT, F("# Tx fails"), EMSdevice::DeviceType::SYSTEM, F("txfails"), DeviceValueUOM::NONE);
}

// add sub or pub task to the queue.
// a fully-qualified topic is created by prefixing the base, unless it's HA
// returns a pointer to the message created
std::shared_ptr<const MqttMessage> Mqtt::queue_message(const uint8_t operation, const std::string & topic, const std::string & payload, bool retain) {
    if (topic.empty()) {
        return nullptr;
    }

    // take the topic and prefix the base, unless its for HA
    std::shared_ptr<MqttMessage> message;
    message = std::make_shared<MqttMessage>(operation, topic, payload, retain);

    // LOG_INFO("Added to queue: %s %s", message->topic.c_str(), message->payload.c_str()); // debugging only

    // if the queue is full, make room but removing the last one
    if (mqtt_messages_.size() >= MAX_MQTT_MESSAGES) {
        mqtt_messages_.pop_front();
    }
    mqtt_messages_.emplace_back(mqtt_message_id_++, std::move(message));

    return mqtt_messages_.back().content_; // this is because the message has been moved
}

// add MQTT message to queue, payload is a string
std::shared_ptr<const MqttMessage> Mqtt::queue_publish_message(const std::string & topic, const std::string & payload, bool retain) {
    if (!enabled()) {
        return nullptr;
    };
    return queue_message(Operation::PUBLISH, topic, payload, retain);
}

// add MQTT subscribe message to queue
std::shared_ptr<const MqttMessage> Mqtt::queue_subscribe_message(const std::string & topic) {
    return queue_message(Operation::SUBSCRIBE, topic, "", false); // no payload
}

// MQTT Publish, using a user's retain flag
void Mqtt::publish(const std::string & topic, const std::string & payload) {
    queue_publish_message(topic, payload, mqtt_retain_);
}

// MQTT Publish, using a user's retain flag - except for char * strings
void Mqtt::publish(const __FlashStringHelper * topic, const char * payload) {
    queue_publish_message(uuid::read_flash_string(topic), payload, mqtt_retain_);
}

// MQTT Publish, using a specific retain flag, topic is a flash string
void Mqtt::publish(const __FlashStringHelper * topic, const std::string & payload) {
    queue_publish_message(uuid::read_flash_string(topic), payload, mqtt_retain_);
}

void Mqtt::publish(const __FlashStringHelper * topic, const JsonObject & payload) {
    publish(uuid::read_flash_string(topic), payload);
}

// publish json doc, only if its not empty
void Mqtt::publish(const std::string & topic, const JsonObject & payload) {
    publish_retain(topic, payload, mqtt_retain_);
}

// no payload
void Mqtt::publish(const std::string & topic) {
    queue_publish_message(topic, "", false);
}

// MQTT Publish, using a specific retain flag, topic is a flash string, forcing retain flag
void Mqtt::publish_retain(const __FlashStringHelper * topic, const std::string & payload, bool retain) {
    queue_publish_message(uuid::read_flash_string(topic), payload, retain);
}

// publish json doc, only if its not empty, using the retain flag
void Mqtt::publish_retain(const std::string & topic, const JsonObject & payload, bool retain) {
    if (enabled() && payload.size()) {
        std::string payload_text;
        serializeJson(payload, payload_text); // convert json to string
        queue_publish_message(topic, payload_text, retain);
    }
}

void Mqtt::publish_retain(const __FlashStringHelper * topic, const JsonObject & payload, bool retain) {
    publish_retain(uuid::read_flash_string(topic), payload, retain);
}

void Mqtt::publish_ha(const __FlashStringHelper * topic, const JsonObject & payload) {
    publish_ha(uuid::read_flash_string(topic), payload);
}

// publish a Home Assistant config topic and payload, with retain flag off.
void Mqtt::publish_ha(const std::string & topic, const JsonObject & payload) {
    if (!enabled()) {
        return;
    }

    // empty payload will remove the previous config
    // publish(topic);

    std::string payload_text;
    payload_text.reserve(measureJson(payload) + 1);
    serializeJson(payload, payload_text); // convert json to string

    std::string fulltopic = uuid::read_flash_string(F_(homeassistant)) + topic;
#if defined(EMSESP_STANDALONE)
    LOG_DEBUG(F("Publishing HA topic=%s, payload=%s"), fulltopic.c_str(), payload_text.c_str());
#elif defined(EMSESP_DEBUG)
    LOG_DEBUG(F("[debug] Publishing HA topic=%s, payload=%s"), fulltopic.c_str(), payload_text.c_str());
#endif

    // queue messages if the MQTT connection is not yet established. to ensure we don't miss messages
    queue_publish_message(fulltopic, payload_text, true); // with retain true
}

// take top from queue and perform the publish or subscribe action
// assumes there is an MQTT connection
void Mqtt::process_queue() {
    if (mqtt_messages_.empty()) {
        return;
    }

    // fetch first from queue and create the full topic name
    auto mqtt_message = mqtt_messages_.front();
    auto message      = mqtt_message.content_;
    char topic[MQTT_TOPIC_MAX_SIZE];
    if (message->topic.find(uuid::read_flash_string(F_(homeassistant))) == 0) {
        // leave topic as it is
        strcpy(topic, message->topic.c_str());
    } else {
        snprintf(topic, MQTT_TOPIC_MAX_SIZE, "%s/%s", mqtt_base_.c_str(), message->topic.c_str());
    }

    // if we're subscribing...
    if (message->operation == Operation::SUBSCRIBE) {
        LOG_DEBUG(F("Subscribing to topic: %s"), topic);
        uint16_t packet_id = mqttClient_->subscribe(topic, mqtt_qos_);
        if (!packet_id) {
            LOG_DEBUG(F("Error subscribing to %s"), topic);
        }

        mqtt_messages_.pop_front(); // remove the message from the queue

        return;
    }

    // if this has already been published and we're waiting for an ACK, don't publish again
    // it will have a real packet ID
    if (mqtt_message.packet_id_ > 0) {
#if defined(EMSESP_DEBUG)
        LOG_DEBUG(F("[DEBUG] Waiting for QOS-ACK"));
#endif
        return;
    }

    // else try and publish it
    uint16_t packet_id = mqttClient_->publish(topic, mqtt_qos_, message->retain, message->payload.c_str(), message->payload.size(), false, mqtt_message.id_);
    LOG_DEBUG(F("Publishing topic %s (#%02d, retain=%d, retry=%d, size=%d, pid=%d)"),
              topic,
              mqtt_message.id_,
              message->retain,
              mqtt_message.retry_count_ + 1,
              message->payload.size(),
              packet_id);

    if (packet_id == 0) {
        // it failed. if we retried n times, give up. remove from queue
        if (mqtt_message.retry_count_ == (MQTT_PUBLISH_MAX_RETRY - 1)) {
            LOG_ERROR(F("Failed to publish to %s after %d attempts"), topic, mqtt_message.retry_count_ + 1);
            mqtt_publish_fails_++;      // increment failure counter
            mqtt_messages_.pop_front(); // delete
            return;
        } else {
            // update the record
            mqtt_messages_.front().retry_count_++;
            LOG_DEBUG(F("Failed to publish to %s. Trying again, #%d"), topic, mqtt_message.retry_count_ + 1);
            return; // leave on queue for next time so it gets republished
        }
    }

    // if we have ACK set with QOS 1 or 2, leave on queue and let the ACK process remove it
    // but add the packet_id so we can check it later
    if (mqtt_qos_ != 0) {
        mqtt_messages_.front().packet_id_ = packet_id;
#if defined(EMSESP_DEBUG)
        LOG_DEBUG(F("[DEBUG] Setting packetID for ACK to %d"), packet_id);
#endif
        return;
    }

    mqtt_messages_.pop_front(); // remove the message from the queue
}

// HA config for a sensor and binary_sensor entity
// entity must match the key/value pair in the *_data topic
// note: some string copying here into chars, it looks messy but does help with heap fragmentation issues
void Mqtt::publish_ha_sensor(uint8_t                     type, // EMSdevice::DeviceValueType
                             uint8_t                     tag,  // EMSdevice::DeviceValueTAG
                             const __FlashStringHelper * name,
                             const uint8_t               device_type, // EMSdevice::DeviceType
                             const __FlashStringHelper * entity,
                             const uint8_t               uom) { // EMSdevice::DeviceValueUOM (0=NONE)
    // ignore if name (fullname) is empty
    if (name == nullptr) {
        return;
    }

    bool have_tag = !EMSdevice::tag_to_string(tag).empty();

    // nested_format is 1 if nested, otherwise 2 for single topics
    bool is_nested;
    if (device_type == EMSdevice::DeviceType::BOILER) {
        is_nested = false; // boiler never uses nested
    } else {
        is_nested = (nested_format_ == 1);
    }

    char device_name[50];
    strlcpy(device_name, EMSdevice::device_type_2_device_name(device_type).c_str(), sizeof(device_name));

    DynamicJsonDocument doc(EMSESP_JSON_SIZE_HA_CONFIG);

    doc["~"] = mqtt_base_;

    // create entity by add the tag if present, seperating with a .
    char new_entity[50];
    if (have_tag) {
        snprintf(new_entity, sizeof(new_entity), "%s.%s", EMSdevice::tag_to_string(tag).c_str(), uuid::read_flash_string(entity).c_str());
    } else {
        snprintf(new_entity, sizeof(new_entity), "%s", uuid::read_flash_string(entity).c_str());
    }

    // build unique identifier which will be used in the topic
    // and replacing all . with _ as not to break HA
    std::string uniq(50, '\0');
    snprintf(&uniq[0], uniq.capacity() + 1, "%s_%s", device_name, new_entity);
    std::replace(uniq.begin(), uniq.end(), '.', '_');
    doc["uniq_id"] = uniq;

    // state topic
    char stat_t[MQTT_TOPIC_MAX_SIZE];
    snprintf(stat_t, sizeof(stat_t), "~/%s", tag_to_topic(device_type, tag).c_str());
    doc["stat_t"] = stat_t;

    // name = <device> <tag> <name>
    char new_name[80];
    if (have_tag) {
        snprintf(new_name, sizeof(new_name), "%s %s %s", device_name, EMSdevice::tag_to_string(tag).c_str(), uuid::read_flash_string(name).c_str());
    } else {
        snprintf(new_name, sizeof(new_name), "%s %s", device_name, uuid::read_flash_string(name).c_str());
    }
    new_name[0] = toupper(new_name[0]); // capitalize first letter
    doc["name"] = new_name;

    // value template
    // if its nested mqtt format then use the appended entity name, otherwise take the original
    char val_tpl[50];
    if (is_nested) {
        snprintf(val_tpl, sizeof(val_tpl), "{{value_json.%s}}", new_entity);
    } else {
        snprintf(val_tpl, sizeof(val_tpl), "{{value_json.%s}}", uuid::read_flash_string(entity).c_str());
    }
    doc["val_tpl"] = val_tpl;

    char topic[MQTT_TOPIC_MAX_SIZE];

    // look at the device value type
    if (type == DeviceValueType::BOOL) {
        // binary sensor
        snprintf(topic, sizeof(topic), "binary_sensor/%s/%s/config", mqtt_base_.c_str(), uniq.c_str()); // topic

        // how to render boolean. HA only accepts String values
        char result[10];
        doc[F("payload_on")]  = Helpers::render_boolean(result, true);
        doc[F("payload_off")] = Helpers::render_boolean(result, false);
    } else {
        // normal HA sensor, not a boolean one
        snprintf(topic, sizeof(topic), "sensor/%s/%s/config", mqtt_base_.c_str(), uniq.c_str()); // topic

        bool set_state_class = false;

        // unit of measure and map the HA icon
        if (uom != DeviceValueUOM::NONE) {
            doc["unit_of_meas"] = EMSdevice::uom_to_string(uom);
        }

        switch (uom) {
        case DeviceValueUOM::DEGREES:
            doc["ic"]       = F_(icondegrees);
            set_state_class = true;
            break;
        case DeviceValueUOM::PERCENT:
            doc["ic"]       = F_(iconpercent);
            set_state_class = true;
            break;
        case DeviceValueUOM::SECONDS:
        case DeviceValueUOM::MINUTES:
        case DeviceValueUOM::HOURS:
            doc["ic"] = F_(icontime);
            break;
        case DeviceValueUOM::KB:
            doc["ic"] = F_(iconkb);
            break;
        case DeviceValueUOM::LMIN:
            doc["ic"]       = F_(iconlmin);
            set_state_class = true;
            break;
        case DeviceValueUOM::WH:
        case DeviceValueUOM::KWH:
            doc["ic"]       = F_(iconkwh);
            set_state_class = true;
            break;
        case DeviceValueUOM::UA:
            doc["ic"]       = F_(iconua);
            set_state_class = true;
            break;
        case DeviceValueUOM::BAR:
            doc["ic"]       = F_(iconbar);
            set_state_class = true;
            break;
        case DeviceValueUOM::W:
        case DeviceValueUOM::KW:
            doc["ic"]       = F_(iconkw);
            set_state_class = true;
            break;
        case DeviceValueUOM::DBM:
            doc["ic"] = F_(icondbm);
            break;
        case DeviceValueUOM::NONE:
            if (type == DeviceValueType::INT ||
                type == DeviceValueType::UINT  ||
                type == DeviceValueType::SHORT ||
                type == DeviceValueType::USHORT ||
                type == DeviceValueType::ULONG) {
                doc["ic"] = F_(iconnum);
            }
        default:
            break;
        }

        // see if we need to set the state_class
        if (set_state_class) {
            doc["state_class"] = F("measurement");
        }
    }

    JsonObject dev = doc.createNestedObject("dev");
    JsonArray  ids = dev.createNestedArray("ids");

    // for System commands we'll use the ID EMS-ESP
    if (device_type == EMSdevice::DeviceType::SYSTEM) {
        ids.add("ems-esp");
    } else {
        char ha_device[40];
        snprintf(ha_device, sizeof(ha_device), "ems-esp-%s", device_name);
        ids.add(ha_device);
    }

    publish_ha(topic, doc.as<JsonObject>());
}

// based on the device and tag, create the MQTT topic name (without the basename)
// differs based on whether MQTT nested is enabled
// tag = EMSdevice::DeviceValueTAG
const std::string Mqtt::tag_to_topic(uint8_t device_type, uint8_t tag) {
    // the system device is treated differently. The topic is 'heartbeat' and doesn't follow the usual convention
    if (device_type == EMSdevice::DeviceType::SYSTEM) {
        return EMSdevice::tag_to_mqtt(tag);
    }

    // if there is a tag add it
    if ((EMSdevice::tag_to_mqtt(tag).empty()) || ((nested_format_ == 1) && (device_type != EMSdevice::DeviceType::BOILER))) {
        return EMSdevice::device_type_2_device_name(device_type) + "_data";
    } else {
        return EMSdevice::device_type_2_device_name(device_type) + "_data_" + EMSdevice::tag_to_mqtt(tag);
    }
}


} // namespace emsesp
