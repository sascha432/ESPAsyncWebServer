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
#include "Arduino.h"
#include "AsyncWebSocket.h"

#include <libb64/cencode.h>

#ifndef ESP8266
#include "mbedtls/sha1.h"
#else
#include <Hash.h>
#endif

#undef printf_P

#define MAX_PRINTF_LEN 64

size_t webSocketSendFrameWindow(AsyncClient *client){
  if(!client->canSend())
    return 0;
  size_t space = client->space();
  if(space < 9)
    return 0;
  return space - 8;
}

size_t webSocketSendFrame(AsyncClient *client, bool final, uint8_t opcode, bool mask, uint8_t *data, size_t len){
  if(!client->canSend())
    return 0;
  size_t space = client->space();
  if(space < 2)
    return 0;
  uint8_t mbuf[4] = {0,0,0,0};
  uint8_t headLen = 2;
  if(len && mask){
    headLen += 4;
    mbuf[0] = rand() % 0xFF;
    mbuf[1] = rand() % 0xFF;
    mbuf[2] = rand() % 0xFF;
    mbuf[3] = rand() % 0xFF;
  }
  if(len > 125)
    headLen += 2;
  if(space < headLen)
    return 0;
  space -= headLen;

  if(len > space) len = space;

  uint8_t buf[8]; // 2 + 2 (extended length) + 4 (mask key)

  buf[0] = opcode & 0x0F;
  if(final)
    buf[0] |= 0x80;
  if(len < 126)
    buf[1] = len & 0x7F;
  else {
    buf[1] = 126;
    buf[2] = (uint8_t)((len >> 8) & 0xFF);
    buf[3] = (uint8_t)(len & 0xFF);
  }
  if(len && mask){
    buf[1] |= 0x80;
    memcpy(buf + (headLen - 4), mbuf, 4);
  }
  // the header is on the stack, the stack needs its own copy (the ESP8266 default is a reference)
  if(client->add((const char *)buf, headLen, ASYNC_WRITE_FLAG_COPY) != headLen){
    return 0;
  }

  if(len){
    if(len && mask){
      size_t i;
      for(i=0;i<len;i++)
        data[i] = data[i] ^ mbuf[i%4];
    }
    if(client->add((const char *)data, len, ASYNC_WRITE_FLAG_COPY) != len){
      //os_printf("error adding %lu data bytes\n", len);
      return 0;
    }
  }
  if(!client->send()){
    //os_printf("error sending frame: %lu\n", headLen+len);
    return 0;
  }
  return len;
}


/*
 *    AsyncWebSocketMessageBuffer
 */



AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer()
  :_data(nullptr)
  ,_len(0)
  ,_lock(false)
  ,_count(0)
{

}

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(const uint8_t * data, size_t size)
  :_data(nullptr)
  ,_len(size)
  ,_lock(false)
  ,_count(0)
{
  if (!data) {
    return;
  }
  _data = new (std::nothrow) uint8_t[_len + 1];
  if (_data) {
    memcpy_P(_data, data, _len);
    _data[_len] = 0;
  }
}


AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(String &&str)
  :_data(nullptr)
  ,_len(str.length())
  ,_lock(false)
  ,_count(0)
  ,_string(std::move(str))
{
  // the String is NUL terminated; an empty String may not have a buffer
  _data = reinterpret_cast<uint8_t *>(const_cast<char *>(_string.c_str()));
}

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(size_t size)
  :_data(nullptr)
  ,_len(size)
  ,_lock(false)
  ,_count(0)
{
  if (!size) {
    return;
  }
  _data = new (std::nothrow) uint8_t[_len + 1];
  if (_data) {
    _data[_len] = 0;
  }

}

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(const AsyncWebSocketMessageBuffer & copy)
  :_data(nullptr)
  ,_len(0)
  ,_lock(false)
  ,_count(0)
{
  _len = copy._len;
  _lock = copy._lock;
  _count = 0;

  if (!_len) {
    return;
  }

  _data = new (std::nothrow) uint8_t[_len + 1];
  if (_data) {
    memcpy(_data, copy._data, _len);
    _data[_len] = 0;
  }

}

AsyncWebSocketMessageBuffer::AsyncWebSocketMessageBuffer(AsyncWebSocketMessageBuffer && copy)
  :_data(nullptr)
  ,_len(0)
  ,_lock(false)
  ,_count(0)
{
  _len = copy._len;
  _lock = copy._lock;
  _count = 0;

  if (copy._data) {
    if (copy._ownsData()) {
      _data = copy._data;
    } else {
      // the String's buffer can be inline (SSO), get the pointer after the move
      _string = std::move(copy._string);
      _data = reinterpret_cast<uint8_t *>(const_cast<char *>(_string.c_str()));
    }
    copy._data = nullptr;
    copy._len = 0;
  }

}

AsyncWebSocketMessageBuffer::~AsyncWebSocketMessageBuffer()
{
    if (_ownsData()) {
      delete[] _data;
    }
}

