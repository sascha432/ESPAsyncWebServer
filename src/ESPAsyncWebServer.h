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
#ifndef _ESPAsyncWebServer_H_
#define _ESPAsyncWebServer_H_

#include "Arduino.h"

#include <functional>
#include "FS.h"

#include "StringArray.h"

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#else
#error Platform not supported
#endif

#ifdef ASYNCWEBSERVER_REGEX
#define ASYNCWEBSERVER_REGEX_ATTRIBUTE
#else
#define ASYNCWEBSERVER_REGEX_ATTRIBUTE __attribute__((warning("ASYNCWEBSERVER_REGEX not defined")))
#endif

#define DEBUGF(...) //Serial.printf(__VA_ARGS__)

class AsyncWebServer;
class AsyncWebServerRequest;
class AsyncWebServerResponse;
class AsyncWebHeader;
class AsyncWebParameter;
class AsyncWebRewrite;
class AsyncWebHandler;
class AsyncStaticWebHandler;
class AsyncCallbackWebHandler;
class AsyncResponseStream;

#ifndef WEBSERVER_H
typedef enum {
  HTTP_GET     = 0b00000001,
  HTTP_POST    = 0b00000010,
  HTTP_DELETE  = 0b00000100,
  HTTP_PUT     = 0b00001000,
  HTTP_PATCH   = 0b00010000,
  HTTP_HEAD    = 0b00100000,
  HTTP_OPTIONS = 0b01000000,
  HTTP_ANY     = 0b01111111,
} WebRequestMethod;
#endif

#ifndef HAVE_FS_FILE_OPEN_MODE
namespace fs {
    class FileOpenMode {
    public:
        static const char *read;
        static const char *write;
        static const char *append;
    };
};
#else
#include "FileOpenMode.h"
#endif

//if this value is returned when asked for data, packet will not be sent and you will be asked for data again
#define RESPONSE_TRY_AGAIN 0xFFFFFFFF

typedef uint8_t WebRequestMethodComposite;
typedef std::function<void(void)> ArDisconnectHandler;

/*
 * PARAMETER :: Chainable object to hold GET/POST and FILE parameters
 * */

class AsyncWebParameter {
  private:
    String _name;
    String _value;
    size_t _size;
    bool _isForm;
    bool _isFile;

  public:

    AsyncWebParameter(const String& name, const String& value, bool form=false, bool file=false, size_t size=0): _name(name), _value(value), _size(size), _isForm(form), _isFile(file){}
    const String& name() const { return _name; }
    const String& value() const { return _value; }
    size_t size() const { return _size; }
    bool isPost() const { return _isForm; }
    bool isFile() const { return _isFile; }
};

/*
 * HEADER :: Chainable object to hold the headers
 * */

class AsyncWebHeader {
  private:
    String _name;
    String _value;

  public:
    AsyncWebHeader(const String& name, const String& value): _name(name), _value(value){}
    AsyncWebHeader(const String& data): _name(), _value(){
      if(!data) return;
      int index = data.indexOf(':');
      if (index < 0) return;
      _name = data.substring(0, index);
      _value = data.substring(index + 2);
    }
    ~AsyncWebHeader(){}
    const String& name() const { return _name; }
    const String& value() const { return _value; }
    String toString() const { return String(_name + F(": ") + _value + F("\r\n")); }
};

/*
 * REQUEST :: Each incoming Client is wrapped inside a Request and both live together until disconnect
 * */

typedef enum { RCT_NOT_USED = -1, RCT_DEFAULT = 0, RCT_HTTP, RCT_WS, RCT_EVENT, RCT_MAX } RequestedConnectionType;

typedef std::function<size_t(uint8_t*, size_t, size_t)> AwsResponseFiller;
typedef std::function<String(const String&)> AwsTemplateProcessor;

