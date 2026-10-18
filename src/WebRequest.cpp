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
#include "ESPAsyncWebServer.h"
#include <Arduino_compat.h>
#include "WebResponseImpl.h"
#include "WString.h"

#define SharedEmptyString emptyString

#define __is_param_char(c) ((c) && ((c) != '{') && ((c) != '[') && ((c) != '&') && ((c) != '='))

AsyncWebServerRequest::AsyncWebServerRequest(AsyncWebServer* s, AsyncClient* c)
  : _client(c)
  , _server(s)
  , _handler(NULL)
  , _response(NULL)
  , _temp()
  , _parseState(0)
  , _version(0)
  , _method(HTTP_ANY)
  , _url()
  , _host()
  , _contentType()
  , _boundary()
  , _reqconntype(RCT_HTTP)
  , _isMultipart(false)
  , _isPlainPost(false)
  , _expectingContinue(false)
  , _contentLength(0)
  , _parsedLength(0)
  , _headers(LinkedList<AsyncWebHeader *>([](AsyncWebHeader *h){ delete h; }))
  , _params(LinkedList<AsyncWebParameter *>([](AsyncWebParameter *p){ delete p; }))
  , _pathParams(LinkedList<String *>([](String *p){ delete p; }))
  , _multiParseState(0)
  , _boundaryPosition(0)
  , _itemStartIndex(0)
  , _itemSize(0)
  , _itemName()
  , _itemFilename()
  , _itemType()
  , _itemValue()
  , _itemBuffer(0)
  , _itemBufferIndex(0)
  , _itemIsFile(false)
  , _nextRequest(nullptr)
  , _cbDepth(0)
  , _disconnected(false)
  , _deleteLater(false)
  , _deadClient(nullptr)
  , _tempObject(NULL)
  , _tempObjectDeleter(nullptr)
{
  if (c) {
    // _enter()/_leave() around every callback: see _cbDepth
    c->onError([](void *r, AsyncClient* c, int8_t error){ (void)c; AsyncWebServerRequest *req = (AsyncWebServerRequest*)r; req->_enter(); req->_onError(error); req->_leave(); }, this);
    c->onAck([](void *r, AsyncClient* c, size_t len, uint32_t time){ (void)c; AsyncWebServerRequest *req = (AsyncWebServerRequest*)r; req->_enter(); req->_onAck(len, time); req->_leave(); }, this);
    c->onDisconnect([](void *r, AsyncClient* c){ AsyncWebServerRequest *req = (AsyncWebServerRequest*)r; req->_clientDisconnected(c); }, this);
    c->onTimeout([](void *r, AsyncClient* c, uint32_t time){ (void)c; AsyncWebServerRequest *req = (AsyncWebServerRequest*)r; req->_enter(); req->_onTimeout(time); req->_leave(); }, this);
    c->onData([](void *r, AsyncClient* c, void *buf, size_t len){ (void)c; AsyncWebServerRequest *req = (AsyncWebServerRequest*)r; req->_enter(); req->_onData(buf, len); req->_leave(); }, this);
    c->onPoll([](void *r, AsyncClient* c){ (void)c; AsyncWebServerRequest *req = ( AsyncWebServerRequest*)r; req->_enter(); req->_onPoll(); req->_leave(); }, this);
  }
  if (_server) {
    _server->_addRequest(this);
  }
}

AsyncWebServerRequest::~AsyncWebServerRequest(){
  if (_server) {
    _server->_removeRequest(this);
  }
  // first, the object may still use the request
  clearTempObject();

  _headers.free();

  _params.free();
  _pathParams.free();

  _interestingHeaders.free();

  if (_response != NULL){
    delete _response;
  }

  if (_tempFile){
    _tempFile.close();
  }
  if (_itemBuffer){
    free(_itemBuffer);
  }
}