bool AsyncWebSocketMessageBuffer::reserve(size_t size)
{
  _len = size;

  if (_ownsData()) {
    delete[] _data;
  }
  _string = String();
  _data = new (std::nothrow) uint8_t[_len + 1];
  if (_data) {
    _data[_len] = 0;
    return true;
  }
  return false;
}



/*
 * Control Frame
 */

class AsyncWebSocketControl {
  private:
    uint8_t _opcode;
    uint8_t *_data;
    size_t _len;
    bool _mask;
    bool _finished;
    size_t _acked;
  public:
    AsyncWebSocketControl(uint8_t opcode, const uint8_t *data = nullptr, size_t len = 0, bool mask = false) :
      _opcode(opcode),
      _data(nullptr),
      _len(0),
      _mask(len && mask),
      _finished(false),
      _acked(0)
  {
      if(data && len) {
        len = std::min<size_t>(len, 125);
        _data = new (std::nothrow) uint8_t[len];
        if(!_data) {
            return;
        }
        memcpy_P(_data, data, len);
        _len = len;
      }
    }
    virtual ~AsyncWebSocketControl(){
      if(_data) {
        delete[] _data;
      }
    }
    virtual bool finished() const { return _finished; }
    uint8_t opcode(){ return _opcode; }
    // size of the frame on the wire: header (+ mask key) and payload
    uint8_t len() const { return _len + (_mask ? 6 : 2); }
    // consumes the acknowledged bytes that belong to this frame, returns how many were used
    size_t ack(size_t count){
      size_t open = len() - _acked;
      if(count > open)
        count = open;
      _acked += count;
      return count;
    }
    bool acked() const { return _finished && _acked >= len(); }
    size_t send(AsyncClient *client){
      _finished = true;
      return webSocketSendFrame(client, true, _opcode & 0x0F, _mask, _data, _len);
    }
};

/*
 * Basic Buffered Message
 */


AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(const char * data, size_t len, uint8_t opcode, bool mask)
  :_len(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
  ,_data(nullptr)
{
  _opcode = opcode & 0x07;
  _mask = mask;
  _data = new (std::nothrow) uint8_t[len + 1];
  if (!_data) {
    _status = WS_MSG_ERROR;
    return;
  }
  _status = WS_MSG_SENDING;
  memcpy_P(_data, data, len);
  _data[len] = 0;
  _len = len;
}

AsyncWebSocketBasicMessage::AsyncWebSocketBasicMessage(uint8_t opcode, bool mask)
  :_len(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
  ,_data(nullptr)
{
  _opcode = opcode & 0x07;
  _mask = mask;
}

AsyncWebSocketBasicMessage::~AsyncWebSocketBasicMessage() {
  if (_data) {
    delete[] _data;
  }
}

 void AsyncWebSocketBasicMessage::ack(size_t len, uint32_t time)  {
   (void)time;
  _acked += len;
  if(_sent == _len && _acked == _ack){
    _status = WS_MSG_SENT;
  }
}

size_t AsyncWebSocketBasicMessage::send(AsyncClient *client)  {
  if(_status != WS_MSG_SENDING)
    return 0;
  if(_acked < _ack){
    return 0;
  }
  if(_sent == _len){
    if(_acked == _ack)
      _status = WS_MSG_SENT;
    return 0;
  }
  if(_sent > _len){
      _status = WS_MSG_ERROR;
      return 0;
  }

  size_t toSend = _len - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if(window < toSend) {
      toSend = window;
  }

  _sent += toSend;
  _ack += toSend + ((toSend < 126)?2:4) + (_mask * 4);

  bool final = (_sent == _len);
  uint8_t* dPtr = (uint8_t*)(_data + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend)?_opcode:(uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;
  if(toSend && sent != toSend){
      _sent -= (toSend - sent);
      _ack -= (toSend - sent);
  }
  return sent;
}

// bool AsyncWebSocketBasicMessage::reserve(size_t size) {
//   if (size) {
//     _data = (uint8_t*)malloc(size +1);
//     if (_data) {
//       memset(_data, 0, size);
//       _len = size;
//       _status = WS_MSG_SENDING;
//       return true;
//     }
//   }
//   return false;
//  }


/*
 * AsyncWebSocketMultiMessage Message
 */


AsyncWebSocketMultiMessage::AsyncWebSocketMultiMessage(AsyncWebSocketMessageBuffer * buffer, uint8_t opcode, bool mask)
  :_len(0)
  ,_sent(0)
  ,_ack(0)
  ,_acked(0)
  ,_WSbuffer(nullptr)
{

  _opcode = opcode & 0x07;
  _mask = mask;

  if (buffer) {
    _WSbuffer = buffer;
    (*_WSbuffer)++;
    _data = buffer->get();
    _len = buffer->length();
    _status = WS_MSG_SENDING;
    //ets_printf("M: %u\n", _len);
  } else {
    _status = WS_MSG_ERROR;
  }

}


AsyncWebSocketMultiMessage::~AsyncWebSocketMultiMessage() {
  if (_WSbuffer) {
    (*_WSbuffer)--; // decreases the counter.
  }
}

 void AsyncWebSocketMultiMessage::ack(size_t len, uint32_t time)  {
   (void)time;
  _acked += len;
  if(_sent >= _len && _acked >= _ack){
    _status = WS_MSG_SENT;
  }
  //ets_printf("A: %u\n", len);
}
 size_t AsyncWebSocketMultiMessage::send(AsyncClient *client)  {
  if(_status != WS_MSG_SENDING)
    return 0;
  if(_acked < _ack){
    return 0;
  }
  if(_sent == _len){
    _status = WS_MSG_SENT;
    return 0;
  }
  if(_sent > _len){
      _status = WS_MSG_ERROR;
      //ets_printf("E: %u > %u\n", _sent, _len);
      return 0;
  }

  size_t toSend = _len - _sent;
  size_t window = webSocketSendFrameWindow(client);

  if(window < toSend) {
      toSend = window;
  }

  _sent += toSend;
  _ack += toSend + ((toSend < 126)?2:4) + (_mask * 4);

  //ets_printf("W: %u %u\n", _sent - toSend, toSend);

  bool final = (_sent == _len);
  uint8_t* dPtr = (uint8_t*)(_data + (_sent - toSend));
  uint8_t opCode = (toSend && _sent == toSend)?_opcode:(uint8_t)WS_CONTINUATION;

  size_t sent = webSocketSendFrame(client, final, opCode, _mask, dPtr, toSend);
  _status = WS_MSG_SENDING;
  if(toSend && sent != toSend){
      //ets_printf("E: %u != %u\n", toSend, sent);
      _sent -= (toSend - sent);
      _ack -= (toSend - sent);
  }
  //ets_printf("S: %u %u\n", _sent, sent);
  return sent;
}


/*
 * Async WebSocket Client
 */
static const char AWSC_PING_PAYLOAD[] PROGMEM = { "ESPAsyncWebServer-PING" };
static constexpr size_t AWSC_PING_PAYLOAD_LEN = sizeof(AWSC_PING_PAYLOAD) - 1;

AsyncWebSocketClient::AsyncWebSocketClient(AsyncWebServerRequest *request, AsyncWebSocket *server)
  : _controlQueue(LinkedList<AsyncWebSocketControl *>([](AsyncWebSocketControl *c){ delete  c; }))
  , _messageQueue(LinkedList<AsyncWebSocketMessage *>([](AsyncWebSocketMessage *m){ delete  m; }))
  , _tempObject(NULL)
{
  _client = request->client();
  _server = server;
  _clientId = _server->_getNextId();
  _status = WS_CONNECTED;
  _pstate = 0;
  _lastMessageTime = millis();
  _keepAlivePeriod = 0;
  _droppedMessages = 0;
  _cbDepth = 0;
  _disconnected = false;
  _deadClient = nullptr;
  _hlen = 0;
  _ctrlData = nullptr;
  _client->setRxTimeout(0);
  _client->onError([](void *r, AsyncClient* c, int8_t error){ (void)c; auto ws = (AsyncWebSocketClient*)(r); ws->_enter(); ws->_onError(error); ws->_leave(); }, this);
  _client->onAck([](void *r, AsyncClient* c, size_t len, uint32_t time){ (void)c; auto ws = (AsyncWebSocketClient*)(r); ws->_enter(); ws->_onAck(len, time); ws->_leave(); }, this);
  _client->onDisconnect([](void *r, AsyncClient* c){ ((AsyncWebSocketClient*)(r))->_clientDisconnected(c); }, this);
  _client->onTimeout([](void *r, AsyncClient* c, uint32_t time){ (void)c; auto ws = (AsyncWebSocketClient*)(r); ws->_enter(); ws->_onTimeout(time); ws->_leave(); }, this);
  _client->onData([](void *r, AsyncClient* c, void *buf, size_t len){ (void)c; auto ws = (AsyncWebSocketClient*)(r); ws->_enter(); ws->_onData(buf, len); ws->_leave(); }, this);
  _client->onPoll([](void *r, AsyncClient* c){ (void)c; auto ws = (AsyncWebSocketClient*)(r); ws->_enter(); ws->_onPoll(); ws->_leave(); }, this);
  _server->_addClient(this);
  _server->_handleEvent(this, WS_EVT_CONNECT, request, NULL, 0);
  // the handshake runs inside a callback of the request
  request->_deleteSelf();
}

AsyncWebSocketClient::~AsyncWebSocketClient(){
  _messageQueue.free();
  _controlQueue.free();
  delete[] _ctrlData;
  _server->_handleEvent(this, WS_EVT_DISCONNECT, NULL, NULL, 0);
}

void AsyncWebSocketClient::_clientDisconnected(AsyncClient *c){
  if(_cbDepth){
    // inside a callback of this client: deleted by _leave()
    _disconnected = true;
    _deadClient = c;
    return;
  }
  _onDisconnect();
  delete c;
}

void AsyncWebSocketClient::_leave(){
  if(--_cbDepth || !_disconnected){
    return;
  }
  auto c = _deadClient;
  _onDisconnect(); // removes and deletes the client
  delete c;
}

void AsyncWebSocketClient::_protocolError(uint16_t code){
  _pstate = 2; // the rest of the data of this connection is ignored
  if(_status == WS_CONNECTED){
    close(code);
  } else {
    _client->close(true);
  }
}

bool AsyncWebSocketClient::canSend(){
  AsyncWebLockGuard l(_server->_getLock());
  return _messageQueue.length() < WS_MAX_QUEUED_MESSAGES;
}

void AsyncWebSocketClient::_onAck(size_t len, uint32_t time){
  AsyncWebLockGuard l(_server->_getLock());
  _lastMessageTime = millis();
  if(!_controlQueue.isEmpty()){
    auto head = _controlQueue.front();
    if(head->finished()){
      // an ack can be smaller than the frame, the rest belongs to the data messages
      len -= head->ack(len);
      if(head->acked()){
        if(_status == WS_DISCONNECTING && head->opcode() == WS_DISCONNECT){
          _controlQueue.remove(head);
          _status = WS_DISCONNECTED;
          _client->close(true);
          return;
        }
        _controlQueue.remove(head);
      }
    }
  }
  if(len && !_messageQueue.isEmpty()){
    _messageQueue.front()->ack(len, time);
  }
  _server->_cleanBuffers();
  _runQueue();
}

void AsyncWebSocketClient::_onPoll(){
  AsyncWebLockGuard l(_server->_getLock());
  if(_client->canSend() && (!_controlQueue.isEmpty() || !_messageQueue.isEmpty())){
    _runQueue();
  } else if(_keepAlivePeriod > 0 && _controlQueue.isEmpty() && _messageQueue.isEmpty() && (millis() - _lastMessageTime) >= _keepAlivePeriod){
    ping(AWSC_PING_PAYLOAD, AWSC_PING_PAYLOAD_LEN);
  }
}

void AsyncWebSocketClient::_runQueue(){
  AsyncWebLockGuard l(_server->_getLock());
  while(!_messageQueue.isEmpty() && _messageQueue.front()->finished()){
    _messageQueue.remove(_messageQueue.front());
  }

  // a control frame that was sent waits for its ack in the queue, it must not be sent again
  if(!_controlQueue.isEmpty() && !_controlQueue.front()->finished() && (_messageQueue.isEmpty() || _messageQueue.front()->betweenFrames()) && webSocketSendFrameWindow(_client) > (size_t)(_controlQueue.front()->len() - 1)){
    _controlQueue.front()->send(_client);
  } else if(!_messageQueue.isEmpty() && _messageQueue.front()->betweenFrames() && webSocketSendFrameWindow(_client)){
    _messageQueue.front()->send(_client);
  }
}

bool AsyncWebSocketClient::queueIsFull(){
  AsyncWebLockGuard l(_server->_getLock());
  if((_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES) || (_status != WS_CONNECTED) ) return true;
  return false;
}

void AsyncWebSocketClient::_queueMessage(AsyncWebSocketMessage *dataMessage){
  if(!dataMessage)
    return;
  AsyncWebLockGuard l(_server->_getLock());
  if(_status != WS_CONNECTED){
    delete dataMessage;
    return;
  }
  if(_messageQueue.length() >= WS_MAX_QUEUED_MESSAGES){
      _droppedMessages++;
      delete dataMessage;
  } else {
      _messageQueue.add(dataMessage);
  }
  if(_client->canSend())
    _runQueue();
}

void AsyncWebSocketClient::_queueControl(AsyncWebSocketControl *controlMessage){
  if(!controlMessage)
    return;
  AsyncWebLockGuard l(_server->_getLock());
  _controlQueue.add(controlMessage);
  if(_client->canSend())
    _runQueue();
}

void AsyncWebSocketClient::close(uint16_t code, const char *message){
  if(_status != WS_CONNECTED)
    return;
  AsyncWebSocketControl *control;
  if(code){
    const uint8_t packetLen = 2 + (message ? std::min<size_t>(123, strlen_P(message)) : 0);
    uint8_t buf[packetLen];
    buf[0] = (uint8_t)(code >> 8);
    buf[1] = (uint8_t)(code & 0xFF);
    if(message) {
      memcpy_P(buf + 2, message, packetLen - 2);
    }
    control = new (std::nothrow) AsyncWebSocketControl(WS_DISCONNECT, buf, packetLen);
  } else {
    control = new (std::nothrow) AsyncWebSocketControl(WS_DISCONNECT);
  }
  if(!control)
    return; // still connected, close() can be called again
  // one close frame: the connection is closed when it has been acked or the peer echoed it,
  // further calls and data messages are ignored
  _status = WS_DISCONNECTING;
  _queueControl(control);
}

void AsyncWebSocketClient::ping(const uint8_t *data, size_t len){
  if(_status == WS_CONNECTED) {
    _queueControl(new (std::nothrow) AsyncWebSocketControl(WS_PING, data, len));
  }
}

void AsyncWebSocketClient::_onDisconnect(){
  _client = NULL;
  _server->_handleDisconnect(this);
}

void AsyncWebSocketClient::_onData(void *pbuf, size_t plen){
  _lastMessageTime = millis();
  uint8_t *data = (uint8_t*)pbuf;
  // the data is not NUL terminated, the event handler gets the length (#5: the byte after the payload
  // is not part of the pbuf). A frame without payload is complete after its header, without more data
  while(!_disconnected && _pstate != 2 && (plen > 0 || (_pstate == 1 && _pinfo.index == _pinfo.len))){
    if(!_pstate){
      // #4: collect the header, it can be split across TCP segments
      size_t need = 2;
      while(plen){
        _hbuf[_hlen++] = *data++;
        plen--;
        if(_hlen >= 2){
          const uint8_t len7 = _hbuf[1] & 0x7F;
          need = 2 + (len7 == 126 ? 2 : (len7 == 127 ? 8 : 0)) + ((_hbuf[1] & 0x80) ? 4 : 0);
        }
        if(_hlen >= need)
          break;
      }
      if(_hlen < need)
        return; // the rest of the header is in the next segment
      _hlen = 0;

      _pinfo.index = 0;
      _pinfo.final = (_hbuf[0] & 0x80) != 0;
      _pinfo.opcode = _hbuf[0] & 0x0F;
      _pinfo.masked = (_hbuf[1] & 0x80) != 0;
      _pinfo.len = _hbuf[1] & 0x7F;
      size_t pos = 2;
      if(_pinfo.len == 126){
        _pinfo.len = ((uint16_t)_hbuf[2] << 8) | _hbuf[3];
        pos = 4;
      } else if(_pinfo.len == 127){
        _pinfo.len = 0;
        for(pos = 2; pos < 10; pos++)
          _pinfo.len = (_pinfo.len << 8) | _hbuf[pos];
      }
      if(_pinfo.masked)
        memcpy(_pinfo.mask, _hbuf + pos, 4);

      // RFC 6455 5.1: a client masks every frame. 5.5: control frames are final, at most 125 byte
      const bool control = (_pinfo.opcode & 0x08) != 0;
      if(!_pinfo.masked || (_pinfo.opcode > WS_BINARY && _pinfo.opcode < WS_DISCONNECT) || _pinfo.opcode > WS_PONG ||
          (control && (!_pinfo.final || _pinfo.len > 125))){
        _protocolError(1002);
        return;
      }
      if(_pinfo.len > ASYNCWEBSOCKET_MAX_FRAME_LEN){
        _protocolError(1009);
        return;
      }
      if(!control){
        if(_pinfo.opcode){
          _pinfo.message_opcode = _pinfo.opcode;
          _pinfo.num = 0;
        } else _pinfo.num += 1;
      }
      _pstate = 1;
    }

    const bool control = (_pinfo.opcode & 0x08) != 0;
    const size_t datalen = std::min((size_t)(_pinfo.len - _pinfo.index), plen);

    if(_pinfo.masked){
      for(size_t i=0;i<datalen;i++)
        data[i] ^= _pinfo.mask[(_pinfo.index+i)%4];
    }

    if((datalen + _pinfo.index) < _pinfo.len){
      if(control){
        // the payload of a control frame is processed as a whole
        if(!_ctrlData){
          _ctrlData = new (std::nothrow) uint8_t[125];
          if(!_ctrlData){
            _protocolError(1011);
            return;
          }
        }
        memcpy(_ctrlData + _pinfo.index, data, datalen);
      } else {
        _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, (uint8_t*)data, datalen);
      }
      _pinfo.index += datalen;
    } else {
      // the frame is complete
      _pstate = 0;
      uint8_t *payload = data;
      size_t payloadLen = datalen;
      if(control && _pinfo.index){
        memcpy(_ctrlData + _pinfo.index, data, datalen);
        payload = _ctrlData;
        payloadLen = _pinfo.len;
      }
      if(_pinfo.opcode == WS_DISCONNECT){
        // #6: the reason is not NUL terminated, a code needs 2 byte
        if(payloadLen >= 2){
          uint16_t reasonCode = ((uint16_t)payload[0] << 8) | payload[1];
          if(reasonCode > 1001){
            _server->_handleEvent(this, WS_EVT_ERROR, (void *)&reasonCode, payload + 2, payloadLen - 2);
          }
        }
        if(_status == WS_DISCONNECTING){
          _status = WS_DISCONNECTED;
          _client->close(true);
        } else if(!_disconnected){
          _status = WS_DISCONNECTING;
          _client->ackLater();
          _queueControl(new (std::nothrow) AsyncWebSocketControl(WS_DISCONNECT, payload, payloadLen));
        }
      } else if(_pinfo.opcode == WS_PING){
        _queueControl(new (std::nothrow) AsyncWebSocketControl(WS_PONG, payload, payloadLen));
      } else if(_pinfo.opcode == WS_PONG){
        if(payloadLen != AWSC_PING_PAYLOAD_LEN || memcmp_P(payload, AWSC_PING_PAYLOAD, AWSC_PING_PAYLOAD_LEN) != 0)
          _server->_handleEvent(this, WS_EVT_PONG, NULL, payload, payloadLen);
      } else {//continuation or text/binary frame
        _server->_handleEvent(this, WS_EVT_DATA, (void *)&_pinfo, data, datalen);
      }
    }

    data += datalen;
    plen -= datalen;
  }
}

size_t AsyncWebSocketClient::vprintf_P(PGM_P format, va_list arg) {
  char temp[MAX_PRINTF_LEN];
  char *buffer = temp;

  // a va_list can only be consumed once, the second pass needs its own copy
  va_list copy;
  va_copy(copy, arg);
  int result = vsnprintf_P(temp, sizeof(temp), format, copy);
  va_end(copy);
  if (result < 0) {
    return 0;
  }
  size_t len = result;

  if (len > sizeof(temp) - 1) {
    buffer = new (std::nothrow) char[len + 1];
    if (!buffer) {
      return 0;
    }
    vsnprintf_P(buffer, len + 1, format, arg);
  }
  text(buffer, len);
  if (buffer != temp) {
    delete[] buffer;
  }
  return len;
}

IPAddress AsyncWebSocketClient::remoteIP() {
    if(!_client) {
        return IPAddress(0U);
    }
    return _client->remoteIP();
}

uint16_t AsyncWebSocketClient::remotePort() {
    if(!_client) {
        return 0;
    }
    return _client->remotePort();
}

/*
 * Async Web Socket - Each separate socket location
 */

AsyncWebSocket::AsyncWebSocket(const String& url)
  :_url(url)
  ,_clients(LinkedList<AsyncWebSocketClient *>([](AsyncWebSocketClient *c){ delete c; }))
  ,_cNextId(1)
  ,_enabled(true)
  ,_buffers(LinkedList<AsyncWebSocketMessageBuffer *>([](AsyncWebSocketMessageBuffer *b){ delete b; }))
{
}

AsyncWebSocket::~AsyncWebSocket(){
  // a client does not detach from its TCP connection when it is deleted: the connection is closed
  // without its callbacks first, the client is removed and deleted by its disconnect handler
  while(!_clients.isEmpty()){
    auto c = _clients.front();
    if(c->client())
      AsyncWebServer::_closeDetached(c->client());
    c->_onDisconnect();
  }
  // after the clients, their messages reference the buffers
  _buffers.free();
}

void AsyncWebSocket::_handleEvent(AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (_eventHandler) {
    _eventHandler(this, client, type, arg, data, len);
  }
}

void AsyncWebSocket::_handleDisconnect(AsyncWebSocketClient * client) {
  {
    AsyncWebLockGuard l(_lock);
    if(!_clients.unlink(client))
      return;
  }
  // outside the lock: the destructor sends WS_EVT_DISCONNECT to the event handler
  delete client;
}

void AsyncWebSocket::forEachClient(std::function<bool(AsyncWebSocketClient *)> func) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if(!func(c))
      break;
  }
}

