#ifndef Espressif_MQTT_Client_h
#define Espressif_MQTT_Client_h

// Local include.
#include "Configuration.h"

#if THINGSBOARD_USE_ESP_MQTT

// Local includes.
#include "IMQTT_Client.h"

// Library includes.
#include <mqtt_client.h>
#include <esp_crt_bundle.h>

// The error integer -1 means a general failure while handling the mqtt client,
// where as -2 means that the outbox is filled and the message can therefore not be sent.
// Therefore we have to check if the value is smaller or equal to the MQTT_FAILURE_MESSAGE_ID,
// to ensure other errors are indentified as well
constexpr int MQTT_FAILURE_MESSAGE_ID = -1;
constexpr char MQTT_DATA_EXCEEDS_BUFFER[] = "Received amount of data (%u) is bigger than current buffer size (%u), increase accordingly";
#if THINGSBOARD_ENABLE_DEBUG
constexpr char RECEIVED_MQTT_EVENT[] = "Handling received mqtt event: (%s)";
constexpr char UPDATING_CONFIGURATION[] = "Updated configuration after inital connection with response: (%s)";
constexpr char OVERRIDING_DEFAULT_CRT_BUNDLE[] = "Overriding default CRT bundle with response: (%s)";
#endif // THINGSBOARD_ENABLE_DEBUG


/// @brief MQTT Client interface implementation that uses the offical ESP MQTT client from Espressif (https://github.com/espressif/esp-mqtt),
/// under the hood to establish and communicate over a MQTT connection. This component works with both Espressif IDF v4.X and v5.X, meaning it is version idependent, this is the case
/// because depending on the used version the implementation automatically adjusts to still initalize the client correctly.
/// Documentation about the specific use and caviates of the ESP MQTT client can be found here https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html
/// @tparam Logger Implementation that should be used to print error messages generated by internal processes and additional debugging messages if THINGSBOARD_ENABLE_DEBUG is set, default = DefaultLogger
template <typename Logger = DefaultLogger>
class Espressif_MQTT_Client : public IMQTT_Client {
  public:
    /// @brief Constructs a IMQTT_Client implementation which creates and empty esp_mqtt_client_config_t, which then has to be configured with the other methods in the class
    Espressif_MQTT_Client()
      : m_received_data_callback()
      , m_connected_callback()
      , m_connected(false)
      , m_enqueue_messages(false)
      , m_mqtt_configuration()
      , m_mqtt_client(nullptr)
    {
        // Nothing to do
    }

    /// @brief Destructor
    ~Espressif_MQTT_Client() {
        (void)esp_mqtt_client_destroy(m_mqtt_client);
    }

