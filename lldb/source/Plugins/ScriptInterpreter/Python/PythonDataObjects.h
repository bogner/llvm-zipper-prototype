//===-- PythonDataObjects.h----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PLUGINS_SCRIPTINTERPRETER_PYTHON_PYTHONDATAOBJECTS_H
#define LLDB_PLUGINS_SCRIPTINTERPRETER_PYTHON_PYTHONDATAOBJECTS_H

// C Includes
// C++ Includes

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-defines.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/StructuredData.h"
#include "lldb/Core/Flags.h"
#include "lldb/Interpreter/OptionValue.h"

namespace lldb_private {
class PythonString;
class PythonList;
class PythonDictionary;
class PythonInteger;

class StructuredPythonObject : public StructuredData::Generic
{
  public:
    StructuredPythonObject()
        : StructuredData::Generic()
    {
    }

    StructuredPythonObject(void *obj)
        : StructuredData::Generic(obj)
    {
        Py_XINCREF(GetValue());
    }

    virtual ~StructuredPythonObject()
    {
        if (Py_IsInitialized())
            Py_XDECREF(GetValue());
        SetValue(nullptr);
    }

    bool
    IsValid() const override
    {
        return GetValue() && GetValue() != Py_None;
    }

    void Dump(Stream &s) const override;

  private:
    DISALLOW_COPY_AND_ASSIGN(StructuredPythonObject);
};

enum class PyObjectType
{
    Unknown,
    None,
    Integer,
    Dictionary,
    List,
    String
};

enum class PyRefType
{
    Borrowed, // We are not given ownership of the incoming PyObject.
              // We cannot safely hold it without calling Py_INCREF.
    Owned     // We have ownership of the incoming PyObject.  We should
              // not call Py_INCREF.
};

enum class PyInitialValue
{
    Invalid,
    Empty
};

class PythonObject
{
public:
    PythonObject()
        : m_py_obj(nullptr)
    {
    }

    PythonObject(PyRefType type, PyObject *py_obj)
        : m_py_obj(nullptr)
    {
        Reset(type, py_obj);
    }

    PythonObject(const PythonObject &rhs)
        : m_py_obj(nullptr)
    {
        Reset(rhs);
    }

    virtual ~PythonObject() { Reset(); }

    void
    Reset()
    {
        // Avoid calling the virtual method since it's not necessary
        // to actually validate the type of the PyObject if we're
        // just setting to null.
        if (Py_IsInitialized())
            Py_XDECREF(m_py_obj);
        m_py_obj = nullptr;
    }

    void
    Reset(const PythonObject &rhs)
    {
        // Avoid calling the virtual method if it's not necessary
        // to actually validate the type of the PyObject.
        if (!rhs.get())
            Reset();
        else
            Reset(PyRefType::Borrowed, rhs.m_py_obj);
    }

    // PythonObject is implicitly convertible to PyObject *, which will call the
    // wrong overload.  We want to explicitly disallow this, since a PyObject
    // *always* owns its reference.  Therefore the overload which takes a
    // PyRefType doesn't make sense, and the copy constructor should be used.
    void
    Reset(PyRefType type, const PythonObject &ref) = delete;

    virtual void
    Reset(PyRefType type, PyObject *py_obj)
    {
        if (py_obj == m_py_obj)
            return;

        if (Py_IsInitialized())
            Py_XDECREF(m_py_obj);

        m_py_obj = py_obj;

        // If this is a borrowed reference, we need to convert it to
        // an owned reference by incrementing it.  If it is an owned
        // reference (for example the caller allocated it with PyDict_New()
        // then we must *not* increment it.
        if (Py_IsInitialized() && type == PyRefType::Borrowed)
            Py_XINCREF(m_py_obj);
    }
        
    void
    Dump () const
    {
        if (m_py_obj)
            _PyObject_Dump (m_py_obj);
        else
            puts ("NULL");
    }
        
    void
    Dump (Stream &strm) const;

    PyObject*
    get () const
    {
        return m_py_obj;
    }

    PyObjectType GetObjectType() const;

    PythonString
    Repr ();
        
    PythonString
    Str ();

    PythonObject &
    operator=(const PythonObject &other)
    {
        Reset(PyRefType::Borrowed, other.get());
        return *this;
    }

    bool
    IsValid() const;

    bool
    IsAllocated() const;

    bool
    IsNone() const;

    StructuredData::ObjectSP CreateStructuredObject() const;

protected:
    PyObject* m_py_obj;
};

class PythonString : public PythonObject
{
public:
    PythonString();
    PythonString(PyRefType type, PyObject *o);
    PythonString(const PythonString &object);
    explicit PythonString(llvm::StringRef string);
    explicit PythonString(const char *string);
    ~PythonString() override;

    static bool Check(PyObject *py_obj);

    // Bring in the no-argument base class version
    using PythonObject::Reset;

    void Reset(PyRefType type, PyObject *py_obj) override;

    llvm::StringRef
    GetString() const;

    size_t
    GetSize() const;

    void SetString(llvm::StringRef string);

    StructuredData::StringSP CreateStructuredString() const;
};

class PythonInteger : public PythonObject
{
public:
    PythonInteger();
    PythonInteger(PyRefType type, PyObject *o);
    PythonInteger(const PythonInteger &object);
    explicit PythonInteger(int64_t value);
    ~PythonInteger() override;

    static bool Check(PyObject *py_obj);

    // Bring in the no-argument base class version
    using PythonObject::Reset;

    void Reset(PyRefType type, PyObject *py_obj) override;

    int64_t GetInteger() const;

    void
    SetInteger (int64_t value);

    StructuredData::IntegerSP CreateStructuredInteger() const;
};

class PythonList : public PythonObject
{
public:
    PythonList(PyInitialValue value);
    PythonList(PyRefType type, PyObject *o);
    PythonList(const PythonList &list);
    ~PythonList() override;

    static bool Check(PyObject *py_obj);

    // Bring in the no-argument base class version
    using PythonObject::Reset;

    void Reset(PyRefType type, PyObject *py_obj) override;

    uint32_t GetSize() const;

    PythonObject GetItemAtIndex(uint32_t index) const;

    void SetItemAtIndex(uint32_t index, const PythonObject &object);

    void AppendItem(const PythonObject &object);

    StructuredData::ArraySP CreateStructuredArray() const;
};

class PythonDictionary : public PythonObject
{
public:
    PythonDictionary(PyInitialValue value);
    PythonDictionary(PyRefType type, PyObject *o);
    PythonDictionary(const PythonDictionary &dict);
    ~PythonDictionary() override;

    static bool Check(PyObject *py_obj);

    // Bring in the no-argument base class version
    using PythonObject::Reset;

    void Reset(PyRefType type, PyObject *py_obj) override;

    uint32_t GetSize() const;

    PythonList GetKeys() const;

    PythonObject GetItemForKey(const PythonObject &key) const;
    void SetItemForKey(const PythonObject &key, const PythonObject &value);

    StructuredData::DictionarySP CreateStructuredDictionary() const;
};
} // namespace lldb_private

#endif  // LLDB_PLUGINS_SCRIPTINTERPRETER_PYTHON_PYTHONDATAOBJECTS_H