bool AsyncWebSocket::availableForWriteAll() {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if (c->queueIsFull()) return false;
  }
  return true;
}

bool AsyncWebSocket::availableForWrite(uint32_t id) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if (c->queueIsFull() && (c->id() == id )) return false;
  }
  return true;
}

size_t AsyncWebSocket::count() const {
  AsyncWebLockGuard l(_lock);
  return _clients.count_if([](AsyncWebSocketClient * c) {
    return c->status() == WS_CONNECTED;
  });
}

AsyncWebSocketClient * AsyncWebSocket::client(uint32_t id) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if (c->id() == id && c->status() == WS_CONNECTED){
      return c;
    }
  }
  return nullptr;
}


void AsyncWebSocket::close(uint32_t id, uint16_t code, const char *message) {
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c->close(code, message);
  }
}

void AsyncWebSocket::closeAll(uint16_t code, const char *message) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if (c->status() == WS_CONNECTED)
      c->close(code, message);
  }
}

void AsyncWebSocket::cleanupClients(uint16_t maxClients) {
  AsyncWebLockGuard l(_lock);
  if (count() > maxClients){
    _clients.front()->close();
  }
}

void AsyncWebSocket::ping(uint32_t id, const uint8_t *data, size_t len){
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c->ping(data, len);
  }
}