void AsyncWebServerRequest::_onData(void *buf, size_t len){
  size_t i = 0;
  while (true) {

  if(_parseState < PARSE_REQ_BODY){
    // Find new line in buf
    char *str = (char*)buf;
    for (i = 0; i < len; i++) {
      if (str[i] == '\n') {
        break;
      }
    }
    if (i == len) { // No new line, just add the buffer in _temp
      char ch = str[len-1];
      str[len-1] = 0;
      _temp.reserve(_temp.length()+len);
      _temp.concat(str);
      _temp.concat(ch);
    } else { // Found new line - extract it and parse
      str[i] = 0; // Terminate the string at the end of the line.
      _temp.concat(str);
      _temp.trim();
      _parseLine();
      if(_disconnected){
        return;
      }
      if (++i < len) {
        // Still have more buffer to process
        buf = str+i;
        len-= i;
        continue;
      }
    }
  } else if(_parseState == PARSE_REQ_BODY){
    // #9: data beyond Content-Length (pipelining, garbage) is not part of the body
    if(len > _contentLength - _parsedLength){
      len = _contentLength - _parsedLength;
    }
    // A handler should be already attached at this point in _parseLine function.
    // If handler does nothing (_onRequest is NULL), we don't need to really parse the body.
    const bool needParse = _handler && !_handler->isRequestHandlerTrivial();
    if(_isMultipart){
      if(needParse){
        size_t i;
        for(i=0; i<len; i++){
          _parseMultipartPostByte(((uint8_t*)buf)[i], i == len - 1);
          _parsedLength++;
          if(_disconnected){
            return; // an upload handler closed the connection
          }
        }
      } else
          _parsedLength += len;
    } else {
      if(_parsedLength == 0){
        if(_contentType.startsWith(F("application/x-www-form-urlencoded"))){
          _isPlainPost = true;
        } else if(F("text/plain") == _contentType && __is_param_char(((char*)buf)[0])){
          size_t i = 0;
          while (i<len && __is_param_char(((char*)buf)[i++]));
          if(i < len && ((char*)buf)[i-1] == '='){
            _isPlainPost = true;
          }
        }
      }
      if(!_isPlainPost) {
        if(_handler) _handler->handleBody(this, (uint8_t*)buf, len, _parsedLength, _contentLength);
        _parsedLength += len;
      } else if(needParse) {
        size_t i;
        for(i=0; i<len; i++){
          _parsedLength++;
          _parsePlainPostChar(((uint8_t*)buf)[i]);
        }
      } else {
        _parsedLength += len;
      }
    }
    if(_parsedLength == _contentLength){
      _parseState = PARSE_REQ_END;
      if(_handler) _handler->handleRequest(this);
      else send(501);
    }
  }
  break;
  }
}

void AsyncWebServerRequest::_removeNotInterestingHeaders(){
  if (_interestingHeaders.containsIgnoreCase(F("ANY"))) return; // nothing to do
  _headers.remove_if([this](AsyncWebHeader * const &header){
    return !_interestingHeaders.containsIgnoreCase(header->name());
  });
}

void AsyncWebServerRequest::_onPoll(){
  //os_printf("p\n");
  if(_response != NULL && _client != NULL && _client->canSend() && !_response->_finished()){
    _response->_ack(this, 0, 0);
  }
}

void AsyncWebServerRequest::_onAck(size_t len, uint32_t time){
  //os_printf("a:%u:%u\n", len, time);
  if(_response != NULL){
    if(!_response->_finished()){
      _response->_ack(this, len, time);
    } else {
      AsyncWebServerResponse* r = _response;
      _response = NULL;
      delete r;
    }
  }
}

void AsyncWebServerRequest::_onError(int8_t error){
  (void)error;
}

void AsyncWebServerRequest::_onTimeout(uint32_t time){
  (void)time;
  //os_printf("TIMEOUT: %u, state: %s\n", time, _client->stateToString());
  _client->close();
}

void AsyncWebServerRequest::onDisconnect (ArDisconnectHandler fn){
    _onDisconnectfn=fn;
}

void AsyncWebServerRequest::_clientDisconnected(AsyncClient *c){
  if(_cbDepth){
    // inside a callback of this request: deleted by _leave()
    _disconnected = true;
    _deadClient = c;
    return;
  }
  _onDisconnect();
  delete c;
}

void AsyncWebServerRequest::_leave(){
  if(--_cbDepth || !(_disconnected || _deleteLater)){
    return;
  }
  if(_deleteLater){
    delete this;
    return;
  }
  auto c = _deadClient;
  _onDisconnect(); // calls the disconnect handler and deletes the request
  delete c;
}