    /// @brief Configures the server certificate, which allows to connect to the MQTT broker over a secure TLS / SSL conenction instead of the default unencrypted channel.
    /// Has to be called before initally calling connect() on the client to ensure the certificate is set before the connection is established, if that is not done the connection will not be encrypted.
    /// Encryption is recommended if relevant data is sent or if the client receives and handles Remote Procedure Calls or Shared Attribute Update Callbacks from the server,
    /// because using an unencrpyted connection, will allow 3rd parties to listen to the communication and impersonate the server sending payloads which might influence the device in unexpected ways.
    /// However if Over the Air udpates are enabled secure communication should definetly be enabled, because if that is not done a 3rd party might impersonate the server sending a malicious payload,
    /// which is then flashed onto the device instead of the real firmware. Which depeding on the payload might even be able to destroy the device or make it otherwise unusable.
    /// See https://stackoverflow.blog/2020/12/14/security-considerations-for-ota-software-updates-for-iot-gateway-devices/ for more information on the aforementioned security risk
    /// @param server_certificate_pem Null-terminated string containg the root certificate in PEM format, of the server we are attempting to send to and receive MQTT data from
    /// @return Whether changing the internal server certificate was successful or not, ensure to disconnect and reconnect to actually apply the change
    bool set_server_certificate(char const * server_certificate_pem) {
      // Because PEM format is expected for the server certificate we do not need to set the certificate_len,
      // as the PEM format expects a null-terminated string
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.cert_pem = server_certificate_pem;
#else
        m_mqtt_configuration.broker.verification.certificate = server_certificate_pem;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Configures a bundle of root certificates for verification and enables the MQTT broker to use certificate bundles, which allows for connecting to the MQTT broker over a secure TLS / SSL connection with any website associated with the root certificates in the bundle.
    /// Has to be called atleast once for the feature to be activated because an internal function pointer in the underlying client has to be configured..
    /// Instead of providing a single server certificate, this then allows providing a collection of root certificates. If nullptr is passed as the x509_bundle it will simply use the default root certificate bundle instead, which contains the full Mozilla root certificate bundle (130 certificates).
    /// It is recommended to use menuconfig to configure the default certificate bundle, which will then read those certificates from the embedded application. Use CONFIG_MBEDTLS_DEFAULT_CERTIFICATE_BUNDLE to filter which certificates to include from the default bundle,
    /// or use CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH to specify the path of additional certificates which should be embedded into the bundle
    /// See https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_crt_bundle.html for more information on ESP x509 Certificate Bundles
    /// @param x509_bundle Optional Pointer to single or beginning of array of root certificates in DER format, of the servers we are attempting to send to and receive MQTT data from, overrides the default certificate bundle previously configured with menuconfig.
    /// The bundle to be sorted by the certificate subject, becuase the underlying implementation uses binary search to find used certificates. Simply pass nullptr if the default root certificate bundle should be used instead, default = nullptr
    /// @param bundle_size Optional total size of bundle with all certificates in bytes, only used if the x509 bundle is a valid pointer and when not using the Arduino framework, default = 0
    /// @return Whether changing the bundle attach function was successful or not, ensure to disconnect and reconnect to actually apply the change
    bool set_server_crt_bundle(uint8_t const * x509_bundle = nullptr, size_t const & bundle_size = 0U) {
        esp_err_t (*crt_bundle_attach)(void * conf) = nullptr;
#ifdef ARDUINO
        crt_bundle_attach = arduino_esp_crt_bundle_attach;
#else
        crt_bundle_attach = esp_crt_bundle_attach;
#endif // ARDUINO

#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.crt_bundle_attach = crt_bundle_attach;
#else
        m_mqtt_configuration.broker.verification.crt_bundle_attach = crt_bundle_attach;
#endif // ESP_IDF_VERSION_MAJOR < 5

        if (x509_bundle != nullptr) {
            esp_err_t error = ESP_OK;
#ifdef ARDUINO
            arduino_esp_crt_bundle_set(x509_bundle);
#else
#if (ESP_IDF_VERSION_MAJOR == 4 && ESP_IDF_VERSION_MINOR <= 4 && ESP_IDF_VERSION_PATCH < 2) || ESP_IDF_VERSION_MAJOR < 4
            error = esp_crt_bundle_set(x509_bundle);
#else
            error = esp_crt_bundle_set(x509_bundle, bundle_size);
#endif // (ESP_IDF_VERSION_MAJOR == 4 && ESP_IDF_VERSION_MINOR <= 4 ESP_IDF_VERSION_MINO < 2) || ESP_IDF_VERSION_MAJOR < 4
#endif // ARDUINO
#if THINGSBOARD_ENABLE_DEBUG
            Logger::printfln(OVERRIDING_DEFAULT_CRT_BUNDLE, esp_err_to_name(error));
#endif // THINGSBOARD_ENABLE_DEBUG
        }
        return update_configuration();
    }

    /// @brief Sets the keep alive timeout in seconds, if the value is 0 then the default of 120 seconds is used instead to disable the keep alive mechanism use set_disable_keep_alive() instead.
    /// The default timeout value ThingsBoard expectes to receive any message including a keep alive to not show the device as inactive can be found here https://thingsboard.io/fig/#mqtt-server-parameters
    /// under the transport.sessions.inactivity_timeout section and is 300 seconds. Meaning a value bigger than 300 seconds with the default config defeats the purpose of the keep alive alltogetherdocs/user-guide/install/con
    /// @param keep_alive_timeout_seconds Timeout until we send another PINGREQ control packet to the broker to establish that we are still connected
    /// @return Whether changing the internal keep alive timeout was successful or not
    bool set_keep_alive_timeout(uint16_t keep_alive_timeout_seconds) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.keepalive = keep_alive_timeout_seconds;
#else
        m_mqtt_configuration.session.keepalive = keep_alive_timeout_seconds;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Whether to disable or enable the keep alive mechanism, meaning we do not send any PINGREQ control packets to the broker anymore, meaning if we do not send other packet the device will be marked as inactive.
    /// The default value is false meaning the keep alive mechanism is enabled.
    /// The default timeout value ThingsBoard expectes to receive any message including a keep alive to not show the device as inactive can be found here https://thingsboard.io/docs/user-guide/install/config/#mqtt-server-parameters
    /// under the transport.sessions.inactivity_timeout section and is 300 seconds. Meaning if we disable the keep alive mechanism and do not send another packet atleast every 300 seconds with the default config
    /// then the device will change states between active and inactive depending on when we send the packets
    /// @param disable_keep_alive Whether to enable or disable the internal keep alive mechanism, which sends PINGREQ control packets every keep alive timeout seconds, configurable in set_keep_alive_timeout()
    /// @return Whether enabling or disabling the internal keep alive mechanism was successful or not
    bool set_disable_keep_alive(bool disable_keep_alive) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.disable_keepalive = disable_keep_alive;
#else
        m_mqtt_configuration.session.disable_keepalive = disable_keep_alive;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Wheter to disable or enable that the MQTT client will reconnect to the server automatically if it errors or disconnects. The default is false meaning we will automatically reconnect
    /// @param disable_auto_reconnect Whether to automatically reconnect if the the client errors or disconnects
    /// @return Whether enabling or disabling the internal auto reconnect mechanism was successful or not
    bool set_disable_auto_reconnect(bool disable_auto_reconnect) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.disable_auto_reconnect = disable_auto_reconnect;
#else
        m_mqtt_configuration.network.disable_auto_reconnect = disable_auto_reconnect;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Sets the priority and stack size of the MQTT task running in the background, that is handling the receiving and sending of any outstanding MQTT messages to or from the broker.
    /// The default value for the priority is 5 and can also be changed in the ESP IDF menuconfig, whereas the default value for the stack size is 6144 bytes and can also be changed in the ESP IDF menuconfig
    /// @param priority Task priority with which the MQTT task should run, higher priority means it takes more precedence over other tasks, making it more important
    /// @param stack_size Task stack size meaning how much stack size the MQTT task can allocate before the device crashes with a StackOverflow, might need to be increased
    /// if the user allocates a lot of memory on the stack in a request callback method, because those functions are dispatched from the MQTT event task
    /// @return Whether changing the internal MQTT task configurations were successfull or not
    bool set_mqtt_task_configuration(uint8_t priority, uint16_t stack_size) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.task_prio = priority;
        m_mqtt_configuration.task_stack = stack_size;
#else
        m_mqtt_configuration.task.priority = priority;
        m_mqtt_configuration.task.stack_size = stack_size;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Sets the amount of time in milliseconds that we wait before we automatically reconnect to the MQTT broker if the connection has been lost. The default value is 10 seconds.
    /// Will be ignored if set_disable_auto_reconnect() has been set to true, because we will not reconnect automatically anymore, instead that would have to be done by the user themselves
    /// @param reconnect_timeout_milliseconds Time in milliseconds until we automatically reconnect to the MQTT broker
    /// @return Whether changing the internal reconnect timeout was successfull or not
    bool set_reconnect_timeout(uint16_t reconnect_timeout_milliseconds) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.reconnect_timeout_ms = reconnect_timeout_milliseconds;
#else
        m_mqtt_configuration.network.reconnect_timeout_ms = reconnect_timeout_milliseconds;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Sets the amount of time in millseconds that we wait until we expect a netowrk operation on the MQTT client to have successfully finished. The defalt value is 10 seconds.
    /// If that is not the case then the network operation will be aborted, might be useful to increase for devices that perform other high priority tasks and do not have enough CPU resources,
    /// to send bigger messages to the MQTT broker in time, which can cause disconnects and the sent message being discarded
    /// @param network_timeout_milliseconds Time in milliseconds that we wait until we abort the network operation if it has not completed yet
    /// @return Whether changing the internal network timeout was successfull or not
    bool set_network_timeout(uint16_t network_timeout_milliseconds) {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.network_timeout_ms = network_timeout_milliseconds;
#else
        m_mqtt_configuration.network.timeout_ms = network_timeout_milliseconds;
#endif // ESP_IDF_VERSION_MAJOR < 5
        return update_configuration();
    }

    /// @brief Sets whether to enqueue published messages or not, enqueueing has to save them in the out buffer, meaning the internal buffer size might need to be increased,
    /// but the MQTT client can in exchange send the publish messages once the main MQTT task is running again, instead of blocking in the task that has called the publish method.
    /// This furthermore, allows to use nearly all internal ThingsBoard calls without having to worry about blocking the task the method was called for,
    /// or having to worry about CPU overhead.
    /// If enqueueing the messages to be published fails once this options has been enabled, then the internal buffer size might need to be increased. Ensure to call set_buffer_size() with a bigger value
    /// and check if enqueueing the messages to be published works successfully again
    /// @param enqueue_messages Whether to enqueue published messages or not, where setting the value to true means that the messages are enqueued and therefor non blocking on the called from task
    void set_enqueue_messages(bool enqueue_messages) {
        m_enqueue_messages = enqueue_messages;
    }

    void set_data_callback(Callback<void, char *, uint8_t *, unsigned int>::function callback) override {
        m_received_data_callback.Set_Callback(callback);
    }

    void set_connect_callback(Callback<void>::function callback) override {
        m_connected_callback.Set_Callback(callback);
    }

    bool set_buffer_size(uint16_t receive_buffer_size, uint16_t send_buffer_size) override {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.buffer_size = receive_buffer_size;
        m_mqtt_configuration.out_buffer_size = send_buffer_size;
#else
        m_mqtt_configuration.buffer.size = receive_buffer_size;
        m_mqtt_configuration.buffer.out_size = send_buffer_size;
#endif // ESP_IDF_VERSION_MAJOR < 5

        // Calls esp_mqtt_set_config(), which should adjust the underlying mqtt client to the changed values.
        // Which it does but not for the buffer_size, this results in the buffer size only being able to be changed when initally creating the mqtt client.
        // If the mqtt client is reinitalized this causes disconnected and reconnects tough and the connection becomes unstable.
        // Therefore this workaround can also not be used. Instead we expect the esp_mqtt_set_config(), to do what the name implies and therefore still call it
        // and created an issue revolving around the aformentioned problem so it might get fixed in future version of the esp_mqtt client.
        // See https://github.com/espressif/esp-mqtt/issues/267 for more information on the issue 
        return update_configuration();
    }

    uint16_t get_receive_buffer_size() override {
#if ESP_IDF_VERSION_MAJOR < 5
        return m_mqtt_configuration.buffer_size;
#else
        return m_mqtt_configuration.buffer.size;
#endif // ESP_IDF_VERSION_MAJOR < 5
    }

    uint16_t get_send_buffer_size() override {
#if ESP_IDF_VERSION_MAJOR < 5
        return m_mqtt_configuration.out_buffer_size;
#else
        return m_mqtt_configuration.buffer.out_size;
#endif // ESP_IDF_VERSION_MAJOR < 5
    }

    void set_server(char const * domain, uint16_t port) override {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.host = domain;
        m_mqtt_configuration.port = port;
        // Decide transport depending on if a certificate was passed, because the set_server() method is called in the connect method meaning if the certificate has not been set yet,
        // it is to late as we attempt to establish the connection in the connect() method which is called directly after this one.
        bool const transport_over_sll = m_mqtt_configuration.cert_pem != nullptr || m_mqtt_configuration.crt_bundle_attach != nullptr;
#else
        m_mqtt_configuration.broker.address.hostname = domain;
        m_mqtt_configuration.broker.address.port = port;
        // Decide transport depending on if a certificate was passed, because the set_server() method is called in the connect method meaning if the certificate has not been set yet,
        // it is to late as we attempt to establish the connection in the connect() method which is called directly after this one.
        bool const transport_over_sll = m_mqtt_configuration.broker.verification.certificate != nullptr || m_mqtt_configuration.broker.verification.crt_bundle_attach != nullptr;
#endif // ESP_IDF_VERSION_MAJOR < 5

        esp_mqtt_transport_t const transport = (transport_over_sll ? esp_mqtt_transport_t::MQTT_TRANSPORT_OVER_SSL : esp_mqtt_transport_t::MQTT_TRANSPORT_OVER_TCP);
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.transport = transport;
#else
        m_mqtt_configuration.broker.address.transport = transport;
#endif // ESP_IDF_VERSION_MAJOR < 5
    }

    bool connect(char const * client_id, char const * user_name, char const * password) override {
#if ESP_IDF_VERSION_MAJOR < 5
        m_mqtt_configuration.client_id = client_id;
        m_mqtt_configuration.username = user_name;
        m_mqtt_configuration.password = password;
#else
        m_mqtt_configuration.credentials.client_id = client_id;
        m_mqtt_configuration.credentials.username = user_name;
        m_mqtt_configuration.credentials.authentication.password = password;
#endif // ESP_IDF_VERSION_MAJOR < 5
        // Update configuration is called to ensure that if we connected previously and call connect again with other credentials,
        // then we also update the client_id, username and password we connect with. Especially important for the provisioning workflow to work correctly
        update_configuration();

        // Check wheter the client has been initalzed before already, it it has we do not want to reinitalize,
        // but simply force reconnection with the client because it has lost that connection
        if (m_mqtt_client != nullptr) {
            const esp_err_t error = esp_mqtt_client_reconnect(m_mqtt_client);
            return error == ESP_OK;
        }

        // The client is first initalized once the connect has actually been called, this is done because the passed setting are required for the client inizialitation structure,
        // additionally before we attempt to connect with the client we have to ensure it is configued by then.
        m_mqtt_client = esp_mqtt_client_init(&m_mqtt_configuration);
        esp_err_t error = esp_mqtt_client_register_event(m_mqtt_client, esp_mqtt_event_id_t::MQTT_EVENT_ANY, Espressif_MQTT_Client::static_mqtt_event_handler, this);

        if (error != ESP_OK) {
            return false;
        }
        error = esp_mqtt_client_start(m_mqtt_client);
        return error == ESP_OK;
    }

    void disconnect() override {
        (void)esp_mqtt_client_disconnect(m_mqtt_client);
    }

    bool loop() override {
        // Unused because the esp mqtt client uses its own task to handle receiving and sending of data, therefore we do not need to do anything in the loop method.
        // Because the loop method is meant for clients that do not have their own process method but instead rely on the upper level code calling a loop method to provide processsing time.
        return m_connected;
    }

    bool publish(char const * topic, uint8_t const * payload, size_t const & length) override {
        int message_id = MQTT_FAILURE_MESSAGE_ID;

        if (m_enqueue_messages) {
            message_id = esp_mqtt_client_enqueue(m_mqtt_client, topic, reinterpret_cast<const char*>(payload), length, 0U, 0U, true);
            return message_id > MQTT_FAILURE_MESSAGE_ID;
        }

        // The blocking version esp_mqtt_client_publish() it is sent directly from the users task context.
        // This way is used to send messages to the cloud, because like that no internal buffer has to be used to store the message until it should be sent,
        // because all messages are sent with QoS level 0. If this is not wanted esp_mqtt_client_enqueue() could be used with store = true,
        // to ensure the sending is done in the mqtt event context instead of the users task context.
        // Allows to use the publish method without having to worry about any CPU overhead, so it can even be used in callbacks or high priority tasks, without starving other tasks,
        // but compared to the other method esp_mqtt_client_enqueue() requires to save the message in the outbox, which increases the memory requirements for the internal buffer size
        message_id = esp_mqtt_client_publish(m_mqtt_client, topic, reinterpret_cast<const char*>(payload), length, 0U, 0U);
        return message_id > MQTT_FAILURE_MESSAGE_ID;
    }

    bool subscribe(char const * topic) override {
        // The esp_mqtt_client_subscribe method does not return false, if we send a subscribe request while not being connected to a broker,
        // so we have to check for that case to ensure the end user is informed that their subscribe request could not be sent and has been ignored.
        if (!connected()) {
            return false;
        }
        int const message_id = esp_mqtt_client_subscribe(m_mqtt_client, topic, 0U);
        return message_id > MQTT_FAILURE_MESSAGE_ID;
    }

    bool unsubscribe(char const * topic) override {
        // The esp_mqtt_client_unsubscribe method does not return false, if we send a unsubscribe request while not being connected to a broker,
        // so we have to check for that case to ensure the end user is informed that their unsubscribe request could not be sent and has been ignored.
        if (!connected()) {
            return false;
        }
        int const message_id = esp_mqtt_client_unsubscribe(m_mqtt_client, topic);
        return message_id > MQTT_FAILURE_MESSAGE_ID;
    }

    bool connected() override {
        return m_connected;
    }

private:
    /// @brief Is internally used to allow changes to the underlying configuration of the esp_mqtt_client_handle_t after it has connected,
    /// to for example increase the buffer size or increase the timeouts or stack size, allows to change the underlying client configuration,
    /// without the need to completly disconnect and reconnect the client
    /// @return Whether updating the configuration with the changed settings was successfull or not
    bool update_configuration() {
        // Check if the client has been initalized, because if it did not the value should still be nullptr
        // and updating the config makes no sense because the changed settings will be applied anyway when the client is first intialized
        if (m_mqtt_client == nullptr) {
            return true;
        }

        esp_err_t const error = esp_mqtt_set_config(m_mqtt_client, &m_mqtt_configuration);
#if THINGSBOARD_ENABLE_DEBUG
        Logger::printfln(UPDATING_CONFIGURATION, esp_err_to_name(error));
#endif // THINGSBOARD_ENABLE_DEBUG
        return error == ESP_OK;
    }

#if THINGSBOARD_ENABLE_DEBUG
    const char * esp_event_id_to_name(const esp_mqtt_event_id_t& event_id) const {
        switch (event_id) {
            case esp_mqtt_event_id_t::MQTT_EVENT_CONNECTED:
                return "MQTT_EVENT_CONNECTED";
            case esp_mqtt_event_id_t::MQTT_EVENT_DISCONNECTED:
                return "MQTT_EVENT_DISCONNECTED";
            case esp_mqtt_event_id_t::MQTT_EVENT_SUBSCRIBED:
                return "MQTT_EVENT_SUBSCRIBED";
            case esp_mqtt_event_id_t::MQTT_EVENT_UNSUBSCRIBED:
                return "MQTT_EVENT_UNSUBSCRIBED";
            case esp_mqtt_event_id_t::MQTT_EVENT_PUBLISHED:
                return "MQTT_EVENT_PUBLISHED";
            case esp_mqtt_event_id_t::MQTT_EVENT_DATA:
                return "MQTT_EVENT_DATA";
            case esp_mqtt_event_id_t::MQTT_EVENT_ERROR:
                return "MQTT_EVENT_ERROR";
            case esp_mqtt_event_id_t::MQTT_EVENT_BEFORE_CONNECT:
                return "MQTT_EVENT_BEFORE_CONNECT";
            case esp_mqtt_event_id_t::MQTT_EVENT_DELETED:
                return "MQTT_EVENT_DELETED";
            case esp_mqtt_event_id_t::MQTT_EVENT_ANY:
                return "MQTT_EVENT_ANY";
            default:
                return "UNKNOWN";
        }
    }
#endif // THINGSBOARD_ENABLE_DEBUG

    /// @brief Event handler registered to receive MQTT events. Is called by the MQTT client event loop, whenever a new event occurs
    /// @param base Event base for the handler
    /// @param event_id The id for the received event
    /// @param event_data The data for the event, esp_mqtt_event_handle_t
    void mqtt_event_handler(esp_event_base_t base, esp_mqtt_event_id_t const & event_id, void * event_data) {
        esp_mqtt_event_handle_t const event = static_cast<esp_mqtt_event_handle_t>(event_data);

#if THINGSBOARD_ENABLE_DEBUG
        Logger::printfln(RECEIVED_MQTT_EVENT, esp_event_id_to_name(event_id));
#endif // THINGSBOARD_ENABLE_DEBUG
        switch (event_id) {
            case esp_mqtt_event_id_t::MQTT_EVENT_CONNECTED:
                m_connected = true;
                m_connected_callback.Call_Callback();
                break;
            case esp_mqtt_event_id_t::MQTT_EVENT_DISCONNECTED:
                m_connected = false;
                break;
            case esp_mqtt_event_id_t::MQTT_EVENT_DATA: {
                // Check wheter the given message has not bee received completly, but instead would be received in multiple chunks,
                // if it were we discard the message because receiving a message over multiple chunks is currently not supported
                if (event->data_len != event->total_data_len) {
                    Logger::printfln(MQTT_DATA_EXCEEDS_BUFFER, event->total_data_len, get_receive_buffer_size());
                    break;
                }
                // Topic is not null terminated, to fix this issue we copy the topic string.
                // This overhead is acceptable, because we nearly always copy only a few bytes (around 20), meaning the overhead is insignificant.
                char topic[event->topic_len + 1] = {};
                strncpy(topic, event->topic, event->topic_len);
                m_received_data_callback.Call_Callback(topic, reinterpret_cast<uint8_t*>(event->data), event->data_len);
                break;
            }
            default:
                // Nothing to do
                break;
        }
    }

    static void static_mqtt_event_handler(void * handler_args, esp_event_base_t base, int32_t event_id, void * event_data) {
        if (handler_args == nullptr) {
            return;
        }
        auto instance = static_cast<Espressif_MQTT_Client *>(handler_args);
        instance->mqtt_event_handler(base, static_cast<esp_mqtt_event_id_t>(event_id), event_data);
    }

    Callback<void, char *, uint8_t *, unsigned int> m_received_data_callback = {}; // Callback that will be called as soon as the mqtt client receives any data
    Callback<void>                                  m_connected_callback = {};     // Callback that will be called as soon as the mqtt client has connected
    bool                                            m_connected = {};              // Whether the client has received the connected or disconnected event
    bool                                            m_enqueue_messages = {};       // Whether we enqueue messages making nearly all ThingsBoard calls non blocking or wheter we publish instead
    esp_mqtt_client_config_t                        m_mqtt_configuration = {};     // Configuration of the underlying mqtt client, saved as a private variable to allow changes after inital configuration with the same options for all non changed settings
    esp_mqtt_client_handle_t                        m_mqtt_client = {};            // Handle to the underlying mqtt client, used to establish the communication
};

#endif // THINGSBOARD_USE_ESP_MQTT

#endif // Espressif_MQTT_Client_h