void AsyncWebSocket::pingAll(const uint8_t *data, size_t len){
  AsyncWebLockGuard l(_lock);
  for(const auto& c: _clients){
    if (c->status() == WS_CONNECTED)
      c->ping(data, len);
  }
}

void AsyncWebSocket::text(uint32_t id, const char * message, size_t len){
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c->text(message, len);
  }
}

void AsyncWebSocket::textAll(AsyncWebSocketMessageBuffer * buffer){
  AsyncWebLockGuard l(_lock);
  if (!buffer) {
    return;
  }
  buffer->lock();
  for(const auto& c: _clients){
    if (c->status() == WS_CONNECTED){
        c->text(buffer);
    }
  }
  buffer->unlock();
  _cleanBuffers();
}


void AsyncWebSocket::binary(uint32_t id, const char *message, size_t len){
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c->binary(message, len);
  }
}

void AsyncWebSocket::binaryAll(AsyncWebSocketMessageBuffer *buffer) {
  AsyncWebLockGuard l(_lock);
  if (!buffer) {
    return;
  }
  buffer->lock();
    for(const auto &c: _clients) {
    if (c->status() == WS_CONNECTED)
      c->binary(buffer);
  }
  buffer->unlock();
  _cleanBuffers();
}

void AsyncWebSocket::message(uint32_t id, AsyncWebSocketMessage *message) {
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if(c) {
    c->message(message);
  }
}