void AsyncWebServerRequest::_deleteSelf(){
  if(_cbDepth){
    _deleteLater = true;
    return;
  }
  delete this;
}

void AsyncWebServerRequest::_onDisconnect(){
  //os_printf("d\n");
  if(_onDisconnectfn) {
      _onDisconnectfn();
    }
  // the destructor removes the request from its server, the server may already be gone
  delete this;
}

void AsyncWebServerRequest::_addParam(AsyncWebParameter *p){
  _params.add(p);
}

void AsyncWebServerRequest::_addPathParam(const char *p){
  _pathParams.add(new String(p));
}

void AsyncWebServerRequest::_addGetParams(const String& params){
  size_t start = 0;
  while (start < params.length()){
    int end = params.indexOf('&', start);
    if (end < 0) end = params.length();
    int equal = params.indexOf('=', start);
    if (equal < 0 || equal > end) equal = end;
    String name = params.substring(start, equal);
    String value = equal + 1 < end ? params.substring(equal + 1, end) : String();
    _addParam(new AsyncWebParameter(urlDecode(name), urlDecode(value)));
    start = end + 1;
  }
}

bool AsyncWebServerRequest::_parseReqHead(){
  // Split the head into method, url and version
  int index = _temp.indexOf(' ');
  String m = _temp.substring(0, index);
  index = _temp.indexOf(' ', index+1);
  String u = _temp.substring(m.length()+1, index);
  _temp = _temp.substring(index+1);

  if(F("GET") == m){
    _method = HTTP_GET;
  } else if(F("POST") == m){
    _method = HTTP_POST;
  } else if(F("DELETE") == m){
    _method = HTTP_DELETE;
  } else if(F("PUT") == m){
    _method = HTTP_PUT;
  } else if(F("PATCH") == m){
    _method = HTTP_PATCH;
  } else if(F("HEAD") == m){
    _method = HTTP_HEAD;
  } else if(F("OPTIONS") == m){
    _method = HTTP_OPTIONS;
  } else {
    _method = HTTP_UNKNOWN;
  }

  String g = String();
  index = u.indexOf('?');
  if(index > 0){
    g = u.substring(index +1);
    u = u.substring(0, index);
  }
  _url = urlDecode(u);
  // CR/LF in the URL would end up in headers (Location, Content-Disposition, ...)
  _url.replace(F("\r"), emptyString);
  _url.replace(F("\n"), emptyString);
  _addGetParams(g);

  if(!_temp.startsWith(F("HTTP/1.0")))
    _version = 1;

  _temp = String();
  return true;
}

bool AsyncWebServerRequest::_parseReqHeader(){
  int index = _temp.indexOf(':');
  // a line without a name or without ':' is not a header
  if(index > 0){
    String name = _temp.substring(0, index);
    // optional whitespace around the value ("Host:x", "Host:  x")
    String value = _temp.substring(index + 1);
    value.trim();
    if(name.equalsIgnoreCase(F("Host"))){
      _host = value;
    } else if(name.equalsIgnoreCase(F("Content-Type"))){
	  _contentType = value.substring(0, value.indexOf(';'));
      if (value.startsWith(F("multipart/"))){
        _boundary = value.substring(value.indexOf('=')+1);
        _boundary.replace(F("\""), F(""));
        _isMultipart = true;
      }
    } else if(name.equalsIgnoreCase(F("Content-Length"))){
      _contentLength = atoi(value.c_str());
    } else if(name.equalsIgnoreCase(F("Expect")) && F("100-continue") == value){
      _expectingContinue = true;
    } else {
      if(name.equalsIgnoreCase(F("Upgrade")) && value.equalsIgnoreCase(F("websocket"))){
        // WebSocket request can be uniquely identified by header: [Upgrade: websocket]
        _reqconntype = RCT_WS;
      } else {

        if(name.equalsIgnoreCase(F("Accept")) && StrView(value).indexOfIgnoreCase(F("text/event-stream")) != -1){
          // WebEvent request can be uniquely identified by header:  [Accept: text/event-stream]
          _reqconntype = RCT_EVENT;
        }
      }
    }
    _headers.add(new AsyncWebHeader(name, value));
  }
  _temp = String();
  return true;
}

