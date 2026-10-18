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
#ifndef ASYNCWEBSERVERHANDLERIMPL_H_
#define ASYNCWEBSERVERHANDLERIMPL_H_

#include <Arduino_compat.h>
#include <string>
#ifdef ASYNCWEBSERVER_REGEX
#include <regex>
#endif

#include "stddef.h"
#include <time.h>

class AsyncCallbackWebHandler: public AsyncWebHandler {
  private:
  protected:
    String _uri;
    WebRequestMethodComposite _method;
    ArRequestHandlerFunction _onRequest;
    ArUploadHandlerFunction _onUpload;
    ArBodyHandlerFunction _onBody;
    bool _isRegex;
  public:
    AsyncCallbackWebHandler() : _uri(), _method(HTTP_ANY), _onRequest(NULL), _onUpload(NULL), _onBody(NULL), _isRegex(false) {}
    void setUri(const String& uri){
      _uri = uri;
      _isRegex = StrView(uri).startsWith('^') && StrView(uri).endsWith('$');
    }
    void setMethod(WebRequestMethodComposite method){ _method = method; }
    void onRequest(ArRequestHandlerFunction fn){ _onRequest = fn; }
    void onUpload(ArUploadHandlerFunction fn){ _onUpload = fn; }
    void onBody(ArBodyHandlerFunction fn){ _onBody = fn; }

    virtual bool canHandle(AsyncWebServerRequest *request) override final{

      if(!_onRequest)
        return false;

      if(!(_method & request->method()))
        return false;

#ifdef ASYNCWEBSERVER_REGEX
      if (_isRegex) {
        std::regex pattern(_uri.c_str());
        std::smatch matches;
        std::string s(request->url().c_str());
        if(std::regex_search(s, matches, pattern)) {
          for (size_t i = 1; i < matches.size(); ++i) { // start from 1
            request->_addPathParam(matches[i].str().c_str());
          }
        } else {
          return false;
        }
      } else
#endif
      // compared in place, no temporary Strings per handler and request
      const String &url = request->url();
      if (strncmp_P(_uri.c_str(), PSTR("/*."), 3) == 0) {
        // "/*.ext": the URL ends with ".ext"
        const char *ext = _uri.c_str() + _uri.lastIndexOf('.');
        size_t extLen = strlen(ext);
        if (url.length() < extLen || strcmp(url.c_str() + url.length() - extLen, ext) != 0)
          return false;
      }
      else if (StrView(_uri).endsWith('*')) {
        // "/prefix*": the URL starts with "/prefix"
        if (strncmp(url.c_str(), _uri.c_str(), _uri.length() - 1) != 0)
          return false;
      }
      else if (_uri.length()) {
        // the URI itself or a path below it ("/uri/...")
        size_t len = _uri.length();
        if (!(url == _uri) && !(url.length() > len && url.c_str()[len] == '/' && strncmp(url.c_str(), _uri.c_str(), len) == 0))
          return false;
      }

      request->addInterestingHeader(F("ANY"));
      return true;
    }

    virtual void handleRequest(AsyncWebServerRequest *request) override final {
      if(_onRequest)
        _onRequest(request);
      else
        request->send(500);
    }
    virtual void handleUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) override final {
      if(_onUpload)
        _onUpload(request, filename, index, data, len, final);
    }
    virtual void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override final {
      if(_onBody)
        _onBody(request, data, len, index, total);
    }
    virtual bool isRequestHandlerTrivial() override final {return _onRequest ? false : true;}
};

#endif /* ASYNCWEBSERVERHANDLERIMPL_H_ */
