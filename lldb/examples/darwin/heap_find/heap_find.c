//===-- head_find.c ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file compiles into a dylib and can be used on darwin to find data that
// is contained in active malloc blocks. To use this make the project, then
// load the shared library in a debug session while you are stopped:
//
// (lldb) process load /path/to/libheap.dylib
//
// Now you can use the "find_pointer_in_heap" and "find_cstring_in_heap" 
// functions in the expression parser.
//
// This will grep everything in all active allocation blocks and print and 
// malloc blocks that contain the pointer 0x112233000000:
//
// (lldb) expression find_pointer_in_heap (0x112233000000)
//
// This will grep everything in all active allocation blocks and print and 
// malloc blocks that contain the C string "hello" (as a substring, no
// NULL termination included):
//
// (lldb) expression find_cstring_in_heap ("hello")
//
// The results will be printed to the STDOUT of the inferior program.
//
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <ctype.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>

struct range_callback_info_t;

typedef void range_callback_t (task_t task, void *baton, unsigned type, uint64_t ptr_addr, uint64_t ptr_size);
typedef void zone_callback_t (void *info, const malloc_zone_t *zone);

typedef struct range_callback_info_tag
{
    zone_callback_t *zone_callback;
    range_callback_t *range_callback;
    void *baton;
} range_callback_info_t;

typedef enum data_type
{
    eDataTypeBytes,
    eDataTypeCStr,
    eDataTypeInteger
} data_type_t;

typedef struct range_contains_data_callback_info_tag
{
    const uint8_t *data;
    const size_t data_len;
    const uint32_t align;
    const data_type_t data_type;
    uint32_t match_count;
} range_contains_data_callback_info_t;


static kern_return_t
task_peek (task_t task, vm_address_t remote_address, vm_size_t size, void **local_memory)
{
    *local_memory = (void*) remote_address;
    return KERN_SUCCESS;
}


static const void
foreach_zone_in_this_process (range_callback_info_t *info)
{
    //printf ("foreach_zone_in_this_process ( info->zone_callback = %p, info->range_callback = %p, info->baton = %p)", info->zone_callback, info->range_callback, info->baton);
    if (info == NULL || info->zone_callback == NULL)
        return;

    vm_address_t *zones = NULL;
    unsigned int num_zones = 0;
        
    kern_return_t err = malloc_get_all_zones (0, task_peek, &zones, &num_zones);
    if (KERN_SUCCESS == err)
    {
        for (unsigned int i=0; i<num_zones; ++i)
        {
            info->zone_callback (info, (const malloc_zone_t *)zones[i]);
        }
    }
}

static void
range_callback (task_t task, void *baton, unsigned type, uint64_t ptr_addr, uint64_t ptr_size)
{
    printf ("task = 0x%4.4x: baton = %p, type = %u, ptr_addr = 0x%llx + 0x%llu\n", task, baton, type, ptr_addr, ptr_size);
}

static void 
ranges_callback (task_t task, void *baton, unsigned type, vm_range_t *ptrs, unsigned count) 
{
    range_callback_info_t *info = (range_callback_info_t *)baton;
    while(count--) {
        info->range_callback (task, info->baton, type, ptrs->address, ptrs->size);
        ptrs++;
    }
}

static void
enumerate_range_in_zone (void *baton, const malloc_zone_t *zone)
{
    range_callback_info_t *info = (range_callback_info_t *)baton;

    if (zone && zone->introspect)
        zone->introspect->enumerator (mach_task_self(), 
                                      info, 
                                      MALLOC_PTR_IN_USE_RANGE_TYPE, 
                                      (vm_address_t)zone, 
                                      task_peek, 
                                      ranges_callback);    
}

const void
foreach_range_in_this_process (range_callback_t *callback, void *baton)
{
    range_callback_info_t info = { enumerate_range_in_zone, callback ? callback : range_callback, baton };
    foreach_zone_in_this_process (&info);
}

static void
range_contains_ptr_callback (task_t task, void *baton, unsigned type, uint64_t ptr_addr, uint64_t ptr_size)
{
    uint8_t *data = NULL;
    range_contains_data_callback_info_t *data_info = (range_contains_data_callback_info_t *)baton;
    if (data_info->data_len <= 0)
    {
        printf ("error: invalid data size: %zu\n", data_info->data_len);
    }
    else if (data_info->data_len > ptr_size)
    {
        // This block is too short to contain the data we are looking for...
        return;
    }
    else if (task_peek (task, ptr_addr, ptr_size, (void **)&data) == KERN_SUCCESS)
    {
        assert (data);
        const uint64_t end_addr = ptr_addr + ptr_size;
        for (uint64_t addr = ptr_addr; 
             addr < end_addr && ((end_addr - addr) >= data_info->data_len);
             addr += data_info->align, data += data_info->align)
        {
            if (memcmp (data_info->data, data, data_info->data_len) == 0)
            {
                ++data_info->match_count;
                printf ("0x%llx: ", addr);
                uint32_t i;
                switch (data_info->data_type)
                {
                case eDataTypeInteger:
                    {
                        // NOTE: little endian specific, but all darwin platforms are little endian now..
                        for (i=0; i<data_info->data_len; ++i)
                            printf (i ? "%2.2x" : "0x%2.2x", data[data_info->data_len - (i + 1)]);
                    }
                    break;
                case eDataTypeBytes:
                    {
                        for (i=0; i<data_info->data_len; ++i)
                            printf (" %2.2x", data[i]);
                    }
                    break;
                case eDataTypeCStr:
                    {
                        putchar ('"');
                        for (i=0; i<data_info->data_len; ++i)
                        {
                            if (isprint (data[i]))
                                putchar (data[i]);
                            else
                                printf ("\\x%2.2x", data[i]);
                        }
                        putchar ('"');
                    }
                    break;
                    
                }
                printf (" found in malloc block 0x%llx + %llu (malloc_size = %llu)\n", ptr_addr, addr - ptr_addr, ptr_size);
            }
        }
    }
    else
    {
        printf ("0x%llx: error: couldn't read %llu bytes\n", ptr_addr, ptr_size);
    }   
}


