/*
  Asynchronous WebServer library for Espressif MCUs

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef ASYNCWEBSOCKET_H_
#define ASYNCWEBSOCKET_H_

#include <Arduino.h>
#ifdef ESP32
#include <AsyncTCP.h>
#ifndef WS_MAX_QUEUED_MESSAGES
#define WS_MAX_QUEUED_MESSAGES 32
#endif
#else
#include <ESPAsyncTCP.h>
#ifndef WS_MAX_QUEUED_MESSAGES
#define WS_MAX_QUEUED_MESSAGES 8
#endif
#endif
#include <ESPAsyncWebServer.h>

#include "AsyncWebSynchronization.h"

#ifdef ESP8266
#include <Hash.h>
#ifdef CRYPTO_HASH_h // include Hash.h from espressif framework if the first include was from the crypto library
#include <../src/Hash.h>
#endif
#endif

#ifdef ESP32
#define DEFAULT_MAX_WS_CLIENTS 8
#else
#define DEFAULT_MAX_WS_CLIENTS 4
#endif

class AsyncWebSocket;
class AsyncWebSocketResponse;
class AsyncWebSocketClient;
class AsyncWebSocketControl;

typedef struct {
    /** Message type as defined by enum AwsFrameType.
     * Note: Applications will only see WS_TEXT and WS_BINARY.
     * All other types are handled by the library. */
    uint8_t message_opcode;
    /** Frame number of a fragmented message. */
    uint32_t num;
    /** Is this the last frame in a fragmented message ?*/
    uint8_t final;
    /** Is this frame masked? */
    uint8_t masked;
    /** Message type as defined by enum AwsFrameType.
     * This value is the same as message_opcode for non-fragmented
     * messages, but may also be WS_CONTINUATION in a fragmented message. */
    uint8_t opcode;
    /** Length of the current frame.
     * This equals the total length of the message if num == 0 && final == true */
    uint64_t len;
    /** Mask key */
    uint8_t mask[4];
    /** Offset of the data inside the current frame. */
    uint64_t index;
} AwsFrameInfo;

typedef enum { WS_DISCONNECTED, WS_CONNECTED, WS_DISCONNECTING } AwsClientStatus;
typedef enum { WS_CONTINUATION, WS_TEXT, WS_BINARY, WS_DISCONNECT = 0x08, WS_PING, WS_PONG } AwsFrameType;
typedef enum { WS_MSG_SENDING, WS_MSG_SENT, WS_MSG_ERROR } AwsMessageStatus;
typedef enum { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_PONG, WS_EVT_ERROR, WS_EVT_DATA } AwsEventType;

class AsyncWebSocketMessageBuffer {
  private:
    uint8_t * _data;
    size_t _len;
    bool _lock;
    uint32_t _count;

  public:
    AsyncWebSocketMessageBuffer();
    AsyncWebSocketMessageBuffer(size_t size);
    AsyncWebSocketMessageBuffer(const uint8_t *data, size_t size); // supports PROGMEM
    AsyncWebSocketMessageBuffer(const AsyncWebSocketMessageBuffer &);
    AsyncWebSocketMessageBuffer(AsyncWebSocketMessageBuffer &&);
    ~AsyncWebSocketMessageBuffer();
    void operator ++(int i) { (void)i; _count++; }
    void operator --(int i) { (void)i; if (_count > 0) { _count--; } ;  }
    bool reserve(size_t size);
    void lock() { _lock = true; }
    void unlock() { _lock = false; }
    uint8_t *get() { return _data; }
    size_t length() const { return _len; }
    void setLength(size_t len) { _len = len; }
    uint32_t count() const { return _count; }
    bool canDelete() const { return (!_count && !_lock); }

    friend AsyncWebSocket;

};

class AsyncWebSocketMessage {
  protected:
    uint8_t _opcode;
    bool _mask;
    AwsMessageStatus _status;
  public:
    AsyncWebSocketMessage():_opcode(WS_TEXT),_mask(false),_status(WS_MSG_ERROR){}
    virtual ~AsyncWebSocketMessage(){}
    virtual void ack(size_t len __attribute__((unused)), uint32_t time __attribute__((unused))){}
    virtual size_t send(AsyncClient *client __attribute__((unused))){ return 0; }
    virtual bool finished(){ return _status != WS_MSG_SENDING; }
    virtual bool betweenFrames() const { return false; }
};

