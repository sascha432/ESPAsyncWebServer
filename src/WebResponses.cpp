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
#include "WebResponseImpl.h"

/*
 * Abstract Response
 * */
PGM_P AsyncWebServerResponse::_responseCodeToString(int code) {
  switch (code) {
    case 100: return PSTR("Continue");
    case 101: return PSTR("Switching Protocols");
    case 200: return PSTR("OK");
    case 201: return PSTR("Created");
    case 202: return PSTR("Accepted");
    case 203: return PSTR("Non-Authoritative Information");
    case 204: return PSTR("No Content");
    case 205: return PSTR("Reset Content");
    case 206: return PSTR("Partial Content");
    case 300: return PSTR("Multiple Choices");
    case 301: return PSTR("Moved Permanently");
    case 302: return PSTR("Found");
    case 303: return PSTR("See Other");
    case 304: return PSTR("Not Modified");
    case 305: return PSTR("Use Proxy");
    case 307: return PSTR("Temporary Redirect");
    case 400: return PSTR("Bad Request");
    case 401: return PSTR("Unauthorized");
    case 402: return PSTR("Payment Required");
    case 403: return PSTR("Forbidden");
    case 404: return PSTR("Not Found");
    case 405: return PSTR("Method Not Allowed");
    case 406: return PSTR("Not Acceptable");
    case 407: return PSTR("Proxy Authentication Required");
    case 408: return PSTR("Request Time-out");
    case 409: return PSTR("Conflict");
    case 410: return PSTR("Gone");
    case 411: return PSTR("Length Required");
    case 412: return PSTR("Precondition Failed");
    case 413: return PSTR("Request Entity Too Large");
    case 414: return PSTR("Request-URI Too Large");
    case 415: return PSTR("Unsupported Media Type");
    case 416: return PSTR("Requested range not satisfiable");
    case 417: return PSTR("Expectation Failed");
    case 500: return PSTR("Internal Server Error");
    case 501: return PSTR("Not Implemented");
    case 502: return PSTR("Bad Gateway");
    case 503: return PSTR("Service Unavailable");
    case 504: return PSTR("Gateway Time-out");
    case 505: return PSTR("HTTP Version not supported");
    default:  return PSTR("");
  }
}

AsyncWebServerResponse::AsyncWebServerResponse()
  : _code(0)
  , _headers(LinkedList<AsyncWebHeader *>([](AsyncWebHeader *h){ delete h; }))
  , _contentType()
  , _contentLength(0)
  , _sendContentLength(true)
  , _chunked(false)
  , _headLength(0)
  , _sentLength(0)
  , _ackedLength(0)
  , _writtenLength(0)
  , _state(RESPONSE_SETUP)
{
  for(auto header: DefaultHeaders::Instance()) {
    _headers.add(new AsyncWebHeader(header->name(), header->value()));
  }
}

AsyncWebServerResponse::~AsyncWebServerResponse(){
  _headers.free();
}

void AsyncWebServerResponse::setCode(int code){
  if(_state == RESPONSE_SETUP)
    _code = code;
}

void AsyncWebServerResponse::setContentLength(size_t len){
  if(_state == RESPONSE_SETUP)
    _contentLength = len;
}

void AsyncWebServerResponse::setContentType(const String& type){
  if(_state == RESPONSE_SETUP)
    _contentType = type;
}

void AsyncWebServerResponse::addHeader(const String& name, const String& value){
  _headers.add(new AsyncWebHeader(name, value));
}

String AsyncWebServerResponse::_assembleHead(uint8_t version){
  if(version){
    addHeader(F("Accept-Ranges"), F("none"));
    if(_chunked)
      addHeader(F("Transfer-Encoding"), F("chunked"));
  }
  // built without a stack buffer: a header of any length is sent completely
  PGM_P codeStr = _responseCodeToString(_code);
  size_t len = 9 + 4 + strlen_P(codeStr) + 2 + 2; // "HTTP/1.x " "nnn " reason CRLF ... CRLF
  if(_sendContentLength)
    len += 16 + 10 + 2;
  if(_contentType.length())
    len += 14 + _contentType.length() + 2;
  for(const auto& header: _headers)
    len += header->name().length() + 2 + header->value().length() + 2;

  String out;
  out.reserve(len);
  out += F("HTTP/1.");
  out += static_cast<unsigned>(version);
  out += ' ';
  out += _code;
  out += ' ';
  out += FPSTR(codeStr);
  out += F("\r\n");

  if(_sendContentLength) {
    out += F("Content-Length: ");
    out += static_cast<unsigned>(_contentLength);
    out += F("\r\n");
  }
  if(_contentType.length()) {
    out += F("Content-Type: ");
    out += _contentType;
    out += F("\r\n");
  }

  for(const auto& header: _headers){
    out += header->name();
    out += F(": ");
    out += header->value();
    out += F("\r\n");
  }
  _headers.free();

  out.concat(F("\r\n"));
  _headLength = out.length();
  return out;
}

