//===-- PythonExceptionState.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb-python.h"
#include "PythonExceptionState.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb_private;

PythonExceptionState::PythonExceptionState(bool restore_on_exit)
    : m_restore_on_exit(restore_on_exit)
{
    Acquire(restore_on_exit);
}

PythonExceptionState::~PythonExceptionState()
{
    if (m_restore_on_exit)
        Restore();
}

void
PythonExceptionState::Acquire(bool restore_on_exit)
{
    // If a state is already acquired, the user needs to decide whether they
    // want to discard or restore it.  Don't allow the potential silent
    // loss of a valid state.
    assert(!IsError());

    if (!HasErrorOccurred())
        return;

    PyObject *py_type = nullptr;
    PyObject *py_value = nullptr;
    PyObject *py_traceback = nullptr;
    PyErr_Fetch(&py_type, &py_value, &py_traceback);
    // PyErr_Fetch clears the error flag.
    assert(!HasErrorOccurred());

    // Ownership of the objects returned by `PyErr_Fetch` is transferred
    // to us.
    m_type.Reset(PyRefType::Owned, py_type);
    m_value.Reset(PyRefType::Owned, py_value);
    m_traceback.Reset(PyRefType::Owned, py_traceback);
}

void
PythonExceptionState::Restore()
{
    if (m_type.IsValid())
    {
        // The documentation for PyErr_Restore says "Do not pass a null type and
        // non-null value or traceback.  So only restore if type was non-null
        // to begin with.  In this case we're passing ownership back to Python
        // so release them all.
        PyErr_Restore(m_type.release(), m_value.release(), m_traceback.release());
    }

    // After we restore, we should not hold onto the exception state.  Demand that
    // it be re-acquired.
    Discard();
}

void
PythonExceptionState::Discard()
{
    m_type.Reset();
    m_value.Reset();
    m_traceback.Reset();
}

bool
PythonExceptionState::HasErrorOccurred()
{
    return PyErr_Occurred();
}

bool
PythonExceptionState::IsError() const
{
    return m_type.IsValid() || m_value.IsValid() || m_traceback.IsValid();
}

PythonObject
PythonExceptionState::GetType() const
{
    return m_type;
}

PythonObject
PythonExceptionState::GetValue() const
{
    return m_value;
}

PythonObject
PythonExceptionState::GetTraceback() const
{
    return m_traceback;
}

std::string
PythonExceptionState::Format() const
{
    // Don't allow this function to modify the error state.
    PythonExceptionState state(true);

    std::string backtrace = ReadBacktrace();
    if (!IsError())
        return std::string();

    // It's possible that ReadPythonBacktrace generated another exception.
    // If this happens we have to clear the exception, because otherwise
    // PyObject_Str() will assert below.  That's why we needed to do the
    // save / restore at the beginning of this function.
    PythonExceptionState bt_error_state(false);

    std::string error_string;
    llvm::raw_string_ostream error_stream(error_string);
    error_stream << m_value.Str().GetString() << "\n";

    if (!bt_error_state.IsError())
    {
        // If we were able to read the backtrace, just append it.
        error_stream << backtrace << "\n";
    }
    else
    {
        // Otherwise, append some information about why we were unable to
        // obtain the backtrace.
        PythonString bt_error = bt_error_state.GetValue().Str();
        error_stream << "An error occurred while retrieving the backtrace: " << bt_error.GetString() << "\n";
    }
    return error_stream.str();
}

std::string
PythonExceptionState::ReadBacktrace() const
{
    PythonObject traceback_module;
    PythonObject stringIO_module;
    PythonObject stringIO_builder;
    PythonObject stringIO_buffer;
    PythonObject printTB;
    PythonObject printTB_args;
    PythonObject printTB_result;
    PythonObject stringIO_getvalue;
    PythonObject printTB_string;

    std::string retval("backtrace unavailable");

    if (!m_traceback.IsAllocated())
        return retval;

    traceback_module.Reset(PyRefType::Owned, PyImport_ImportModule("traceback"));
    stringIO_module.Reset(PyRefType::Owned, PyImport_ImportModule("StringIO"));
    if (!traceback_module.IsAllocated() || !stringIO_module.IsAllocated())
        return retval;

    stringIO_builder.Reset(PyRefType::Owned, PyObject_GetAttrString(stringIO_module.get(), "StringIO"));
    if (!stringIO_builder.IsAllocated())
        return retval;

    stringIO_buffer.Reset(PyRefType::Owned, PyObject_CallObject(stringIO_builder.get(), nullptr));
    if (!stringIO_buffer.IsAllocated())
        return retval;

    printTB.Reset(PyRefType::Owned, PyObject_GetAttrString(traceback_module.get(), "print_tb"));
    if (!printTB.IsAllocated())
        return retval;

    printTB_args.Reset(PyRefType::Owned, Py_BuildValue("OOO", m_traceback.get(), Py_None, stringIO_buffer.get()));
    printTB_result.Reset(PyRefType::Owned, PyObject_CallObject(printTB.get(), printTB_args.get()));
    stringIO_getvalue.Reset(PyRefType::Owned, PyObject_GetAttrString(stringIO_buffer.get(), "getvalue"));
    if (!stringIO_getvalue.IsAllocated())
        return retval;

    printTB_string.Reset(PyRefType::Owned, PyObject_CallObject(stringIO_getvalue.get(), nullptr));
    if (!printTB_string.IsAllocated())
        return retval;

    PythonString str(PyRefType::Borrowed, printTB_string.get());
    llvm::StringRef string_data(str.GetString());
    retval.assign(string_data.data(), string_data.size());

    return retval;
}