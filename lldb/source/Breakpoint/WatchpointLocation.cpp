//===-- WatchpointLocation.cpp ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/WatchpointLocation.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Core/Stream.h"

using namespace lldb;
using namespace lldb_private;

WatchpointLocation::WatchpointLocation (lldb::addr_t addr, size_t size, bool hardware) :
    StoppointLocation (GetNextID(), addr, size, hardware),
    m_target(NULL),
    m_enabled(0),
    m_is_hardware(hardware),
    m_watch_read(0),
    m_watch_write(0),
    m_watch_was_read(0),
    m_watch_was_written(0),
    m_ignore_count(0),
    m_callback(NULL),
    m_callback_baton(NULL)
{
}

WatchpointLocation::~WatchpointLocation()
{
}

break_id_t
WatchpointLocation::GetNextID()
{
    static break_id_t g_next_ID = 0;
    return ++g_next_ID;
}

bool
WatchpointLocation::SetCallback (WatchpointHitCallback callback, void *callback_baton)
{
    m_callback = callback;
    m_callback_baton = callback_baton;
    return true;
}

void
WatchpointLocation::SetDeclInfo (std::string &str)
{
    m_decl_str = str;
    return;
}


bool
WatchpointLocation::IsHardware () const
{
    return m_is_hardware;
}

// RETURNS - true if we should stop at this breakpoint, false if we
// should continue.

bool
WatchpointLocation::ShouldStop (StoppointCallbackContext *context)
{
    ++m_hit_count;

    if (!IsEnabled())
        return false;

    if (m_hit_count <= GetIgnoreCount())
        return false;

    uint32_t access = 0;
    if (m_watch_was_read)
        access |= LLDB_WATCH_TYPE_READ;
    if (m_watch_was_written)
        access |= LLDB_WATCH_TYPE_WRITE;

    if (m_callback)
        return m_callback(m_callback_baton, context, GetID(), access);
    else
        return true;
}

void
WatchpointLocation::GetDescription (Stream *s, lldb::DescriptionLevel level)
{
    DumpWithLevel(s, level);
    return;
}

void
WatchpointLocation::Dump(Stream *s) const
{
    DumpWithLevel(s, lldb::eDescriptionLevelBrief);
}

void
WatchpointLocation::DumpWithLevel(Stream *s, lldb::DescriptionLevel description_level) const
{
    if (s == NULL)
        return;

    assert(description_level >= lldb::eDescriptionLevelBrief &&
           description_level <= lldb::eDescriptionLevelVerbose);

    s->Printf("WatchpointLocation %u: addr = 0x%8.8llx size = %zu state = %s type = %s%s",
              GetID(),
              (uint64_t)m_addr,
              m_byte_size,
              m_enabled ? "enabled" : "disabled",
              m_watch_read ? "r" : "",
              m_watch_write ? "w" : "");

    if (description_level >= lldb::eDescriptionLevelFull)
        s->Printf("\n    declare @ '%s'", m_decl_str.c_str());

    if (description_level >= lldb::eDescriptionLevelVerbose)
        s->Printf("\n    hw_index = %i  hit_count = %-4u  ignore_count = %-4u  callback = %8p baton = %8p",
                  GetHardwareIndex(),
                  GetHitCount(),
                  GetIgnoreCount(),
                  m_callback,
                  m_callback_baton);
}

bool
WatchpointLocation::IsEnabled() const
{
    return m_enabled;
}

void
WatchpointLocation::SetEnabled(bool enabled)
{
    if (!enabled)
        SetHardwareIndex(LLDB_INVALID_INDEX32);
    m_enabled = enabled;
}

void
WatchpointLocation::SetWatchpointType (uint32_t type)
{
    m_watch_read = (type & LLDB_WATCH_TYPE_READ) != 0;
    m_watch_write = (type & LLDB_WATCH_TYPE_WRITE) != 0;
}

bool
WatchpointLocation::WatchpointRead () const
{
    return m_watch_read != 0;
}
bool
WatchpointLocation::WatchpointWrite () const
{
    return m_watch_write != 0;
}
uint32_t
WatchpointLocation::GetIgnoreCount () const
{
    return m_ignore_count;
}

void
WatchpointLocation::SetIgnoreCount (uint32_t n)
{
    m_ignore_count = n;
}