bool AsyncWebServerResponse::_started() const { return _state > RESPONSE_SETUP; }
bool AsyncWebServerResponse::_finished() const { return _state > RESPONSE_WAIT_ACK; }
bool AsyncWebServerResponse::_failed() const { return _state == RESPONSE_FAILED; }
bool AsyncWebServerResponse::_sourceValid() const { return false; }
void AsyncWebServerResponse::_respond(AsyncWebServerRequest *request){ _state = RESPONSE_END; request->client()->close(); }
size_t AsyncWebServerResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){ (void)request; (void)len; (void)time; return 0; }

/*
 * String/Code Response
 * */
AsyncBasicResponse::AsyncBasicResponse(int code, const String& contentType, const String& content){
  _content = content;
  _init(code, contentType);
}

AsyncBasicResponse::AsyncBasicResponse(int code, const String& contentType, String&& content){
  _content = std::move(content);
  _init(code, contentType);
}

void AsyncBasicResponse::_init(int code, const String& contentType){
  _code = code;
  _contentType = contentType;
  if(_content.length()){
    _contentLength = _content.length();
    if(!_contentType.length()) {
      _contentType = F("text/plain");
    }
  }
  addHeader(F("Connection"), F("close"));
}

// The head and the content are handed to the socket separately and the content is sent from an
// offset: no copy of the content (out += _content) and no substring() per ack. The stack copies the
// data (ASYNC_WRITE_FLAG_COPY, the ESP8266 default is a reference), the strings can be released
size_t AsyncBasicResponse::_send(AsyncWebServerRequest *request){
  auto client = request->client();
  size_t space = client->space();
  size_t written = 0;
  if(_head.length() && space){
    size_t added = client->add(_head.c_str(), std::min<size_t>(space, _head.length()), ASYNC_WRITE_FLAG_COPY);
    _head.remove(0, added);
    space -= added;
    written += added;
  }
  if(!_head.length() && _sentLength < _contentLength && space){
    size_t added = client->add(_content.c_str() + _sentLength, std::min<size_t>(space, _contentLength - _sentLength), ASYNC_WRITE_FLAG_COPY);
    _sentLength += added;
    written += added;
  }
  if(written){
    client->send();
    _writtenLength += written;
  }
  if(!_head.length() && _sentLength >= _contentLength){
    _content = String(); // everything was copied into the socket
    _state = RESPONSE_WAIT_ACK;
  }
  return written;
}

void AsyncBasicResponse::_respond(AsyncWebServerRequest *request){
  _head = _assembleHead(request->version());
  _state = RESPONSE_CONTENT;
  _send(request);
}

size_t AsyncBasicResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  _ackedLength += len;
  if(_state == RESPONSE_CONTENT){
    return _send(request);
  } else if(_state == RESPONSE_WAIT_ACK){
    if(_ackedLength >= _writtenLength){
      _state = RESPONSE_END;
      request->client()->setRxTimeout(ASYNCWEBSERVER_RESPONSE_END_RX_TIMEOUT);
    }
  }
  return 0;
}


/*
 * Abstract Response
 * */

AsyncAbstractResponse::AsyncAbstractResponse()
{
}

void AsyncAbstractResponse::_respond(AsyncWebServerRequest *request){
  addHeader(F("Connection"), F("close"));
  _head = _assembleHead(request->version());
  _state = RESPONSE_HEADERS;
  _ack(request, 0, 0);
}