void AsyncWebServerRequest::_parsePlainPostChar(uint8_t data){
  if(data && (char)data != '&')
    _temp += (char)data;
  if(!data || (char)data == '&' || _parsedLength == _contentLength){
    String name = F("body");
    String value = _temp;
    if(!StrView(_temp).startsWith('{') && !StrView(_temp).startsWith('[') && _temp.indexOf('=') > 0){
      name = _temp.substring(0, _temp.indexOf('='));
      value = _temp.substring(_temp.indexOf('=') + 1);
    }
    _addParam(new AsyncWebParameter(urlDecode(name), urlDecode(value), true));
    _temp = String();
  }
}

void AsyncWebServerRequest::_handleUploadByte(uint8_t data, bool last){
  _itemBuffer[_itemBufferIndex++] = data;

  if(last || _itemBufferIndex == 1460){
    if(_handler)
      _handler->handleUpload(this, _itemFilename, _itemSize - _itemBufferIndex, _itemBuffer, _itemBufferIndex, false);
    _itemBufferIndex = 0;
  }
}

enum {
  EXPECT_BOUNDARY,
  PARSE_HEADERS,
  WAIT_FOR_RETURN1,
  EXPECT_FEED1,
  EXPECT_DASH1,
  EXPECT_DASH2,
  BOUNDARY_OR_DATA,
  DASH3_OR_RETURN2,
  EXPECT_FEED2,
  PARSING_FINISHED,
  PARSE_ERROR
};

