
#include <ESP8266WiFi.h>

#define MAX_SRV_CLIENTS 1
#define STACK_PROTECTOR 512 // bytes

class SerialProxy {
public:
    using connectStatusCallback_t = std::function<void(bool)>;

private:
    HardwareSerial& _serial;
    Print& _logger;

    const int _port = 23;

    WiFiServer _server;
    WiFiClient _serverClients[MAX_SRV_CLIENTS];

    connectStatusCallback_t _connectStatusCallback;
    bool lastStatus = false;

public:
    SerialProxy(HardwareSerial& serial, Print& logger, int port = 23)
        : _serial(serial)
        , _logger(logger)
        , _port(port)
        , _server(_port)
    {
    }

    void begin(connectStatusCallback_t callback = nullptr)
    {
        _connectStatusCallback = callback;
        _server.begin();
        _server.setNoDelay(true);
        lastStatus = false;
    }

    void end()
    {
        _server.stop();
    }

    int proxy(char* sbuf, int size)
    {

        if (lastStatus == true) {
            bool flag = false;
            for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
                if (_serverClients[i]) { // still connected
                    flag = true;
                    break;
                }
            }
            if (!flag) {
                lastStatus = false;
                if (_connectStatusCallback) {
                    _connectStatusCallback(false);
                }
            }
        }

        if (_server.hasClient()) {
            int i;
            for (i = 0; i < MAX_SRV_CLIENTS; i++)
                if (!_serverClients[i]) {
                    _serverClients[i] = _server.accept();
                    _logger.print("New client: index ");
                    _logger.print(i);
                    if (lastStatus == false) {
                        lastStatus = true;
                        if (_connectStatusCallback) {
                            _connectStatusCallback(true);
                        }
                    }
                    break;
                }
            if (i >= MAX_SRV_CLIENTS)
                _server.accept().println("busy");
        }

        for (int i = 0; i < MAX_SRV_CLIENTS; i++)
            while (_serverClients[i].available() && _serial.availableForWrite() > 0) {
                _serial.write(_serverClients[i].read());
            }

        int maxToTcp = 0;
        for (int i = 0; i < MAX_SRV_CLIENTS; i++)
            if (_serverClients[i]) {
                int afw = _serverClients[i].availableForWrite();
                if (afw) {
                    if (!maxToTcp) {
                        maxToTcp = afw;
                    } else {
                        maxToTcp = min(maxToTcp, afw);
                    }
                } /* else {
                    _logger.println("one client is congested");
                } */
            }

        int len = min(min(_serial.available(), maxToTcp), size);
        if (len) {
            int serial_got = _serial.readBytes(sbuf, len);
            for (int i = 0; i < MAX_SRV_CLIENTS; i++)
                // if client.availableForWrite() was 0 (congested)
                // and increased since then,
                // ensure write space is sufficient:
                if (_serverClients[i].availableForWrite() >= serial_got) {
                    size_t tcp_sent = _serverClients[i].write(sbuf, serial_got);
                    if (tcp_sent != len) {
                        _logger.printf("len mismatch: available:%zd serial-read:%zd tcp-write:%zd\n", len, serial_got, tcp_sent);
                    }
                }
            return serial_got;
        }
        return 0;
    }

    void setConnectedCallback(connectStatusCallback_t callback)
    {
        _connectStatusCallback = callback;
    }
};