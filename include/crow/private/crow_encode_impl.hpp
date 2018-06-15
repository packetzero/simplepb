#ifndef _CROW_ENCODE_IMPL_HPP_
#define _CROW_ENCODE_IMPL_HPP_
#include <map>
#include <errno.h>
#include <stdexcept>
#include <unistd.h>

#include "../../crow.hpp"
#include "stack.hpp"
#include "protobuf_wire_format.h"

#define NONE_LEFT(PTR) (PTR >= _end)
#define BYTES_REMAIN(PTR) (PTR < _end)
#define MAX_NAME_LEN 64
#define MAX_FIELDS   72    // ridiculously wide table

namespace crow {

  class EncoderImpl : public Encoder {
    static const uint8_t UPPER_BIT = (uint8_t)0x80;
  public:
    EncoderImpl(size_t initialCapacity) : Encoder(), _stack(initialCapacity),
          _dataStack(1024), _hdrStack(1024), _fields(),
          _structFields(), _haveStructData(false), _structLen(0),
          _structDefFinalized(false), _structBuf(0)  {}

    ~EncoderImpl() { }

    Field *fieldFor(CrowType type, uint32_t id, uint32_t subid = 0) override {
      for (int i=0; i < _fields.size(); i++) {
        Field *pField = &_fields[i];
        if (pField->id == id && pField->subid == subid) return pField;
      }
      Field* p = _newField(type);
      if (p == _dummyField()) return p;

      p->id = id;
      p->subid = subid;

      return p;
    }

    Field *fieldFor(CrowType type, std::string name) override {
      for (int i=0; i < _fields.size(); i++) {
        Field *pField = &_fields[i];
        if (pField->name == name) return pField;
      }
      Field* p = _newField(type);
      if (p == _dummyField()) return p;

      p->name = name;

      return p;
    }

    Field* _dummyField() {
      static Field dummy = { NONE /* type */, 0 /* id */, 9999999, "FIELD_LIMIT_REACHED", 0 /* index */ };
      return &dummy;
    }