void AsyncWebServerRequest::_parseMultipartPostByte(uint8_t data, bool last){
#define itemWriteByte(b) do { _itemSize++; if(_itemIsFile) _handleUploadByte(b, last); else _itemValue+=(char)(b); } while(0)

  if(!_parsedLength){
    _multiParseState = EXPECT_BOUNDARY;
    _temp = String();
    _itemName = String();
    _itemFilename = String();
    _itemType = String();
  }

  if(_multiParseState == WAIT_FOR_RETURN1){
    if(data != '\r'){
      itemWriteByte(data);
    } else {
      _multiParseState = EXPECT_FEED1;
    }
  } else if(_multiParseState == EXPECT_BOUNDARY){
    if(_parsedLength < 2 && data != '-'){
      _multiParseState = PARSE_ERROR;
      return;
    } else if(_parsedLength - 2 < _boundary.length() && _boundary.c_str()[_parsedLength - 2] != data){
      _multiParseState = PARSE_ERROR;
      return;
    } else if(_parsedLength - 2 == _boundary.length() && data != '\r'){
      _multiParseState = PARSE_ERROR;
      return;
    } else if(_parsedLength - 3 == _boundary.length()){
      if(data != '\n'){
        _multiParseState = PARSE_ERROR;
        return;
      }
      _multiParseState = PARSE_HEADERS;
      _itemIsFile = false;
    }
  } else if(_multiParseState == PARSE_HEADERS){
    if((char)data != '\r' && (char)data != '\n')
       _temp += (char)data;
    if((char)data == '\n'){
      if(_temp.length()){
        if(_temp.length() > 12 && _temp.substring(0, 12).equalsIgnoreCase(F("Content-Type"))){
          _itemType = _temp.substring(14);
          _itemIsFile = true;
        } else if(_temp.length() > 19 && _temp.substring(0, 19).equalsIgnoreCase(F("Content-Disposition"))){
          _temp = _temp.substring(_temp.indexOf(';') + 2);
          while(_temp.indexOf(';') > 0){
            String name = _temp.substring(0, _temp.indexOf('='));
            String nameVal = _temp.substring(_temp.indexOf('=') + 2, _temp.indexOf(';') - 1);
            if(F("name") == name) {
              _itemName = nameVal;
            } else if(F("filename") == name) {
              _itemFilename = nameVal;
              _itemIsFile = true;
            }
            _temp = _temp.substring(_temp.indexOf(';') + 2);
          }
          String name = _temp.substring(0, _temp.indexOf('='));
          String nameVal = _temp.substring(_temp.indexOf('=') + 2, _temp.length() - 1);
          if(F("name") == name){
            _itemName = nameVal;
          } else if(F("filename") == name){
            _itemFilename = nameVal;
            _itemIsFile = true;
          }
        }
        _temp = String();
      } else {
        _multiParseState = WAIT_FOR_RETURN1;
        //value starts from here
        _itemSize = 0;
        _itemStartIndex = _parsedLength;
        _itemValue = String();
        if(_itemIsFile){
          if(_itemBuffer)
            free(_itemBuffer);
          _itemBuffer = (uint8_t*)malloc(1460);
          if(_itemBuffer == NULL){
            _multiParseState = PARSE_ERROR;
            return;
          }
          _itemBufferIndex = 0;
        }
      }
    }
  } else if(_multiParseState == EXPECT_FEED1){
    if(data != '\n'){
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); _parseMultipartPostByte(data, last);
    } else {
      _multiParseState = EXPECT_DASH1;
    }
  } else if(_multiParseState == EXPECT_DASH1){
    if(data != '-'){
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); itemWriteByte('\n');  _parseMultipartPostByte(data, last);
    } else {
      _multiParseState = EXPECT_DASH2;
    }
  } else if(_multiParseState == EXPECT_DASH2){
    if(data != '-'){
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); itemWriteByte('\n'); itemWriteByte('-');  _parseMultipartPostByte(data, last);
    } else {
      _multiParseState = BOUNDARY_OR_DATA;
      _boundaryPosition = 0;
    }
  } else if(_multiParseState == BOUNDARY_OR_DATA){
    if(_boundaryPosition < _boundary.length() && _boundary.c_str()[_boundaryPosition] != data){
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); itemWriteByte('\n'); itemWriteByte('-');  itemWriteByte('-');
      size_t i;
      for(i=0; i<_boundaryPosition; i++)
        itemWriteByte(_boundary.c_str()[i]);
      _parseMultipartPostByte(data, last);
    } else if(_boundaryPosition == _boundary.length() - 1){
      _multiParseState = DASH3_OR_RETURN2;
      if(!_itemIsFile){
        _addParam(new AsyncWebParameter(_itemName, _itemValue, true));
      } else {
        if(_itemSize){
          if(_handler) _handler->handleUpload(this, _itemFilename, _itemSize - _itemBufferIndex, _itemBuffer, _itemBufferIndex, true);
          _itemBufferIndex = 0;
          _addParam(new AsyncWebParameter(_itemName, _itemFilename, true, true, _itemSize));
        }
        free(_itemBuffer);
        _itemBuffer = NULL;
      }

    } else {
      _boundaryPosition++;
    }
  } else if(_multiParseState == DASH3_OR_RETURN2){
    if(data == '-' && (_contentLength - _parsedLength - 4) != 0){
      //os_printf("ERROR: The parser got to the end of the POST but is expecting %u bytes more!\nDrop an issue so we can have more info on the matter!\n", _contentLength - _parsedLength - 4);
      _contentLength = _parsedLength + 4;//lets close the request gracefully
    }
    if(data == '\r'){
      _multiParseState = EXPECT_FEED2;
    } else if(data == '-' && _contentLength == (_parsedLength + 4)){
      _multiParseState = PARSING_FINISHED;
    } else {
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); itemWriteByte('\n'); itemWriteByte('-');  itemWriteByte('-');
      size_t i; for(i=0; i<_boundary.length(); i++) itemWriteByte(_boundary.c_str()[i]);
      _parseMultipartPostByte(data, last);
    }
  } else if(_multiParseState == EXPECT_FEED2){
    if(data == '\n'){
      _multiParseState = PARSE_HEADERS;
      _itemIsFile = false;
    } else {
      _multiParseState = WAIT_FOR_RETURN1;
      itemWriteByte('\r'); itemWriteByte('\n'); itemWriteByte('-');  itemWriteByte('-');
      size_t i; for(i=0; i<_boundary.length(); i++) itemWriteByte(_boundary.c_str()[i]);
      itemWriteByte('\r'); _parseMultipartPostByte(data, last);
    }
  }
}