class AsyncWebSocketBasicMessage: public AsyncWebSocketMessage {
  private:
    size_t _len;
    size_t _sent;
    size_t _ack;
    size_t _acked;
    uint8_t * _data;
public:
    AsyncWebSocketBasicMessage(const char *data, size_t len, uint8_t opcode=WS_TEXT, bool mask=false); // supports PROGMEM
    AsyncWebSocketBasicMessage(uint8_t opcode=WS_TEXT, bool mask=false);
    virtual ~AsyncWebSocketBasicMessage() override;
    virtual bool betweenFrames() const override { return _acked == _ack; }
    virtual void ack(size_t len, uint32_t time) override ;
    virtual size_t send(AsyncClient *client) override ;
};

class AsyncWebSocketMultiMessage: public AsyncWebSocketMessage {
  private:
    uint8_t *_data;
    size_t _len;
    size_t _sent;
    size_t _ack;
    size_t _acked;
    AsyncWebSocketMessageBuffer * _WSbuffer;
public:
    AsyncWebSocketMultiMessage(AsyncWebSocketMessageBuffer *buffer, uint8_t opcode=WS_TEXT, bool mask=false);
    virtual ~AsyncWebSocketMultiMessage() override;
    virtual bool betweenFrames() const override { return _acked == _ack; }
    virtual void ack(size_t len, uint32_t time) override ;
    virtual size_t send(AsyncClient *client) override ;
};

class AsyncWebSocketClient {
  private:
    AsyncClient *_client;
    AsyncWebSocket *_server;
    uint32_t _clientId;
    AwsClientStatus _status;

    LinkedList<AsyncWebSocketControl *> _controlQueue;
    LinkedList<AsyncWebSocketMessage *> _messageQueue;

    uint8_t _pstate;
    AwsFrameInfo _pinfo;

    uint32_t _lastMessageTime;
    uint32_t _keepAlivePeriod;

    void _queueMessage(AsyncWebSocketMessage *dataMessage);
    void _queueControl(AsyncWebSocketControl *controlMessage);
    void _runQueue();

  public:
    void *_tempObject;

    AsyncWebSocketClient(AsyncWebServerRequest *request, AsyncWebSocket *server);
    ~AsyncWebSocketClient();

    //client id increments for the given server
    uint32_t id(){ return _clientId; }
    AwsClientStatus status(){ return _status; }
    AsyncClient* client(){ return _client; }
    AsyncWebSocket *server(){ return _server; }
    AwsFrameInfo const &pinfo() const { return _pinfo; }

    IPAddress remoteIP();
    uint16_t  remotePort();

    //control frames
    void close(uint16_t code = 0, const char * message = nullptr); // supports PROGMEM
    void ping(const uint8_t *data = nullptr, size_t len = 0); // supports PROGMEM
    void ping(const char *data = nullptr, size_t len = 0) { // supports PROGMEM
        ping(reinterpret_cast<const uint8_t *>(data), len);
    }

    //set auto-ping period in seconds. disabled if zero (default)
    void keepAlivePeriod(uint16_t seconds){
      _keepAlivePeriod = seconds * 1000;
    }
    uint16_t keepAlivePeriod(){
      return (uint16_t)(_keepAlivePeriod / 1000);
    }

    //data packets
    void message(AsyncWebSocketMessage *message){ _queueMessage(message); }
    bool queueIsFull();

    size_t printf_P(PGM_P formatP, ...)  __attribute__ ((format (printf, 2, 3)));
    size_t printf(const char *format, ...)  __attribute__ ((format (printf, 2, 3))) { // supports PROGMEM
      va_list arg;
      va_start(arg, format);
      size_t len = printf_P(format, arg);
      va_end(arg);
      return len;
    }