    void put(const Field *pField, int32_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(ZigZagEncode32(value), staq());
    }
    void put(const Field *pField, uint32_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(value, staq());
    }
    void put(const Field *pField, int64_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(ZigZagEncode64(value), staq());
    }
    void put(const Field *pField, uint64_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
     writeVarInt(value, staq());
    }
    void put(const Field *pField, double value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeFixed64(EncodeDouble(value), staq());
    }
    void put(const Field *pField, float value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeFixed32(EncodeFloat(value), staq());
    }
    void put(const Field *pField, const std::string value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(value.length(), staq());
      memcpy(staq().Push(value.length()), value.c_str(), value.length());
    }
    void put(const Field *pField, const char* str) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      size_t srcLen = strlen(str);
      writeVarInt(srcLen, staq());
      memcpy(staq().Push(srcLen), str, srcLen);
    }
    void put(const Field *pField, const std::vector<uint8_t>& value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(value.size(), staq());
      memcpy(staq().Push(value.size()), value.data(), value.size());
    }
    void put(const Field *pField, const uint8_t* bytes, size_t len) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(len, staq());
      memcpy(staq().Push(len), bytes, len);
    }
    void put(const Field *pField, bool value) override {
      put(pField, (value ? (uint8_t)1 : (uint8_t)0));
    }

    virtual void put(const Field *pField, int8_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      uint8_t* ptr = staq().Push(1);
      *ptr = (uint8_t)value;
    }
    virtual void put(const Field *pField, uint8_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      uint8_t* ptr = staq().Push(1);
      *ptr = value;
    }
    virtual void put(const Field *pField, int16_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(ZigZagEncode32(value), staq());
    }
    virtual void put(const Field *pField, uint16_t value) override {
      if (pField == _dummyField()) return;
      writeIndexTag(pField);
      writeVarInt(value, staq());
    }

    void _flush(int fd = 0) {
      // flush header
      if (_hdrStack.GetSize() > 0) {
        // copy data
        memcpy(_stack.Push(_hdrStack.GetSize()), _hdrStack.Bottom(), _hdrStack.GetSize());
        _hdrStack.Clear();
      }

      // write struct data if defined

      if (_structLen > 0 && _haveStructData) {

        if (_structDefFinalized && !_haveStructData) {
          throw new std::runtime_error("row has no struct data");
        }
        *(_stack.Push(1)) = TROW;
        memcpy(_stack.Push(_structLen), _structBuf.Bottom(), _structLen);
        _structDefFinalized = true;

        // when we have both struct and variable fields, need to write length
        // of variable section after struct data

        if (_fields.size() > _structFields.size()) {
          writeVarInt(_dataStack.GetSize(),_stack);
        }
      }

      // write variable fields

      if (_dataStack.GetSize() > 0) {

        // write TROW, but only if we don't have struct data defined
        if (_structLen == 0) {
          *(_stack.Push(1)) = TROW;
        }

        // copy data
        memcpy(_stack.Push(_dataStack.GetSize()), _dataStack.Bottom(), _dataStack.GetSize());
        _dataStack.Clear();
      }
      _haveStructData = false;

      if (fd > 0) {
        write(fd, (const void *)_stack.Bottom(), _stack.GetSize());
        _stack.Clear();
      }
    }

    virtual void startRow() override {
      _flush();
    }

    virtual void flush() const override {
      ((EncoderImpl*)this)->_flush();
    }
    virtual void flush(int fd) override {
      _flush(fd);
    }
    virtual void endRow(int fd) override {
      if (fd > 0) _flush(fd);
    }


    const uint8_t* data() const override { flush(); return _stack.Bottom(); }

    size_t size() const override { return _stack.GetSize(); }

    void clear() override {
      _stack.Clear();
      _fields.clear();
      _structLen = 0;
      _structFields.clear();
    }

    virtual int struct_hdr(CrowType typeId, uint32_t id, uint32_t subid = 0, std::string name = "", int fixedLength = 0) override {

      // check typeId, fixedLength

      if (typeId == 0 || typeId >= CrowType::NUM_TYPES) {
        return -1;
      }
      if (fixedLength == 0 && (typeId == CrowType::TSTRING || typeId == CrowType::TBYTES)) {
        return -2;
      }

      if (_structDefFinalized) {
        throw new std::invalid_argument("Trying to add struct field after first struct row finalized");
      }

      if (_fields.size() > _structFields.size()) {
        throw new std::invalid_argument("All struct columns must come before variable columns");
      }

      Field *pField = (name.empty() ? fieldFor(typeId, id, subid) : fieldFor(typeId, name));
      pField->name = name;
      pField->isRaw = true;
      pField->fixedLen = fixedLength;
      _structFields.push_back(*pField);

      _structLen += (fixedLength > 0 ? fixedLength : byte_size(typeId));

      writeIndexTag(pField);

      return 0;
    }

    virtual int struct_hdr(CrowType typ, std::string name, int fixedLength = 0) override {
      return struct_hdr(typ, 0, 0, name, fixedLength);
    }

    int put_struct(const void *data, size_t struct_size) override {
      if (struct_size == 0 || struct_size != _structLen) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "struct size:%lu does not match definition:%lu", struct_size, _structLen);
        throw new std::invalid_argument(tmp);
      }

      if (_structBuf.GetSize() == 0) {
        _structBuf.Push(_structLen);
      }

      //startRow();

      memcpy(_structBuf.Bottom(), data, _structLen);
      _haveStructData = true;

      return 0;
    }

  private:

    /**
     * return _stack or _setStack, depending on _setModeEnabled field.
     */
    Stack & staq() { return _dataStack; } // (_setModeEnabled ? _setStack : _stack); }

    Field *_newField(CrowType typeId) {
      // new one
      uint8_t index = (uint8_t)_fields.size();
      if ((index & UPPER_BIT) != 0) {
        // wow, 127 fields?  return a dummy field .. one that doesn't get written
        return _dummyField();
      }
      _fields.push_back(Field());
      Field *p = &_fields[index];
      p->index = index;
      p->typeId = typeId;
      return p;
    }

    void writeFixed64(uint64_t value, Stack &stack) {
      union { uint64_t ival; uint8_t bytes[8];};
      ival = value;
      memcpy(stack.Push(sizeof(value)), bytes, sizeof(bytes));
    }
    void writeFixed32(uint32_t value, Stack &stack) {
      union { uint32_t ival; uint8_t bytes[4];};
      ival = value;
      memcpy(stack.Push(sizeof(value)), bytes, sizeof(bytes));
    }
    /*
     * one byte 0x80 | index
     */
    void writeIndexTag(const Field *pField) {

      if (pField->_written) {

        Stack &stack = _dataStack;
        uint8_t* ptr = stack.Push(1);
        ptr[0] = pField->index | UPPER_BIT;

      } else {
        Stack &stack = _hdrStack;
        size_t namelen = pField->name.length();

        uint8_t tagbyte = CrowTag::THFIELD;
        if (pField->subid > 0) { tagbyte |= FIELDINFO_FLAG_HAS_SUBID; }
        if (namelen > 0) { tagbyte |= FIELDINFO_FLAG_HAS_NAME; }
        if (pField->isRaw) { tagbyte |= FIELDINFO_FLAG_RAW; }

        uint8_t* ptr = stack.Push(2);
        *ptr++ = tagbyte;
        *ptr++ = pField->index;

        // typeid
        ptr = stack.Push(1);
        ptr[0] = pField->typeId;

        // id and subid (if set)
        writeVarInt(pField->id, stack);
        if (pField->subid > 0) { writeVarInt(pField->subid, stack); }

        // name (if set)
        if (namelen > 0) {
          writeVarInt(namelen, stack);
          ptr = stack.Push(namelen);
          memcpy(ptr,pField->name.c_str(),namelen);
        }

        if (pField->isRaw && pField->fixedLen > 0) {
          writeVarInt(pField->fixedLen, stack);
        }
        // mark as written, so we dont write FIELDINFO more than once
        ((Field*)pField)->_written = true;

        if (!pField->isRaw) {
          // field index on data row
          ptr = _dataStack.Push(1);
          ptr[0] = pField->index | UPPER_BIT;
        }
      }
    }

    size_t writeVarInt(uint64_t value, Stack &stack) {
      size_t i=0;
      while (true) {
        i++;
        uint8_t b = (uint8_t)(value & 0x07F);
        value = value >> 7;
        if (0L == value) {
          uint8_t* ptr = stack.Push(1);
          *ptr = b;
          break;
        }
        uint8_t* p = stack.Push(1);
        *p = (b | 0x80u);
      }

      return i;
    }

    Stack _stack, _dataStack, _hdrStack;
    std::vector<Field> _fields;
    std::vector<Field> _structFields;
    bool _haveStructData;
    size_t _structLen;
    bool _structDefFinalized;
    Stack _structBuf;
  };

  class EncoderFactory {
  public:
    static Encoder* New(size_t initialCapacity = 4096) { return new EncoderImpl(initialCapacity); }
  };

}
#endif // _CROW_ENCODE_IMPL_HPP_