void AsyncWebServerRequest::_parseLine(){
  if(_parseState == PARSE_REQ_START){
    if(!_temp.length()){
      _parseState = PARSE_REQ_FAIL;
      _client->close();
    } else {
      _parseReqHead();
      _parseState = PARSE_REQ_HEADERS;
    }
    return;
  }

  if(_parseState == PARSE_REQ_HEADERS){
    if(!_temp.length()){
      //end of headers
      // #7: RFC 2046 limits the boundary to 1..70 characters, the parser keeps its position in 8 bit
      const bool badBoundary = _isMultipart && (!_boundary.length() || _boundary.length() > 70);
      if(badBoundary){
        _isMultipart = false;
      }
      // an unknown method gets no handler (HTTP_ANY would match every handler), it is answered with 501
      const bool dispatch = _method != HTTP_UNKNOWN && !badBoundary;
      if(dispatch){
        _server->_rewriteRequest(this);
        _server->_attachHandler(this);
      }
      _removeNotInterestingHeaders();
      if(badBoundary){
        send(400); // the body is read without a handler, the second response at its end is ignored
      }
      if(_expectingContinue && dispatch){
        String response = F("HTTP/1.1 100 Continue\r\n\r\n");
        _client->write(response.c_str(), response.length(), ASYNC_WRITE_FLAG_COPY);
      }
      //check handler for authentication
      if(_contentLength){
        _parseState = PARSE_REQ_BODY;
      } else {
        _parseState = PARSE_REQ_END;
        if(_handler) _handler->handleRequest(this);
        else send(501);
      }
    } else _parseReqHeader();
  }
}

size_t AsyncWebServerRequest::headers() const{
  return _headers.length();
}

bool AsyncWebServerRequest::hasHeader(const String& name) const {
  for(const auto& h: _headers){
    if(h->name().equalsIgnoreCase(name)){
      return true;
    }
  }
  return false;
}

bool AsyncWebServerRequest::hasHeader(const __FlashStringHelper * data) const {
  return getHeader(data) != nullptr;
}

AsyncWebHeader* AsyncWebServerRequest::getHeader(const String& name) const {
  for(const auto& h: _headers){
    if(h->name().equalsIgnoreCase(name)){
      return h;
    }
  }
  return nullptr;
}

AsyncWebHeader* AsyncWebServerRequest::getHeader(const __FlashStringHelper * data) const {
  for(const auto& h: _headers){
    if(strcasecmp_P(h->name().c_str(), (PGM_P)data) == 0){
      return h;
    }
  }
  return nullptr;
}

AsyncWebHeader* AsyncWebServerRequest::getHeader(size_t num) const {
  auto header = _headers.nth(num);
  return header ? *header : nullptr;
}

size_t AsyncWebServerRequest::params() const {
  return _params.length();
}

bool AsyncWebServerRequest::hasParam(const String& name, bool post, bool file) const {
  for(const auto& p: _params){
    if(p->name() == name && p->isPost() == post && p->isFile() == file){
      return true;
    }
  }
  return false;
}

bool AsyncWebServerRequest::hasParam(const __FlashStringHelper * data, bool post, bool file) const {
  return getParam(data, post, file) != nullptr;
}

AsyncWebParameter* AsyncWebServerRequest::getParam(const String& name, bool post, bool file) const {
  for(const auto& p: _params){
    if(p->name() == name && p->isPost() == post && p->isFile() == file){
      return p;
    }
  }
  return nullptr;
}

AsyncWebParameter* AsyncWebServerRequest::getParam(const __FlashStringHelper * data, bool post, bool file) const {
  for(const auto& p: _params){
    if(p->isPost() == post && p->isFile() == file && strcmp_P(p->name().c_str(), (PGM_P)data) == 0){
      return p;
    }
  }
  return nullptr;
}

AsyncWebParameter* AsyncWebServerRequest::getParam(size_t num) const {
  auto param = _params.nth(num);
  return param ? *param : nullptr;
}

void AsyncWebServerRequest::addInterestingHeader(const String& name){
  if(!_interestingHeaders.containsIgnoreCase(name))
    _interestingHeaders.add(name);
}

