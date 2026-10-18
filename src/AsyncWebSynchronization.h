#ifndef ASYNCWEBSYNCHRONIZATION_H_
#define ASYNCWEBSYNCHRONIZATION_H_

// Synchronisation is only available on ESP32, as the ESP8266 isn't using FreeRTOS by default

#include <Arduino.h>

#ifdef ESP32

// This is the ESP32 version of the Sync Lock, a recursive FreeRTOS mutex: the same task can lock it
// again (the owner check that compared pxCurrentTCB, an array, let every task pass)
class AsyncWebLock
{
private:
  SemaphoreHandle_t _lock;

public:
  AsyncWebLock() : _lock(xSemaphoreCreateRecursiveMutex()) {
  }

  ~AsyncWebLock() {
    vSemaphoreDelete(_lock);
  }

  AsyncWebLock(const AsyncWebLock &) = delete;
  AsyncWebLock &operator=(const AsyncWebLock &) = delete;

  bool lock() const {
    return xSemaphoreTakeRecursive(_lock, portMAX_DELAY) == pdTRUE;
  }

  void unlock() const {
    xSemaphoreGiveRecursive(_lock);
  }
};

#else

// This is the 8266 version of the Sync Lock which is currently unimplemented
class AsyncWebLock
{

public:
  AsyncWebLock() {
  }

  ~AsyncWebLock() {
  }

  bool lock() const {
    return false;
  }

  void unlock() const {
  }
};
#endif

class AsyncWebLockGuard
{
private:
  const AsyncWebLock *_lock;

public:
  AsyncWebLockGuard(const AsyncWebLock &l) {
    if (l.lock()) {
      _lock = &l;
    } else {
      _lock = NULL;
    }
  }

  ~AsyncWebLockGuard() {
    if (_lock) {
      _lock->unlock();
    }
  }
};

#endif // ASYNCWEBSYNCHRONIZATION_H_
