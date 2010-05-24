//===------------------------ exception.cpp -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <stdlib.h>

#include "exception"

#if __APPLE__
  #include <cxxabi.h>
  using namespace __cxxabiv1;
  // On Darwin, there are two STL shared libraries and a lower level ABI
  // shared libray.  The globals holding the current terminate handler and
  // current unexpected handler are in the ABI library.
  #define __terminate_handler  __cxxabiapple::__cxa_terminate_handler
  #define __unexpected_handler __cxxabiapple::__cxa_unexpected_handler
#else
  static std::terminate_handler  __terminate_handler;
  static std::unexpected_handler __unexpected_handler;
#endif



std::unexpected_handler 
std::set_unexpected(std::unexpected_handler func) throw()
{
    std::terminate_handler old = __unexpected_handler;
    __unexpected_handler = func;
    return old;
}

void
std::unexpected()
{
    (*__unexpected_handler)();
	// unexpected handler should not return
    std::terminate();
}


std::terminate_handler 
std::set_terminate(std::terminate_handler func) throw()
{
    std::terminate_handler old = __terminate_handler;
    __terminate_handler = func;
    return old;
}


void
std::terminate()
{
    try {
        (*__terminate_handler)();
        // handler should not return
        ::abort ();
    } 
    catch (...) {
        // handler should not throw exception
        ::abort ();
    }
}


bool std::uncaught_exception() throw()
{
#if __APPLE__
	// on Darwin, there is a helper function so __cxa_get_globals is private
    return __cxxabiapple::__cxa_uncaught_exception();
#else
    #warning uncaught_exception not yet implemented
    ::abort();
    // Not provided by Ubuntu gcc-4.2.4's cxxabi.h.
    // __cxa_eh_globals * globals = __cxa_get_globals();
    // return (globals->uncaughtExceptions != 0);
#endif
}


namespace std 
{


exception::~exception() throw() 
{ 
}

bad_exception::~bad_exception() throw() 
{ 
}

const char* exception::what() const throw()
{
  return "std::exception";
}

const char* bad_exception::what() const throw()
{
  return "std::bad_exception";
}



exception_ptr::~exception_ptr()
{
#if __APPLE__
    __cxxabiapple::__cxa_decrement_exception_refcount(__ptr_);
#else
	#warning exception_ptr not yet implemented
	::abort();
#endif
}

exception_ptr::exception_ptr(const exception_ptr& other)
    : __ptr_(other.__ptr_)
{
#if __APPLE__
    __cxxabiapple::__cxa_increment_exception_refcount(__ptr_);
#else
	#warning exception_ptr not yet implemented
	::abort();
#endif
}

exception_ptr& exception_ptr::operator=(const exception_ptr& other)
{
#if __APPLE__
    if (__ptr_ != other.__ptr_)
    {
        __cxxabiapple::__cxa_increment_exception_refcount(other.__ptr_);
        __cxxabiapple::__cxa_decrement_exception_refcount(__ptr_);
		__ptr_ = other.__ptr_;
	}
    return *this;
#else
	#warning exception_ptr not yet implemented
	::abort();
#endif
}

} // std


std::exception_ptr std::current_exception()
{
#if __APPLE__
	// be nicer if there was a constructor that took a ptr, then 
	// this whole function would be just:
	//    return exception_ptr(__cxa_current_primary_exception());
    std::exception_ptr ptr;
	ptr.__ptr_ = __cxxabiapple::__cxa_current_primary_exception();
	return ptr;
#else
	#warning exception_ptr not yet implemented
	::abort();
#endif
}

void std::rethrow_exception(exception_ptr p)
{
#if __APPLE__
	__cxxabiapple::__cxa_rethrow_primary_exception(p.__ptr_); 
	// if p.__ptr_ is NULL, above returns so we terminate
    terminate(); 
#else
	#warning exception_ptr not yet implemented
	::abort();
#endif
}