void AsyncWebServerRequest::send(AsyncWebServerResponse *response){
  if(_response){
    // a response was already sent: replacing it would leak it and send a second response
    delete response;
    return;
  }
  _response = response;
  if(_response == NULL){
    // closing here deletes the request (close(true) calls the disconnect callback) while the
    // caller still uses it, the next poll closes the connection
    _client->setRxTimeout(1);
    return;
  }
  if(!_response->_sourceValid()){
    delete response;
    _response = NULL;
    send(500);
  }
  else {
    _client->setRxTimeout(0);
    _response->_respond(this);
  }
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(int code, const String& contentType, const String& content){
  return new AsyncBasicResponse(code, contentType, content);
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(int code, const String& contentType, String&& content){
  return new AsyncBasicResponse(code, contentType, std::move(content));
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(FS &fs, const String& path, const String& contentType, bool download){
  if(fs.exists(path) || (!download && fs.exists(path + F(".gz"))))
    return new AsyncFileResponse(fs, path, contentType, download);
  return NULL;
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(File content, const String& path, const String& contentType, bool download){
  if(content == true)
    return new AsyncFileResponse(content, path, contentType, download);
  return NULL;
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(Stream &stream, const String& contentType, size_t len){
  return new AsyncStreamResponse(stream, contentType, len);
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse(const String& contentType, size_t len, AwsResponseFiller callback){
  return new AsyncCallbackResponse(contentType, len, callback);
}

AsyncWebServerResponse * AsyncWebServerRequest::beginChunkedResponse(const String& contentType, AwsResponseFiller callback){
  if(_version)
    return new AsyncChunkedResponse(contentType, callback);
  return new AsyncCallbackResponse(contentType, 0, callback);
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse_P(int code, const String& contentType, const uint8_t * content, size_t len){
  return new AsyncProgmemResponse(code, contentType, content, len);
}

AsyncWebServerResponse * AsyncWebServerRequest::beginResponse_P(int code, const String& contentType, PGM_P content){
  return beginResponse_P(code, contentType, (const uint8_t *)content, strlen_P(content));
}

void AsyncWebServerRequest::send(int code, const String& contentType, const String& content){
  send(beginResponse(code, contentType, content));
}

void AsyncWebServerRequest::send(int code, const String& contentType, String&& content){
  send(beginResponse(code, contentType, std::move(content)));
}

void AsyncWebServerRequest::send(FS &fs, const String& path, const String& contentType, bool download){
  if(fs.exists(path) || (!download && fs.exists(path + F(".gz")))){
    send(beginResponse(fs, path, contentType, download));
  } else send(404);
}

void AsyncWebServerRequest::send(File content, const String& path, const String& contentType, bool download){
  if(content == true){
    send(beginResponse(content, path, contentType, download));
  } else send(404);
}

void AsyncWebServerRequest::send(Stream &stream, const String& contentType, size_t len){
  send(beginResponse(stream, contentType, len));
}

void AsyncWebServerRequest::send(const String& contentType, size_t len, AwsResponseFiller callback){
  send(beginResponse(contentType, len, callback));
}

void AsyncWebServerRequest::sendChunked(const String& contentType, AwsResponseFiller callback){
  send(beginChunkedResponse(contentType, callback));
}

void AsyncWebServerRequest::send_P(int code, const String& contentType, const uint8_t * content, size_t len){
  send(beginResponse_P(code, contentType, content, len));
}

void AsyncWebServerRequest::send_P(int code, const String& contentType, PGM_P content){
  send(beginResponse_P(code, contentType, content));
}

void AsyncWebServerRequest::redirect(const String& url){
  AsyncWebServerResponse * response = beginResponse(302);
  response->addHeader(F("Location"),url);
  send(response);
}

bool AsyncWebServerRequest::hasArg(const char* name) const {
  for(const auto& arg: _params){
    if(arg->name() == name){
      return true;
    }
  }
  return false;
}

bool AsyncWebServerRequest::hasArg(const __FlashStringHelper * data) const {
  for(const auto& arg: _params){
    if(strcmp_P(arg->name().c_str(), (PGM_P)data) == 0){
      return true;
    }
  }
  return false;
}

const String& AsyncWebServerRequest::arg(const String& name) const {
  for(const auto& arg: _params){
    if(arg->name() == name){
      return arg->value();
    }
  }
  return SharedEmptyString;
}

const String& AsyncWebServerRequest::arg(const __FlashStringHelper * data) const {
  for(const auto& arg: _params){
    if(strcmp_P(arg->name().c_str(), (PGM_P)data) == 0){
      return arg->value();
    }
  }
  return SharedEmptyString;
}

const String& AsyncWebServerRequest::arg(size_t i) const {
  auto param = getParam(i);
  return param ? param->value() : SharedEmptyString;
}

const String& AsyncWebServerRequest::argName(size_t i) const {
  auto param = getParam(i);
  return param ? param->name() : SharedEmptyString;
}

const String& AsyncWebServerRequest::pathArg(size_t i) const {
  auto param = _pathParams.nth(i);
  return param ? **param : SharedEmptyString;
}


const String& AsyncWebServerRequest::header(const char* name) const {
  AsyncWebHeader* h = nullptr;
  for(const auto& item: _headers){
    if(strcasecmp(item->name().c_str(), name) == 0){
      h = item;
      break;
    }
  }
  return h ? h->value() : SharedEmptyString;
}

const String& AsyncWebServerRequest::header(const __FlashStringHelper * data) const {
  AsyncWebHeader* h = getHeader(data);
  return h ? h->value() : SharedEmptyString;
};


const String& AsyncWebServerRequest::header(size_t i) const {
  AsyncWebHeader* h = getHeader(i);
  return h ?  h->value() : SharedEmptyString;
}

const String& AsyncWebServerRequest::headerName(size_t i) const {
  AsyncWebHeader* h = getHeader(i);
  return h ? h->name() : SharedEmptyString;
}

String AsyncWebServerRequest::urlDecode(const String& text) const {
  char temp[] = "0x00";
  unsigned int len = text.length();
  unsigned int i = 0;
  String decoded = String();
  decoded.reserve(len); // Allocate the string internal buffer - never longer from source text
  while (i < len){
    char decodedChar;
    char encodedChar = text.charAt(i++);
    // an invalid escape ("%zz", "%" at the end) is kept as it is
    if ((encodedChar == '%') && (i + 1 < len) && isxdigit(text.charAt(i)) && isxdigit(text.charAt(i + 1))){
      temp[2] = text.charAt(i++);
      temp[3] = text.charAt(i++);
      decodedChar = strtol(temp, NULL, 16);
      if (!decodedChar) {
        continue; // %00 would truncate the string in c_str()
      }
    } else if (encodedChar == '+') {
      decodedChar = ' ';
    } else {
      decodedChar = encodedChar;  // normal ascii char
    }
    decoded.concat(decodedChar);
  }
  return decoded;
}


const __FlashStringHelper *AsyncWebServerRequest::methodToString() const
{
  if(_method == HTTP_ANY) return F("ANY");
  else if(_method & HTTP_GET) return F("GET");
  else if(_method & HTTP_POST) return F("POST");
  else if(_method & HTTP_DELETE) return F("DELETE");
  else if(_method & HTTP_PUT) return F("PUT");
  else if(_method & HTTP_PATCH) return F("PATCH");
  else if(_method & HTTP_HEAD) return F("HEAD");
  else if(_method & HTTP_OPTIONS) return F("OPTIONS");
  return F("UNKNOWN");
}

const __FlashStringHelper *AsyncWebServerRequest::requestedConnTypeToString() const {
  switch (_reqconntype) {
    case RCT_NOT_USED: return F("RCT_NOT_USED");
    case RCT_DEFAULT:  return F("RCT_DEFAULT");
    case RCT_HTTP:     return F("RCT_HTTP");
    case RCT_WS:       return F("RCT_WS");
    case RCT_EVENT:    return F("RCT_EVENT");
    default:           return F("ERROR");
  }
}

bool AsyncWebServerRequest::isExpectedRequestedConnType(RequestedConnectionType erct1, RequestedConnectionType erct2, RequestedConnectionType erct3) {
    bool res = false;
    if ((erct1 != RCT_NOT_USED) && (erct1 == _reqconntype)) res = true;
    if ((erct2 != RCT_NOT_USED) && (erct2 == _reqconntype)) res = true;
    if ((erct3 != RCT_NOT_USED) && (erct3 == _reqconntype)) res = true;
    return res;
}