class AsyncWebServerRequest {
  using File = fs::File;
  using FS = fs::FS;
  friend class AsyncWebServer;
  friend class AsyncCallbackWebHandler;
  friend class HttpCookieHeader;
  private:
    AsyncClient* _client;
    AsyncWebServer* _server;
    AsyncWebHandler* _handler;
    AsyncWebServerResponse* _response;
    StringArray _interestingHeaders;
    ArDisconnectHandler _onDisconnectfn;

    String _temp;
    uint8_t _parseState;

    uint8_t _version;
    WebRequestMethodComposite _method;
    String _url;
    String _host;
    String _contentType;
    String _boundary;
    String _authorization;
    RequestedConnectionType _reqconntype;
    void _removeNotInterestingHeaders();
    bool _isDigest;
    bool _isMultipart;
    bool _isPlainPost;
    bool _expectingContinue;
    size_t _contentLength;
    size_t _parsedLength;

    LinkedList<AsyncWebHeader *> _headers;
    LinkedList<AsyncWebParameter *> _params;
    LinkedList<String *> _pathParams;

    uint8_t _multiParseState;
    uint8_t _boundaryPosition;
    size_t _itemStartIndex;
    size_t _itemSize;
    String _itemName;
    String _itemFilename;
    String _itemType;
    String _itemValue;
    uint8_t *_itemBuffer;
    size_t _itemBufferIndex;
    bool _itemIsFile;

    void _onPoll();
    void _onAck(size_t len, uint32_t time);
    void _onError(int8_t error);
    void _onTimeout(uint32_t time);
    void _onDisconnect();
    void _onData(void *buf, size_t len);

    void _addParam(AsyncWebParameter*);
    void _addPathParam(const char *param);

    bool _parseReqHead();
    bool _parseReqHeader();
    void _parseLine();
    void _parsePlainPostChar(uint8_t data);
    void _parseMultipartPostByte(uint8_t data, bool last);
    void _addGetParams(const String& params);

    void _handleUploadStart();
    void _handleUploadByte(uint8_t data, bool last);
    void _handleUploadEnd();

  public:
    File _tempFile;
    void *_tempObject;

    AsyncWebServerRequest(AsyncWebServer*, AsyncClient*);
    ~AsyncWebServerRequest();

    AsyncClient* client(){ return _client; }
    uint8_t version() const { return _version; }
    WebRequestMethodComposite method() const { return _method; }
    const String& url() const { return _url; }
    const String& host() const { return _host; }
    const String& contentType() const { return _contentType; }
    size_t contentLength() const { return _contentLength; }
    bool multipart() const { return _isMultipart; }
    const __FlashStringHelper *methodToString() const;
    const __FlashStringHelper *requestedConnTypeToString() const;
    RequestedConnectionType requestedConnType() const { return _reqconntype; }
    bool isExpectedRequestedConnType(RequestedConnectionType erct1, RequestedConnectionType erct2 = RCT_NOT_USED, RequestedConnectionType erct3 = RCT_NOT_USED);
    void onDisconnect (ArDisconnectHandler fn);

    //hash is the string representation of:
    // base64(user:pass) for basic or
    // user:realm:md5(user:realm:pass) for digest
    bool authenticate(const char * hash);
    bool authenticate(const char * username, const char * password, const char * realm = NULL, bool passwordIsHash = false);
    void requestAuthentication(const char * realm = NULL, bool isDigest = true);

    void setHandler(AsyncWebHandler *handler){ _handler = handler; }
    void addInterestingHeader(const String& name);

    void redirect(const String& url);

    void send(AsyncWebServerResponse *response);
    void send(int code, const String& contentType=String(), const String& content=String());
    void send(FS &fs, const String& path, const String& contentType=String(), bool download=false, AwsTemplateProcessor callback=nullptr);
    void send(File content, const String& path, const String& contentType=String(), bool download=false, AwsTemplateProcessor callback=nullptr);
    void send(Stream &stream, const String& contentType, size_t len, AwsTemplateProcessor callback=nullptr);
    void send(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback=nullptr);
    void sendChunked(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback=nullptr);
    void send_P(int code, const String& contentType, const uint8_t * content, size_t len, AwsTemplateProcessor callback=nullptr);
    void send_P(int code, const String& contentType, PGM_P content, AwsTemplateProcessor callback=nullptr);