    void text(const char * message, size_t len) { // supports PROGMEM
      _queueMessage(new AsyncWebSocketBasicMessage(message, len));
    }
    void text(const char *message) { // supports PROGMEM
      text(message, strlen_P(message));
    }
    void text(const uint8_t * message, size_t len) { // supports PROGMEM
      text((const char *)message, len);
    }
    void text(char * message) {
      text(message, strlen(message));
    }
    void text(const String &message) {
      text(message.c_str(), message.length());
    }
    void text(const __FlashStringHelper *data) {
      text(reinterpret_cast<const char *>(data));
    }
    void text(AsyncWebSocketMessageBuffer *buffer) {
      _queueMessage(new AsyncWebSocketMultiMessage(buffer));
    }

    void binary(const char *message, size_t len) { // supports PROGMEM
      _queueMessage(new AsyncWebSocketBasicMessage(message, len, WS_BINARY));
    }
    void binary(const char *message) { // supports PROGMEM
      binary(message, strlen_P(message));
    }
    void binary(const uint8_t *message, size_t len) { // supports PROGMEM
      binary((const char *)message, len);
    }
    void binary(char *message) {
      binary(message, strlen(message));
    }
    void binary(const String &message) {
      binary(message.c_str(), message.length());
    }
    void binary(const __FlashStringHelper *data, size_t len) {
      binary(reinterpret_cast<PGM_P>(data), len);
    }
    void binary(AsyncWebSocketMessageBuffer *buffer) {
      _queueMessage(new AsyncWebSocketMultiMessage(buffer, WS_BINARY));
    }

    bool canSend() { return _messageQueue.length() < WS_MAX_QUEUED_MESSAGES; }

    //system callbacks (do not call)
    void _onAck(size_t len, uint32_t time);
    void _onError(int8_t) {}
    void _onPoll();
    void _onTimeout(uint32_t time) {
      _client->close(true);
    }
    void _onDisconnect();
    void _onData(void *pbuf, size_t plen);
};

using AwsEventHandler = std::function<void(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)>;

//WebServer Handler implementation that plays the role of a socket server
class AsyncWebSocket: public AsyncWebHandler {
  public:
    using AsyncWebSocketClientLinkedList = LinkedList<AsyncWebSocketClient *>;

  private:
    String _url;
    AsyncWebSocketClientLinkedList _clients;
    uint32_t _cNextId;
    AwsEventHandler _eventHandler;
    bool _enabled;
    AsyncWebLock _lock;

  public:
    AsyncWebSocket(const String& url);
    ~AsyncWebSocket() {}
    const char * url() const { return _url.c_str(); }
    void enable(bool e){ _enabled = e; }
    bool enabled() const { return _enabled; }
    bool availableForWriteAll();
    bool availableForWrite(uint32_t id);

    size_t count() const;
    AsyncWebSocketClient * client(uint32_t id);
    bool hasClient(uint32_t id){ return client(id) != NULL; }

    void close(uint32_t id, uint16_t code=0, const char * message=NULL);  // supports PROGMEM
    void closeAll(uint16_t code=0, const char * message=NULL); // supports PROGMEM
    void cleanupClients(uint16_t maxClients = DEFAULT_MAX_WS_CLIENTS);

    void ping(uint32_t id, const uint8_t *data=NULL, size_t len=0); // supports PROGMEM
    void pingAll(const uint8_t *data=NULL, size_t len=0); // supports PROGMEM

    void text(uint32_t id, const char *message, size_t len); // supports PROGMEM
    void text(uint32_t id, const char *message) { // supports PROGMEM
      text(id, message, strlen_P(message));
    }
    void text(uint32_t id, const uint8_t *message, size_t len) { // supports PROGMEM
      text(id, (const char *)message, len);
    }
    void text(uint32_t id, char *message) {
      text(id, message, strlen(message));
    }
    void text(uint32_t id, const String &message) {
      text(id, message.c_str(), message.length());
    }
    void text(uint32_t id, const __FlashStringHelper *message);

