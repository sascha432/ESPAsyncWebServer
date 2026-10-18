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
#ifndef STRINGARRAY_H_
#define STRINGARRAY_H_

#include "stddef.h"
#include "WString.h"
#include <functional>
#include <utility>

template <typename T>
class LinkedListNode {
    T _value;
  public:
    LinkedListNode<T>* next;
    LinkedListNode(const T val): _value(val), next(nullptr) {}
    ~LinkedListNode(){}
    const T& value() const { return _value; };
    T& value(){ return _value; }
};

template <typename T, template<typename> class Item = LinkedListNode>
class LinkedList {
  public:
    typedef Item<T> ItemType;
    typedef std::function<void(const T&)> OnRemove;
    typedef std::function<bool(const T&)> Predicate;
  private:
    ItemType* _root;
    OnRemove _onRemove;

    // the next node is read before the current one is yielded: removing the current element inside a
    // range-for does not read the freed node (removing any other element is still not allowed)
    class Iterator {
      ItemType* _node;
      ItemType* _next;
    public:
      Iterator(ItemType* current = nullptr) : _node(current), _next(current ? current->next : nullptr) {}
      Iterator(const Iterator& i) : _node(i._node), _next(i._next) {}
      Iterator& operator ++() {
        _node = _next;
        _next = _node ? _node->next : nullptr;
        return *this;
      }
      bool operator != (const Iterator& i) const { return _node != i._node; }
      const T& operator * () const { return _node->value(); }
      const T* operator -> () const { return &_node->value(); }
    };

  public:
    typedef const Iterator ConstIterator;
    ConstIterator begin() const { return ConstIterator(_root); }
    ConstIterator end() const { return ConstIterator(nullptr); }

    LinkedList(OnRemove onRemove) : _root(nullptr), _onRemove(onRemove) {}
    // a copy would share the nodes with the original
    LinkedList(const LinkedList&) = delete;
    LinkedList& operator=(const LinkedList&) = delete;
    LinkedList(LinkedList&& other) : _root(other._root), _onRemove(std::move(other._onRemove)) {
      other._root = nullptr;
    }
    LinkedList& operator=(LinkedList&& other){
      if(this != &other){
        free();
        _root = other._root;
        _onRemove = std::move(other._onRemove);
        other._root = nullptr;
      }
      return *this;
    }
    ~LinkedList(){
      free();
    }
    void add(const T& t){
      auto it = new ItemType(t);
      if(!_root){
        _root = it;
      } else {
        auto i = _root;
        while(i->next) i = i->next;
        i->next = it;
      }
    }
    T& front() const {
      return _root->value();
    }

    bool isEmpty() const {
      return _root == nullptr;
    }
    size_t length() const {
      size_t i = 0;
      auto it = _root;
      while(it){
        i++;
        it = it->next;
      }
      return i;
    }
    size_t count_if(Predicate predicate) const {
      size_t i = 0;
      auto it = _root;
      while(it){
        if (!predicate){
          i++;
        }
        else if (predicate(it->value())) {
          i++;
        }
        it = it->next;
      }
      return i;
    }
    const T* nth(size_t N) const {
      size_t i = 0;
      auto it = _root;
      while(it){
        if(i++ == N)
          return &(it->value());
        it = it->next;
      }
      return nullptr;
    }
    bool remove(const T& t){
      auto it = _root;
      auto pit = _root;
      while(it){
        if(it->value() == t){
          if(it == _root){
            _root = _root->next;
          } else {
            pit->next = it->next;
          }

          if (_onRemove) {
            _onRemove(it->value());
          }

          delete it;
          return true;
        }
        pit = it;
        it = it->next;
      }
      return false;
    }
    // removes the element without calling the remove callback, the caller owns it afterwards
    bool unlink(const T& t){
      ItemType* prev = nullptr;
      for(auto it = _root; it; prev = it, it = it->next){
        if(it->value() == t){
          if(prev){
            prev->next = it->next;
          } else {
            _root = it->next;
          }
          delete it;
          return true;
        }
      }
      return false;
    }
    // removes every element the predicate matches, returns how many were removed
    size_t remove_if(Predicate predicate){
      size_t count = 0;
      ItemType* prev = nullptr;
      auto it = _root;
      while(it){
        auto next = it->next;
        if(predicate(it->value())){
          if(prev){
            prev->next = next;
          } else {
            _root = next;
          }
          if (_onRemove) {
            _onRemove(it->value());
          }
          delete it;
          count++;
        } else {
          prev = it;
        }
        it = next;
      }
      return count;
    }
    bool remove_first(Predicate predicate){
      auto it = _root;
      auto pit = _root;
      while(it){
        if(predicate(it->value())){
          if(it == _root){
            _root = _root->next;
          } else {
            pit->next = it->next;
          }
          if (_onRemove) {
            _onRemove(it->value());
          }
          delete it;
          return true;
        }
        pit = it;
        it = it->next;
      }
      return false;
    }

    void free(){
      while(_root != nullptr){
        auto it = _root;
        _root = _root->next;
        if (_onRemove) {
          _onRemove(it->value());
        }
        delete it;
      }
      _root = nullptr;
    }

    void setOnRemove(const OnRemove &cb) {
        _onRemove = cb;
    }
};


class StringArray : public LinkedList<String> {
public:

  StringArray() : LinkedList(nullptr) {}

  bool containsIgnoreCase(const String& str){
    for (const auto& s : *this) {
      if (str.equalsIgnoreCase(s)) {
        return true;
      }
    }
    return false;
  }
};




#endif /* STRINGARRAY_H_ */