    AsyncWebServerResponse *beginResponse(int code, const String& contentType=String(), const String& content=String());
    AsyncWebServerResponse *beginResponse(FS &fs, const String& path, const String& contentType=String(), bool download=false, AwsTemplateProcessor callback=nullptr);
    AsyncWebServerResponse *beginResponse(File content, const String& path, const String& contentType=String(), bool download=false, AwsTemplateProcessor callback=nullptr);
    AsyncWebServerResponse *beginResponse(Stream &stream, const String& contentType, size_t len, AwsTemplateProcessor callback=nullptr);
    AsyncWebServerResponse *beginResponse(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback=nullptr);
    AsyncWebServerResponse *beginChunkedResponse(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback=nullptr);
    AsyncResponseStream *beginResponseStream(const String& contentType, size_t bufferSize=1460);
    AsyncWebServerResponse *beginResponse_P(int code, const String& contentType, const uint8_t * content, size_t len, AwsTemplateProcessor callback=nullptr);
    AsyncWebServerResponse *beginResponse_P(int code, const String& contentType, PGM_P content, AwsTemplateProcessor callback=nullptr);


    // copying PROGMEM strings into memory provides a better performance
    // when it comes to long strings. strcmp is about 10 times faster than
    // strcmp_P. since strcmp/strcmp_P is only executed when the length
    // of both strings matches and the overhead of the the entire loop
    // is taken into account, there is not much left. String and const char *
    // are using strcmp, const __FLashStringHelper * is using strcmp_P
    //
    // For best performance, use the correct tpype, do not convert PROGMEM or
    // or const char into String objects, use const ref to store the
    // return values instead of copying the entire string and do not use
    // hasArg() + arg() etc... since it is executing the exact same code just
    // returning a different type of result
    //
    // after retrieving the value, argExists() and headerExists() can
    // be used to determine if length is 0 or if the argument/header does not
    // exist without calling hasArg/hasHeader
    //
    // const String *pUsername;
    // if ((pUsername = &request->arg(F("username")))->length() != 0) {
    //      // not empty
    // }
    // else if (request.argExists(*pUsername)) {
    //     // empty
    // }
    //
    // auto &username = request->arg(F("username"));
    // if (request.argExists(username)) {
    //     if (username.length() == 0) {
    //         // could be emtpy!
    //     }
    // }

    static constexpr size_t kAutoStrLen = ~0;

    // check if parameter exists
    bool hasParam(const String &name, bool post = false, bool file = false) const;
    bool hasParam(const __FlashStringHelper *name, bool post = false, bool file = false) const;
    bool hasParam(const char *name, bool post = false, bool file = false, size_t len = kAutoStrLen) const;

    // get parameter
    AsyncWebParameter *getParam(const String &name, bool post = false, bool file = false) const;
    AsyncWebParameter *getParam(const __FlashStringHelper *name, bool post = false, bool file = false) const;
    AsyncWebParameter *getParam(const char *name, bool post = false, bool file = false, size_t len = kAutoStrLen) const;

    AsyncWebParameter *getParam(size_t num) const;
    // get arguments count
    size_t params() const;

    // get arguments count
    size_t args() const { return params(); }
    // check if argument exists
    bool hasArg(const String &name) const;
    bool hasArg(const __FlashStringHelper *name) const;
    bool hasArg(const char *name, size_t len = kAutoStrLen) const;

    // get request argument value by name
    const String &arg(const String &name) const;
    const String &arg(const __FlashStringHelper *name) const;
    const String &arg(const char *name, size_t len = kAutoStrLen) const;
    //---

    // get request argument value by number
    const String& arg(size_t i) const;
    // get request argument name by number
    const String& argName(size_t i) const;