size_t AsyncAbstractResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  if(!_sourceValid()){
    _state = RESPONSE_FAILED;
    request->client()->close();
    return 0;
  }
  _ackedLength += len;
  size_t space = request->client()->space();

  size_t headLen = _head.length();
  if(_state == RESPONSE_HEADERS){
    if(space >= headLen){
      _state = RESPONSE_CONTENT;
      space -= headLen;
    } else {
      String out = _head.substring(0, space);
      _head = _head.substring(space);
      _writtenLength += request->client()->write(out.c_str(), out.length(), ASYNC_WRITE_FLAG_COPY);
      return out.length();
    }
  }

  if(_state == RESPONSE_CONTENT){
    size_t outLen;
    if(_chunked){
      if(space <= 8){
        return 0;
      }
      outLen = space;
    } else if(!_sendContentLength){
      outLen = space;
    } else {
      outLen = ((_contentLength - _sentLength) > space)?space:(_contentLength - _sentLength);
    }

    uint8_t *buf = (uint8_t *)malloc(outLen+headLen);
    if (!buf && outLen > 512) {
      // low heap: a smaller block keeps the response going, without a buffer it stalls
      outLen = 512;
      buf = (uint8_t *)malloc(outLen+headLen);
    }
    if (!buf) {
      return 0;
    }

    if(headLen){
      memcpy(buf, _head.c_str(), _head.length());
    }

    size_t readLen = 0;

    if(_chunked){
      // HTTP 1.1 allows leading zeros in chunk length. Or spaces may be added.
      // See RFC2616 sections 2, 3.6.1.
      readLen = _fillBuffer(buf+headLen+6, outLen - 8);
      if(readLen == RESPONSE_TRY_AGAIN){
          free(buf);
          return 0;
      }
      outLen = sprintf_P((char*)buf+headLen, PSTR("%x"), readLen) + headLen;
      while(outLen < headLen + 4) buf[outLen++] = ' ';
      buf[outLen++] = '\r';
      buf[outLen++] = '\n';
      outLen += readLen;
      buf[outLen++] = '\r';
      buf[outLen++] = '\n';
    } else {
      readLen = _fillBuffer(buf+headLen, outLen);
      if(readLen == RESPONSE_TRY_AGAIN){
          free(buf);
          return 0;
      }
      outLen = readLen + headLen;
    }

    if(headLen){
        _head = String();
    }

    if(outLen){
        // the buffer is released below, the stack needs its own copy
        _writtenLength += request->client()->write((const char*)buf, outLen, ASYNC_WRITE_FLAG_COPY);
    }

    if(_chunked){
        _sentLength += readLen;
    } else {
        _sentLength += outLen - headLen;
    }

    free(buf);

    if((_chunked && readLen == 0) || (!_sendContentLength && outLen == 0) || (!_chunked && _sentLength == _contentLength)){
      _state = RESPONSE_WAIT_ACK;
    }
    return outLen;

  } else if(_state == RESPONSE_WAIT_ACK){
    // the response ends when everything written was acked: closing earlier (an RST with
    // close(true)) drops the data that is still in flight
    if(_ackedLength >= _writtenLength){
      _state = RESPONSE_END;
      request->client()->setRxTimeout(ASYNCWEBSERVER_RESPONSE_END_RX_TIMEOUT);
      // without content length and chunks the close marks the end of the body; the disconnect
      // handler deletes the request and this response, nothing may be touched afterwards
      if(!_chunked && !_sendContentLength)
        request->client()->close();
    }
  }
  return 0;
}

/*
 * File Response
 * */

const char ICACHE_FLASH_ATTR __attribute__((aligned(4))) *AsyncWeBServerFileOpenReadMode = "r";

AsyncFileResponse::~AsyncFileResponse(){
  if(_content)
    _content.close();
}

void AsyncFileResponse::_setContentType(const String& path){
  if (path.endsWith(F(".html"))) _contentType = F("text/html");
  else if (path.endsWith(F(".htm"))) _contentType = F("text/html");
  else if (path.endsWith(F(".css"))) _contentType = F("text/css");
  else if (path.endsWith(F(".json"))) _contentType = F("application/json");
  else if (path.endsWith(F(".js"))) _contentType = F("application/javascript");
  else if (path.endsWith(F(".png"))) _contentType = F("image/png");
  else if (path.endsWith(F(".gif"))) _contentType = F("image/gif");
  else if (path.endsWith(F(".jpg"))) _contentType = F("image/jpeg");
  else if (path.endsWith(F(".ico"))) _contentType = F("image/x-icon");
  else if (path.endsWith(F(".svg"))) _contentType = F("image/svg+xml");
  else if (path.endsWith(F(".eot"))) _contentType = F("font/eot");
  else if (path.endsWith(F(".woff"))) _contentType = F("font/woff");
  else if (path.endsWith(F(".woff2"))) _contentType = F("font/woff2");
  else if (path.endsWith(F(".ttf"))) _contentType = F("font/ttf");
  else if (path.endsWith(F(".xml"))) _contentType = F("text/xml");
  else if (path.endsWith(F(".pdf"))) _contentType = F("application/pdf");
  else if (path.endsWith(F(".zip"))) _contentType = F("application/zip");
  else if(path.endsWith(F(".gz"))) _contentType = F("application/x-gzip");
  else _contentType = F("text/plain");
}