void AsyncWebSocket::messageAll(AsyncWebSocketMultiMessage *message) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients){
    if (c->status() == WS_CONNECTED) {
      c->message(message);
    }
  }
  _cleanBuffers();
}

size_t AsyncWebSocket::vprintf_P(uint32_t id, const char *format, va_list arg) {
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    return c->vprintf_P(format, arg);
  }
  return 0;
}

size_t AsyncWebSocket::vprintfAll_P(const char *format, va_list arg) {
  // a va_list can only be consumed once, the length pass needs its own copy
  va_list copy;
  va_copy(copy, arg);
  int result = vsnprintf_P(nullptr, 0, format, copy);
  va_end(copy);
  if (result < 0) {
    return 0;
  }
  size_t len = result;

  AsyncWebSocketMessageBuffer *buffer = makeBuffer(len);
  if (!buffer || !buffer->get()) {
    return 0;
  }

  vsnprintf_P((char *)buffer->get(), len + 1, format, arg);

  textAll(buffer);
  return len;
}

void AsyncWebSocket::text(uint32_t id, const __FlashStringHelper *message) {
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c->text(message);
  }
}

void AsyncWebSocket::textAll(const __FlashStringHelper *message) {
  AsyncWebLockGuard l(_lock);
  for(const auto& c: _clients) {
    if (c->status() == WS_CONNECTED)
      c->text(message);
  }
}