typedef uint64_t MachMallocEventId;

enum MachMallocEventType
{
    eMachMallocEventTypeAlloc = 2,
    eMachMallocEventTypeDealloc = 4,
    eMachMallocEventTypeOther = 1
};

struct MachMallocEvent
{
    mach_vm_address_t m_base_address;
    uint64_t m_size;
    MachMallocEventType m_event_type;
    MachMallocEventId m_event_id;
};

static void foundStackLog(mach_stack_logging_record_t record, void *context) {
    *((bool*)context) = true;
}

bool
malloc_stack_logging_is_enabled ()
{
    bool found = false;
    __mach_stack_logging_enumerate_records(m_task, 0x0, foundStackLog, &found);
    return found;
}

struct history_enumerator_impl_data
{
    MachMallocEvent *buffer;
    uint32_t        *position;
    uint32_t         count;
};

static void 
history_enumerator_impl(mach_stack_logging_record_t record, void* enum_obj)
{
    history_enumerator_impl_data *data = (history_enumerator_impl_data*)enum_obj;
    
    if (*data->position >= data->count)
        return;
    
    data->buffer[*data->position].m_base_address = record.address;
    data->buffer[*data->position].m_size = record.argument;
    data->buffer[*data->position].m_event_id = record.stack_identifier;
    data->buffer[*data->position].m_event_type = record.type_flags == stack_logging_type_alloc ?   eMachMallocEventTypeAlloc :
                                                 record.type_flags == stack_logging_type_dealloc ? eMachMallocEventTypeDealloc :
                                                                                                   eMachMallocEventTypeOther;
    *data->position+=1;
}

bool
MachTask::EnumerateMallocRecords (MachMallocEvent *event_buffer,
                                  uint32_t buffer_size,
                                  uint32_t *count)
{
    return EnumerateMallocRecords(0,
                                  event_buffer,
                                  buffer_size,
                                  count);
}

bool
MachTask::EnumerateMallocRecords (mach_vm_address_t address,
                                  MachMallocEvent *event_buffer,
                                  uint32_t buffer_size,
                                  uint32_t *count)
{
    if (!event_buffer || !count)
        return false;
    
    if (buffer_size == 0)
        return false;
    
    *count = 0;
    history_enumerator_impl_data data = { event_buffer, count, buffer_size };
    __mach_stack_logging_enumerate_records(m_task, address, history_enumerator_impl, &data);
    return (*count > 0);
}

bool
MachTask::EnumerateMallocFrames (MachMallocEventId event_id,
                                 mach_vm_address_t *function_addresses_buffer,
                                 uint32_t buffer_size,
                                 uint32_t *count)
{
    if (!function_addresses_buffer || !count)
        return false;
    
    if (buffer_size == 0)
        return false;
    
    __mach_stack_logging_frames_for_uniqued_stack(m_task, event_id, &function_addresses_buffer[0], buffer_size, count);
    *count -= 1;
    if (function_addresses_buffer[*count-1] < vm_page_size)
        *count -= 1;
    return (*count > 0);
}

uint32_t
find_pointer_in_heap (intptr_t addr)
{
    range_contains_data_callback_info_t data_info = { (uint8_t *)&addr, sizeof(addr), sizeof(addr), eDataTypeInteger, 0};
    range_callback_info_t info = { enumerate_range_in_zone, range_contains_ptr_callback, &data_info };
    foreach_zone_in_this_process (&info);
    return data_info.match_count;
}

uint32_t
find_cstring_in_heap (const char *s)
{
    if (s && s[0])
    {
        range_contains_data_callback_info_t data_info = { (uint8_t *)s, strlen(s), 1, eDataTypeCStr, 0};
        range_callback_info_t info = { enumerate_range_in_zone, range_contains_ptr_callback, &data_info };
        foreach_zone_in_this_process (&info);
        return data_info.match_count;
    }
    else
    {
        printf ("error: invalid argument (empty cstring)\n");
    }
    return 0;
}