// "attachment|inline; filename="<name>"" for the last path component: '"' and '\' are escaped,
// control characters (CR/LF from an encoded URL) are dropped
static String _contentDisposition(const String& path, bool download){
  const char *filename = path.c_str() + (path.lastIndexOf('/') + 1);
  String out;
  out.reserve(24 + strlen(filename));
  out += download ? F("attachment; filename=\"") : F("inline; filename=\"");
  for(; *filename; filename++){
    char c = *filename;
    if((uint8_t)c < 0x20 || c == 0x7f)
      continue;
    if(c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

AsyncFileResponse::AsyncFileResponse(FS &fs, const String& path, const String& contentType, bool download): AsyncAbstractResponse(){
  _code = 200;
  _path = path;

  if(!download && !fs.exists(_path) && fs.exists(_path + F(".gz"))){
    _path = _path + F(".gz");
    addHeader(F("Content-Encoding"), F("gzip"));
    _sendContentLength = true;
    _chunked = false;
  }

  _content = fs.open(_path, AsyncWeBServerFileOpenReadMode);
  _contentLength = _content.size();

  if(contentType.length() == 0)
    _setContentType(path);
  else
    _contentType = contentType;

  addHeader(F("Content-Disposition"), _contentDisposition(path, download));
}

AsyncFileResponse::AsyncFileResponse(File content, const String& path, const String& contentType, bool download): AsyncAbstractResponse(){
  _code = 200;
  _path = path;

  if(!download && String(content.name()).endsWith(F(".gz")) && !path.endsWith(F(".gz"))){
    addHeader(F("Content-Encoding"), F("gzip"));
    _sendContentLength = true;
    _chunked = false;
  }

  _content = content;
  _contentLength = _content.size();

  if(contentType.length() == 0)
    _setContentType(path);
  else
    _contentType = contentType;

  addHeader(F("Content-Disposition"), _contentDisposition(path, download));
}

size_t AsyncFileResponse::_fillBuffer(uint8_t *data, size_t len){
  return _content.read(data, len);
}

/*
 * Stream Response
 * */

AsyncStreamResponse::AsyncStreamResponse(Stream &stream, const String& contentType, size_t len): AsyncAbstractResponse() {
  _code = 200;
  _content = &stream;
  _contentLength = len;
  _contentType = contentType;
}

size_t AsyncStreamResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t available = _content->available();
  size_t outLen = (available > len)?len:available;
  size_t i;
  for(i=0;i<outLen;i++)
    data[i] = _content->read();
  return outLen;
}

/*
 * Callback Response
 * */

AsyncCallbackResponse::AsyncCallbackResponse(const String& contentType, size_t len, AwsResponseFiller callback): AsyncAbstractResponse() {
  _code = 200;
  _content = callback;
  _contentLength = len;
  if(!len)
    _sendContentLength = false;
  _contentType = contentType;
  _filledLength = 0;
}

size_t AsyncCallbackResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t ret = _content(data, len, _filledLength);
  if(ret != RESPONSE_TRY_AGAIN){
      _filledLength += ret;
  }
  return ret;
}

/*
 * Chunked Response
 * */

AsyncChunkedResponse::AsyncChunkedResponse(const String& contentType, AwsResponseFiller callback): AsyncAbstractResponse() {
  _code = 200;
  _content = callback;
  _contentLength = 0;
  _contentType = contentType;
  _sendContentLength = false;
  _chunked = true;
  _filledLength = 0;
}

size_t AsyncChunkedResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t ret = _content(data, len, _filledLength);
  if(ret != RESPONSE_TRY_AGAIN){
      _filledLength += ret;
  }
  return ret;
}

/*
 * Progmem Response
 * */

AsyncProgmemResponse::AsyncProgmemResponse(int code, const String& contentType, const uint8_t * content, size_t len): AsyncAbstractResponse() {
  _code = code;
  _content = content;
  _contentType = contentType;
  _contentLength = len;
  _readLength = 0;
}

size_t AsyncProgmemResponse::_fillBuffer(uint8_t *data, size_t len){
  size_t left = _contentLength - _readLength;
  if (left > len) {
    memcpy_P(data, _content + _readLength, len);
    _readLength += len;
    return len;
  }
  memcpy_P(data, _content + _readLength, left);
  _readLength += left;
  return left;
}