    void textAll(const char *message, size_t len) { // supports PROGMEM
      textAll(makeBuffer((const uint8_t *)message, len));
    }
    void textAll(const char *message) { // supports PROGMEM
      return textAll(message, strlen_P(message));
    }
    void textAll(const uint8_t *message, size_t len) { // supports PROGMEM
      return textAll((const char *)message, len);
    }
    void textAll(char * message){
      textAll(message, strlen(message));
    }
    void textAll(const String &message) {
      textAll(message.c_str(), message.length());
    }
    void textAll(const __FlashStringHelper *message);
    void textAll(AsyncWebSocketMessageBuffer * buffer);


    void binary(uint32_t id, const char * message, size_t len); // supports PROGMEM
    void binary(uint32_t id, const char * message) { // supports PROGMEM
      binary(id, message, strlen_P(message));
    }
    void binary(uint32_t id, const uint8_t *message, size_t len) { // supports PROGMEM
      binary(id, (const char *)message, len);
    }
    void binary(uint32_t id, char * message) {
      binary(id, message, strlen(message));
    }
    void binary(uint32_t id, const String &message) {
      binary(id, message.c_str(), message.length());
    }
    void binary(uint32_t id, const __FlashStringHelper *message, size_t len);

    void binaryAll(const char *message, size_t len) { // supports PROGMEM
      binaryAll(makeBuffer((const uint8_t *)message, len));
    }
    void binaryAll(const char * message) {
      binaryAll(message, strlen_P(message));
    }
    void binaryAll(const uint8_t * message, size_t len) { // supports PROGMEM
      binaryAll((const char *)message, len);
    }
    void binaryAll(char * message) {
      binaryAll(message, strlen(message));
    }
    void binaryAll(const String &message) {
      binaryAll(message.c_str(), message.length());
    }
    void binaryAll(const __FlashStringHelper *message, size_t len);
    void binaryAll(AsyncWebSocketMessageBuffer * buffer);

    void message(uint32_t id, AsyncWebSocketMessage *message);
    void messageAll(AsyncWebSocketMultiMessage *message);

    size_t printf_P(uint32_t id, PGM_P formatP, ...)  __attribute__ ((format (printf, 3, 4)));
    size_t printf(uint32_t id, const char *format, ...)  __attribute__ ((format (printf, 3, 4))) { // supports PROGMEM
      va_list arg;
      va_start(arg, format);
      size_t len = printf_P(id, format, arg);
      va_end(arg);
      return len;
    }

    size_t printfAll_P(PGM_P formatP, ...)  __attribute__ ((format (printf, 2, 3)));
    size_t printfAll(const char *format, ...)  __attribute__ ((format (printf, 2, 3))) { // supports PROGMEM
      va_list arg;
      va_start(arg, format);
      size_t len = printfAll_P(format, arg);
      va_end(arg);
      return len;
    }

    //event listener
    void onEvent(AwsEventHandler handler){
      _eventHandler = handler;
    }

    //system callbacks (do not call)
    uint32_t _getNextId() {
        return _cNextId++;
    }
    void _addClient(AsyncWebSocketClient * client) {
          _clients.add(client);
    }
    void _handleDisconnect(AsyncWebSocketClient * client);
    void _handleEvent(AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len);
    virtual bool canHandle(AsyncWebServerRequest *request) override final;
    virtual void handleRequest(AsyncWebServerRequest *request) override final;


    //  messagebuffer functions/objects.
    AsyncWebSocketMessageBuffer *makeBuffer(size_t size = 0);
    AsyncWebSocketMessageBuffer *makeBuffer(const uint8_t *data, size_t size); // supports PROGMEM
    LinkedList<AsyncWebSocketMessageBuffer *> _buffers;
    void _cleanBuffers();

    AsyncWebSocketClientLinkedList getClients() const;
};

//WebServer response to authenticate the socket and detach the tcp client from the web server request
class AsyncWebSocketResponse: public AsyncWebServerResponse {
  private:
    String _content;
    AsyncWebSocket *_server;
  public:
    AsyncWebSocketResponse(const String& key, AsyncWebSocket *server);
    void _respond(AsyncWebServerRequest *request);
    size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t time);
    bool _sourceValid() const { return true; }
};


#endif /* ASYNCWEBSOCKET_H_ */