    const String& ASYNCWEBSERVER_REGEX_ATTRIBUTE pathArg(size_t i) const;

    // get header count
    size_t headers() const;
    // check if header exists
    bool hasHeader(const String &name) const;
    bool hasHeader(const __FlashStringHelper *name) const;
    bool hasHeader(const char *name, size_t len = kAutoStrLen) const;

    // get request header by name
    AsyncWebHeader *getHeader(const String &name) const;
    AsyncWebHeader *getHeader(const __FlashStringHelper *name) const;
    AsyncWebHeader *getHeader(const char *name, size_t len = kAutoStrLen) const;
    //---

    // get header by index
    AsyncWebHeader* getHeader(size_t num) const;

    // get request header value by name
    const String &header(const String &name) const;
    const String &header(const __FlashStringHelper *name) const;
    const String &header(const char *name, size_t len = kAutoStrLen) const;

    // get request header value by number
    const String &header(size_t i) const;
    // get request header name by number
    const String &headerName(size_t i) const;

    // returns true if arg() or header() exist
    static bool argExists(const String &str);
    // alias for argExists()
    static bool headerExists(const String &str);

    // get linkd list of the headers
    inline __attribute__((__always_inline__))
    LinkedList<AsyncWebHeader *> &getHeaders() {
        return _headers;
    }
    inline __attribute__((__always_inline__))
    const LinkedList<AsyncWebHeader *> &getHeaders() const {
        return _headers;
    }

    enum class UrlDecodeErrorType {
        NONE = 0,
        NOT_ENOUGH_DIGITS,
        INVALID_CHARACTERS,
    };