void AsyncWebSocket::binary(uint32_t id, const __FlashStringHelper *message, size_t len) {
  AsyncWebLockGuard l(_lock);
  AsyncWebSocketClient *c = client(id);
  if (c) {
    c-> binary(message, len);
  }
}

void AsyncWebSocket::binaryAll(const __FlashStringHelper *message, size_t len) {
  AsyncWebLockGuard l(_lock);
  for(const auto &c: _clients) {
    if (c->status() == WS_CONNECTED)
      c-> binary(message, len);
  }
 }

static const char _WS_STR_CONNECTION[] PROGMEM = { "Connection" };
static const char _WS_STR_UPGRADE[] PROGMEM = { "Upgrade" };
static const char _WS_STR_ORIGIN[] PROGMEM = { "Origin" };
static const char _WS_STR_VERSION[] PROGMEM = { "Sec-WebSocket-Version" };
static const char _WS_STR_KEY[] PROGMEM = { "Sec-WebSocket-Key" };
static const char _WS_STR_PROTOCOL[] PROGMEM = { "Sec-WebSocket-Protocol" };
static const char _WS_STR_ACCEPT[] PROGMEM = { "Sec-WebSocket-Accept" };
static const char _WS_STR_UUID[] PROGMEM = { "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" };

#define WS_STR_CONNECTION FPSTR(_WS_STR_CONNECTION)
#define WS_STR_UPGRADE FPSTR(_WS_STR_UPGRADE)
#define WS_STR_ORIGIN FPSTR(_WS_STR_ORIGIN)
#define WS_STR_VERSION FPSTR(_WS_STR_VERSION)
#define WS_STR_KEY FPSTR(_WS_STR_KEY)
#define WS_STR_PROTOCOL FPSTR(_WS_STR_PROTOCOL)
#define WS_STR_ACCEPT FPSTR(_WS_STR_ACCEPT)
#define WS_STR_UUID FPSTR(_WS_STR_UUID)

bool AsyncWebSocket::canHandle(AsyncWebServerRequest *request){
  if(!_enabled)
    return false;

  if(request->method() != HTTP_GET || !request->url().equals(_url) || !request->isExpectedRequestedConnType(RCT_WS))
    return false;

  request->addInterestingHeader(WS_STR_CONNECTION);
  request->addInterestingHeader(WS_STR_UPGRADE);
  request->addInterestingHeader(WS_STR_ORIGIN);
  request->addInterestingHeader(WS_STR_VERSION);
  request->addInterestingHeader(WS_STR_KEY);
  request->addInterestingHeader(WS_STR_PROTOCOL);
  return true;
}

