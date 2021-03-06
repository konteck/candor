/**
 * Copyright (c) 2012, Fedor Indutny.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _INCLUDE_CANDOR_H_
#define _INCLUDE_CANDOR_H_

#include <stdint.h>  // uint32_t
#include <sys/types.h>  // size_t

namespace candor {

// Forward declarations
namespace internal {
  class Heap;
  class CodeSpace;
  template <class T, class ItemParent>
  class List;
  class EmptyClass;
  class HValueReference;
}  // namespace internal

class Value;
class Nil;
class Function;
class Number;
class Boolean;
class String;
class Object;
class Array;
class CData;
struct Error;

class Isolate {
 public:
  Isolate();
  ~Isolate();

  static Isolate* GetCurrent();

  bool HasError();
  Error* GetError();
  void PrintError();

  Array* StackTrace();

  static void EnableFullgenLogging();
  static void DisableFullgenLogging();
  static void EnableHIRLogging();
  static void DisableHIRLogging();
  static void EnableLIRLogging();
  static void DisableLIRLogging();

 protected:
  void SetError(Error* err);

  internal::Heap* heap;
  internal::CodeSpace* space;

  Error* error;

  friend class Value;
  friend class Nil;
  friend class Function;
  friend class Number;
  friend class Boolean;
  friend class String;
  friend class Object;
  friend class Array;
  friend class CData;

  template <class T>
  friend class Handle;
  friend class CWrapper;
};

struct Error {
  const char* message;
  int line;
  int offset;

  const char* filename;
  const char* source;
  uint32_t length;
};

class Value {
 public:
  enum ValueType {
    kNone,
    kNil,
    kNumber,
    kBoolean,
    kString,
    kFunction,
    kObject,
    kArray,
    kCData
  };

  typedef void (*WeakCallback)(Value* value);

  static Value* New(char* addr);

  template <class T>
  T* As();

  template <class T>
  static T* Cast(char* addr);

  template <class T>
  static T* Cast(Value* value);

  template <class T>
  bool Is();

  ValueType Type();

  Number* ToNumber();
  Boolean* ToBoolean();
  String* ToString();

  void SetWeakCallback(WeakCallback callback);
  void ClearWeak();

  inline char* addr() { return reinterpret_cast<char*>(this); }

  static const ValueType tag = kNone;
};

class Function : public Value {
 public:
  typedef Value* (*BindingCallback)(uint32_t argc, Value* argv[]);

  static Function* New(const char* filename,
                       const char* source,
                       uint32_t length);
  static Function* New(const char* filename, const char* source);
  static Function* New(const char* source);
  static Function* New(BindingCallback callback);

  Object* GetContext();
  void SetContext(Object* context);

  uint32_t Argc();

  Value* Call(uint32_t argc, Value* argv[]);

  static const ValueType tag = kFunction;
};

class Nil : public Value {
 public:
  static Nil* New();

  static const ValueType tag = kNil;
};

class Boolean : public Value {
 public:
  static Boolean* New(bool value);
  static Boolean* True();
  static Boolean* False();

  bool IsTrue();
  bool IsFalse();

  static const ValueType tag = kBoolean;
};

class Number : public Value {
 public:
  static Number* NewDouble(double value);
  static Number* NewIntegral(int64_t value);

  double Value();
  int64_t IntegralValue();

  bool IsIntegral();

  static const ValueType tag = kNumber;
};

class String : public Value {
 public:
  static String* New(const char* value);
  static String* New(const char* value, uint32_t len);

  const char* Value();
  uint32_t Length();

  static const ValueType tag = kString;
};

class Object : public Value {
 public:
  static Object* New();

  void Set(Value* key, Value* value);
  void Set(const char* key, Value* value);
  Value* Get(Value* key);
  Value* Get(const char* key);
  void Delete(Value* key);
  void Delete(const char* key);

  Array* Keys();
  Object* Clone();

  static const ValueType tag = kObject;
};

class Array : public Value {
 public:
  static Array* New();

  void Set(int64_t key, Value* value);
  Value* Get(int64_t key);
  void Delete(int64_t key);

  int64_t Length();

  static const ValueType tag = kArray;
};

class CData : public Value {
 public:
  static CData* New(size_t size);

  void* GetContents();

  static const ValueType tag = kCData;
};

template <class T>
class Handle {
 public:
  Handle();
  explicit Handle(Value* v);
  ~Handle();

  void Wrap(Value* v);
  void Unwrap();

  void Ref();
  void Unref();

  bool IsEmpty();

  inline bool IsWeak() { return ref_count <= 0; }
  inline bool IsPersistent() { return ref_count > 0; }

  inline T* operator*() { return value; }
  inline T* operator->() { return value; }

  template <class S>
  static inline Handle<T> Cast(Handle<S> handle) {
    return Handle<T>(Value::Cast<T>(*handle));
  }

 protected:
  T* value;
  int ref_count;

  internal::HValueReference* ref;
};

class CWrapper {
 public:
  explicit CWrapper(const int* magic);
  virtual ~CWrapper();

  inline CData* Wrap() { return ref->As<CData>(); }

  static inline bool HasClass(Value* value, const int* magic) {
    if (!value->Is<CData>()) return false;

    CWrapper* w = *reinterpret_cast<CWrapper**>(
        value->As<CData>()->GetContents());
    return w->magic_ == magic;
  }

  template <class T>
  static inline T* Unwrap(Value* value) {
    return *reinterpret_cast<T**>(value->As<CData>()->GetContents());
  }

  void Ref();
  void Unref();
  bool IsWeak();
  bool IsPersistent();

 protected:
  static void WeakCallback(Value* data);

  Isolate* isolate;
  const int* magic_;

  Handle<CData> ref;
};

}  // namespace candor

#endif  // _INCLUDE_CANDOR_H_