    // if any error occurs and returnEmptyOnMalformedInput is not nullptr,
    // the error is stored in returnEmptyOnMalformedInput and an emtpy string returned
    // on success returnEmptyOnMalformedInput is set to UrlDecodeErrorType::NONE
    static String urlDecode(const String& text, UrlDecodeErrorType *returnEmptyOnMalformedInput = nullptr);
};

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasParam(const String &name, bool post, bool file) const {
    return getParam(name.c_str(), post, file, name.length()) != nullptr;
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasParam(const __FlashStringHelper *name, bool post, bool file) const {
    return getParam(name, post, file) != nullptr;
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasParam(const char *name, bool post, bool file, size_t len) const {
    return getParam(name, post, file, len) != nullptr;
}

inline __attribute__((__always_inline__))
AsyncWebParameter *AsyncWebServerRequest::getParam(const String &name, bool post, bool file) const {
    return getParam(name.c_str(), post, file, name.length());
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasHeader(const String &name) const {
    return hasHeader(name.c_str(), name.length());
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasHeader(const __FlashStringHelper *name) const {
    return getHeader(name) != nullptr;
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasHeader(const char *name, size_t len) const {
    return getHeader(name, len) != nullptr;
}

inline __attribute__((__always_inline__))
AsyncWebHeader *AsyncWebServerRequest::getHeader(const String &name) const {
    return getHeader(name.c_str(), name.length());
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasArg(const String &name) const {
    return hasArg(name.c_str(), name.length());
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasArg(const __FlashStringHelper *name) const {
    return std::addressof(arg(name)) != std::addressof(emptyString);
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::hasArg(const char *name, size_t len) const {
    return std::addressof(arg(name, len)) != std::addressof(emptyString);
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::header(const String &name) const {
    return header(name.c_str(), name.length());
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::header(const __FlashStringHelper *name) const {
  AsyncWebHeader *h = getHeader(name);
  return h ? h->value() : emptyString;
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::header(const char *name, size_t len) const {
  AsyncWebHeader *h = getHeader(name, len);
  return h ? h->value() : emptyString;
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::arg(const String &name) const {
    return arg(name.c_str(), name.length());
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::arg(size_t i) const
{
  return getParam(i)->value();
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::argName(size_t i) const
{
  return getParam(i)->name();
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::pathArg(size_t i) const
{
  auto param = _pathParams.nth(i);
  return param ? **param : emptyString;
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::header(size_t i) const
{
  AsyncWebHeader *h = getHeader(i);
  return h ? h->value() : emptyString;
}

inline __attribute__((__always_inline__))
const String &AsyncWebServerRequest::headerName(size_t i) const
{
  AsyncWebHeader *h = getHeader(i);
  return h ? h->name() : emptyString;
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::argExists(const String &str) {
    return std::addressof(str) != std::addressof(emptyString);
}

inline __attribute__((__always_inline__))
bool AsyncWebServerRequest::headerExists(const String &str) {
    return argExists(str);
}

/*
 * FILTER :: Callback to filter AsyncWebRewrite and AsyncWebHandler (done by the Server)
 * */

typedef std::function<bool(AsyncWebServerRequest *request)> ArRequestFilterFunction;

bool ON_STA_FILTER(AsyncWebServerRequest *request);

bool ON_AP_FILTER(AsyncWebServerRequest *request);

/*
 * REWRITE :: One instance can be handle any Request (done by the Server)
 * */

class AsyncWebRewrite {
  protected:
    String _from;
    String _toUrl;
    String _params;
    ArRequestFilterFunction _filter;
  public:
    AsyncWebRewrite(const char* from, const char* to): _from(from), _toUrl(to), _params(String()), _filter(NULL){
      int index = _toUrl.indexOf('?');
      if (index > 0) {
        _params = _toUrl.substring(index +1);
        _toUrl = _toUrl.substring(0, index);
      }
    }
    virtual ~AsyncWebRewrite(){}
    AsyncWebRewrite& setFilter(ArRequestFilterFunction fn) { _filter = fn; return *this; }
    bool filter(AsyncWebServerRequest *request) const { return _filter == NULL || _filter(request); }
    const String& from(void) const { return _from; }
    const String& toUrl(void) const { return _toUrl; }
    const String& params(void) const { return _params; }
    virtual bool match(AsyncWebServerRequest *request) { return from() == request->url() && filter(request); }
};

/*
 * HANDLER :: One instance can be attached to any Request (done by the Server)
 * */

class AsyncWebHandler {
  protected:
    ArRequestFilterFunction _filter;
    String _username;
    String _password;
  public:
    AsyncWebHandler() {}
    AsyncWebHandler& setFilter(ArRequestFilterFunction fn) { _filter = fn; return *this; }
    AsyncWebHandler& setAuthentication(const char *username, const char *password){  _username = username; _password = password; return *this; };
    AsyncWebHandler& setAuthentication(const String &username, const String &password){  _username = username; _password = password; return *this; };
    bool filter(AsyncWebServerRequest *request){ return !_filter || _filter(request); }
    virtual ~AsyncWebHandler(){}
    virtual bool canHandle(AsyncWebServerRequest *request __attribute__((unused))){
      return false;
    }
    virtual void handleRequest(AsyncWebServerRequest *request __attribute__((unused))){}
    virtual void handleUpload(AsyncWebServerRequest *request  __attribute__((unused)), const String& filename __attribute__((unused)), size_t index __attribute__((unused)), uint8_t *data __attribute__((unused)), size_t len __attribute__((unused)), bool final  __attribute__((unused))){}
    virtual void handleBody(AsyncWebServerRequest *request __attribute__((unused)), uint8_t *data __attribute__((unused)), size_t len __attribute__((unused)), size_t index __attribute__((unused)), size_t total __attribute__((unused))){}
    virtual bool isRequestHandlerTrivial(){return true;}
};

/*
 * RESPONSE :: One instance is created for each Request (attached by the Handler)
 * */

typedef enum {
  RESPONSE_SETUP, RESPONSE_HEADERS, RESPONSE_CONTENT, RESPONSE_WAIT_ACK, RESPONSE_END, RESPONSE_FAILED
} WebResponseState;

class AsyncWebServerResponse {
  protected:
    int _code;
    LinkedList<AsyncWebHeader *> _headers;
    String _contentType;
    size_t _contentLength;
    bool _sendContentLength;
    bool _chunked;
    size_t _headLength;
    size_t _sentLength;
    size_t _ackedLength;
    size_t _writtenLength;
    WebResponseState _state;
    const char* _responseCodeToString(int code);
public:
    static const __FlashStringHelper *responseCodeToString(int code);

  public:
    AsyncWebServerResponse();
    virtual ~AsyncWebServerResponse();
    virtual void setCode(int code);
    virtual void setContentLength(size_t len);
    virtual void setContentType(const String& type);
    virtual void addHeader(const String& name, const String& value);
    virtual String _assembleHead(uint8_t version);
    virtual bool _started() const;
    virtual bool _finished() const;
    virtual bool _failed() const;
    virtual bool _sourceValid() const;
    virtual void _respond(AsyncWebServerRequest *request);
    virtual size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t time);
};

/*
 * SERVER :: One instance
 * */

typedef std::function<void(AsyncWebServerRequest *request)> ArRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final)> ArUploadHandlerFunction;
typedef std::function<void(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)> ArBodyHandlerFunction;

class AsyncWebServer {
  protected:
    AsyncServer _server;
    LinkedList<AsyncWebRewrite*> _rewrites;
    LinkedList<AsyncWebHandler*> _handlers;
    AsyncCallbackWebHandler* _catchAllHandler;

  public:
    AsyncWebServer(uint16_t port);
    ~AsyncWebServer();

    void begin();
    void end();

#if ASYNC_TCP_SSL_ENABLED
    void onSslFileRequest(AcSSlFileHandler cb, void* arg);
    void beginSecure(const char *cert, const char *private_key_file, const char *password);
#endif

    AsyncWebRewrite& addRewrite(AsyncWebRewrite* rewrite);
    bool removeRewrite(AsyncWebRewrite* rewrite);
    AsyncWebRewrite& rewrite(const char* from, const char* to);

    AsyncWebHandler& addHandler(AsyncWebHandler* handler);
    bool removeHandler(AsyncWebHandler* handler);

    AsyncCallbackWebHandler& on(const char* uri, ArRequestHandlerFunction onRequest);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody);

    AsyncStaticWebHandler& serveStatic(const char* uri, fs::FS& fs, const char* path, const char* cache_control = NULL);

    void onNotFound(ArRequestHandlerFunction fn);  //called when handler is not assigned
    void onFileUpload(ArUploadHandlerFunction fn); //handle file uploads
    void onRequestBody(ArBodyHandlerFunction fn); //handle posts with plain body content (JSON often transmitted this way as a request)

    void reset(); //remove all writers and handlers, with onNotFound/onFileUpload/onRequestBody

    void _handleDisconnect(AsyncWebServerRequest *request);
    void _attachHandler(AsyncWebServerRequest *request);
    void _rewriteRequest(AsyncWebServerRequest *request);
};

class DefaultHeaders {
  using headers_t = LinkedList<AsyncWebHeader *>;
  headers_t _headers;

  DefaultHeaders()
  :_headers(headers_t([](AsyncWebHeader *h){ delete h; }))
  {}
public:
  using ConstIterator = headers_t::ConstIterator;

  void addHeader(const String& name, const String& value){
    _headers.add(new AsyncWebHeader(name, value));
  }

  ConstIterator begin() const { return _headers.begin(); }
  ConstIterator end() const { return _headers.end(); }

  DefaultHeaders(DefaultHeaders const &) = delete;
  DefaultHeaders &operator=(DefaultHeaders const &) = delete;
  static DefaultHeaders &Instance() {
    static DefaultHeaders instance;
    return instance;
  }
};

#include "WebResponseImpl.h"
#include "WebHandlerImpl.h"
#include "AsyncWebSocket.h"
#include "AsyncEventSource.h"

#endif /* _AsyncWebServer_H_ */
