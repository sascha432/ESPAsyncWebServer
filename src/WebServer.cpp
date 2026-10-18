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
#include "WebHandlerImpl.h"

bool ON_STA_FILTER(AsyncWebServerRequest *request) {
  return WiFi.localIP() == request->client()->localIP();
}

bool ON_AP_FILTER(AsyncWebServerRequest *request) {
  return WiFi.localIP() != request->client()->localIP();
}


AsyncWebServer::AsyncWebServer(uint16_t port)
  : _server(port)
  , _rewrites(LinkedList<AsyncWebRewrite*>([](AsyncWebRewrite* r){ delete r; }))
  , _handlers(LinkedList<AsyncWebHandler*>([](AsyncWebHandler* h){ delete h; }))
  , _requests(nullptr)
{
  _catchAllHandler = new AsyncCallbackWebHandler();
  if(_catchAllHandler == NULL)
    return;
  _server.onClient([](void *s, AsyncClient* c){
    if(c == NULL)
      return;
    c->setRxTimeout(3);
    AsyncWebServerRequest *r = new (std::nothrow) AsyncWebServerRequest((AsyncWebServer*)s, c);
    if(r == NULL){
      c->close(true);
      c->free();
      delete c;
    }
  }, this);
}

AsyncWebServer::~AsyncWebServer(){
  end();
  // the requests point to this server and to the handlers, they are closed before reset() deletes them
  _closeRequests();
  reset();
  if(_catchAllHandler) delete _catchAllHandler;
}

void AsyncWebServer::_addRequest(AsyncWebServerRequest *request){
  AsyncWebLockGuard l(_requestsLock);
  request->_nextRequest = _requests;
  _requests = request;
}

void AsyncWebServer::_removeRequest(AsyncWebServerRequest *request){
  AsyncWebLockGuard l(_requestsLock);
  for(auto ptr = &_requests; *ptr; ptr = &(*ptr)->_nextRequest){
    if(*ptr == request){
      *ptr = request->_nextRequest;
      break;
    }
  }
  request->_nextRequest = nullptr;
}

void AsyncWebServer::_closeRequests(){
  AsyncWebServerRequest *list;
  {
    AsyncWebLockGuard l(_requestsLock);
    list = _requests;
    _requests = nullptr;
    // detached: the requests do not remove themselves from the list any more
    for(auto request = list; request; request = request->_nextRequest){
      request->_server = nullptr;
      request->_handler = nullptr;
    }
  }
  while(list){
    auto request = list;
    list = request->_nextRequest;
    request->_nextRequest = nullptr;
    if(request->_client){
      _closeDetached(request->_client);
      request->_client = nullptr;
    }
    request->_onDisconnect(); // calls the disconnect handler and deletes the request
  }
}

void AsyncWebServer::_closeDetached(AsyncClient *client){
  client->onError(nullptr, nullptr);
  client->onAck(nullptr, nullptr);
  client->onTimeout(nullptr, nullptr);
  client->onData(nullptr, nullptr);
  client->onPoll(nullptr, nullptr);
  client->onDisconnect([](void *, AsyncClient *c){ delete c; }, nullptr);
  client->close(true);
}

AsyncWebRewrite& AsyncWebServer::addRewrite(AsyncWebRewrite* rewrite){
  _rewrites.add(rewrite);
  return *rewrite;
}

bool AsyncWebServer::removeRewrite(AsyncWebRewrite *rewrite){
  return _rewrites.remove(rewrite);
}

AsyncWebRewrite& AsyncWebServer::rewrite(const char* from, const char* to){
  return addRewrite(new AsyncWebRewrite(from, to));
}

AsyncWebHandler& AsyncWebServer::addHandler(AsyncWebHandler* handler){
  _handlers.add(handler);
  return *handler;
}

bool AsyncWebServer::removeHandler(AsyncWebHandler *handler){
  return _handlers.remove(handler);
}

void AsyncWebServer::begin(){
  _server.setNoDelay(true);
  _server.begin();
}

void AsyncWebServer::end(){
  _server.end();
}

#if ASYNC_TCP_SSL_ENABLED
void AsyncWebServer::onSslFileRequest(AcSSlFileHandler cb, void* arg){
  _server.onSslFileRequest(cb, arg);
}

void AsyncWebServer::beginSecure(const char *cert, const char *key, const char *password){
  _server.beginSecure(cert, key, password);
}
#endif

void AsyncWebServer::_rewriteRequest(AsyncWebServerRequest *request){
  for(const auto& r: _rewrites){
    if (r->match(request)){
      request->_url = r->toUrl();
      request->_addGetParams(r->params());
    }
  }
}

void AsyncWebServer::_attachHandler(AsyncWebServerRequest *request){
  for(const auto& h: _handlers){
    if (h->filter(request) && h->canHandle(request)){
      request->setHandler(h);
      return;
    }
  }
  
  request->addInterestingHeader(F("ANY"));
  request->setHandler(_catchAllHandler);
}


AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(uri);
  handler->setMethod(method);
  handler->onRequest(onRequest);
  handler->onUpload(onUpload);
  handler->onBody(onBody);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(uri);
  handler->setMethod(method);
  handler->onRequest(onRequest);
  handler->onUpload(onUpload);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(uri);
  handler->setMethod(method);
  handler->onRequest(onRequest);
  addHandler(handler);
  return *handler;
}

AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, ArRequestHandlerFunction onRequest){
  AsyncCallbackWebHandler* handler = new AsyncCallbackWebHandler();
  handler->setUri(uri);
  handler->onRequest(onRequest);
  addHandler(handler);
  return *handler;
}

void AsyncWebServer::onNotFound(ArRequestHandlerFunction fn){
  _catchAllHandler->onRequest(fn);
}

void AsyncWebServer::onFileUpload(ArUploadHandlerFunction fn){
  _catchAllHandler->onUpload(fn);
}

void AsyncWebServer::onRequestBody(ArBodyHandlerFunction fn){
  _catchAllHandler->onBody(fn);
}

void AsyncWebServer::reset(){
  _rewrites.free();
  _handlers.free();
  
  if (_catchAllHandler != NULL){
    _catchAllHandler->onRequest(NULL);
    _catchAllHandler->onUpload(NULL);
    _catchAllHandler->onBody(NULL);
  }
}