void AsyncWebSocket::handleRequest(AsyncWebServerRequest *request){
  if(!request->hasHeader(WS_STR_VERSION) || !request->hasHeader(WS_STR_KEY)){
    request->send(400);
    return;
  }
  AsyncWebHeader* version = request->getHeader(WS_STR_VERSION);
  if(version->value().toInt() != 13){
    AsyncWebServerResponse *response = request->beginResponse(400);
    response->addHeader(WS_STR_VERSION, F("13"));
    request->send(response);
    return;
  }
  AsyncWebHeader* key = request->getHeader(WS_STR_KEY);
  AsyncWebServerResponse *response = new (std::nothrow) AsyncWebSocketResponse(key->value(), this);
  if(!response){
    // out of memory: closing here would delete the request while it is still being parsed,
    // the next poll closes the connection
    request->client()->setRxTimeout(1);
    return;
  }
  if(request->hasHeader(WS_STR_PROTOCOL)){
    AsyncWebHeader* protocol = request->getHeader(WS_STR_PROTOCOL);
    //ToDo: check protocol
    response->addHeader(WS_STR_PROTOCOL, protocol->value());
  }
  request->send(response);
}

AsyncWebSocketMessageBuffer * AsyncWebSocket::makeBuffer(size_t size)
{
  AsyncWebSocketMessageBuffer * buffer = new (std::nothrow) AsyncWebSocketMessageBuffer(size);
  if (buffer) {
    buffer->lock(); // the caller unlocks it (textAll(), binaryAll(), after text(buffer))
    AsyncWebLockGuard l(_lock);
    _buffers.add(buffer);
  }
  return buffer;
}

AsyncWebSocketMessageBuffer * AsyncWebSocket::makeBuffer(const uint8_t *data, size_t size)
{
  AsyncWebSocketMessageBuffer * buffer = new (std::nothrow) AsyncWebSocketMessageBuffer(data, size);

  if (buffer) {
    buffer->lock(); // the caller unlocks it (textAll(), binaryAll(), after text(buffer))
    AsyncWebLockGuard l(_lock);
    _buffers.add(buffer);
  }

  return buffer;
}

AsyncWebSocketMessageBuffer * AsyncWebSocket::makeBuffer(String &&str)
{
  AsyncWebSocketMessageBuffer * buffer = new (std::nothrow) AsyncWebSocketMessageBuffer(std::move(str));

  if (buffer) {
    buffer->lock(); // the caller unlocks it (textAll(), binaryAll(), after text(buffer))
    AsyncWebLockGuard l(_lock);
    _buffers.add(buffer);
  }

  return buffer;
}

void AsyncWebSocket::_cleanBuffers()
{
  AsyncWebLockGuard l(_lock);

  _buffers.remove_if([](AsyncWebSocketMessageBuffer * const &c){
    return c && c->canDelete();
  });
}

const AsyncWebSocket::AsyncWebSocketClientLinkedList &AsyncWebSocket::getClients() const {
  return _clients;
}

/*
 * Response to Web Socket request - sends the authorization and detaches the TCP Client from the web server
 * Authentication code from https://github.com/Links2004/arduinoWebSockets/blob/master/src/WebSockets.cpp#L480
 */

AsyncWebSocketResponse::AsyncWebSocketResponse(const String& key, AsyncWebSocket *server){
  _server = server;
  _code = 101;
  _sendContentLength = false;

  // the key is a header of the request, append the UUID to a copy
  String input = key;
  input += WS_STR_UUID;
  uint8_t hash[20];
#ifdef ESP8266
  sha1(input, hash);
#else
  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts_ret(&ctx);
  mbedtls_sha1_update_ret(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha1_finish_ret(&ctx, hash);
  mbedtls_sha1_free(&ctx);
#endif
  // 20 bytes are 28 base64 characters, base64_encode_blockend() adds the NUL
  char buffer[32] = {};
  base64_encodestate _state;
  base64_init_encodestate(&_state);
  int len = base64_encode_block((const char *) hash, sizeof(hash), buffer, &_state);
  base64_encode_blockend((buffer + len), &_state);
  addHeader(WS_STR_CONNECTION, WS_STR_UPGRADE);
  addHeader(WS_STR_UPGRADE, F("websocket"));
  addHeader(WS_STR_ACCEPT,buffer);
}

void AsyncWebSocketResponse::_respond(AsyncWebServerRequest *request){
  if(_state == RESPONSE_FAILED){
    request->client()->close(true);
    return;
  }
  String out = _assembleHead(request->version());
  request->client()->write(out.c_str(), _headLength, ASYNC_WRITE_FLAG_COPY);
  _state = RESPONSE_WAIT_ACK;
}

size_t AsyncWebSocketResponse::_ack(AsyncWebServerRequest *request, size_t len, uint32_t time){
  (void)time;
  if(len){
    if(!new (std::nothrow) AsyncWebSocketClient(request, _server)){
      // out of memory: the request still owns the connection, the next poll closes it
      request->client()->setRxTimeout(1);
    }
  }
  return 0;
}
