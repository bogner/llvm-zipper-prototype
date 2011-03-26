//===-- Opcode.h ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef lldb_Opcode_h
#define lldb_Opcode_h

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-public.h"

namespace lldb_private {

    class Opcode
    {
    public:
        enum Type
        {
            eTypeInvalid,
            eType8,
            eType16,
            eType32,
            eType64,
            eTypeBytes
        };
        
        Opcode () : m_type (eTypeInvalid)
        {
        }

        Opcode (uint8_t inst) : m_type (eType8)
        {
            m_data.inst8 = inst;
        }

        Opcode (uint16_t inst) : m_type (eType16)
        {
            m_data.inst16 = inst;
        }

        Opcode (uint32_t inst) : m_type (eType32)
        {
            m_data.inst32 = inst;
        }

        Opcode (uint64_t inst) : m_type (eType64)
        {
            m_data.inst64 = inst;
        }

        Opcode (uint8_t *bytes, size_t length)
        {
            SetOpcodeBytes (bytes, length);
        }

        Opcode::Type
        GetType () const
        {
            return m_type;
        }
    
        uint8_t
        GetOpcode8 (uint8_t invalid_opcode = UINT8_MAX) const
        {
            switch (m_type)
            {
            case Opcode::eTypeInvalid:  break;
            case Opcode::eType8:        return m_data.inst8;
            case Opcode::eType16:       break;
            case Opcode::eType32:       break;
            case Opcode::eType64:       break;
            case Opcode::eTypeBytes:    break;
                break;
            }
            return invalid_opcode;
        }

        uint16_t
        GetOpcode16 (uint16_t invalid_opcode = UINT16_MAX) const
        {
            switch (m_type)
            {
            case Opcode::eTypeInvalid:  break;
            case Opcode::eType8:        return m_data.inst8;
            case Opcode::eType16:       return m_data.inst16;
            case Opcode::eType32:       break;
            case Opcode::eType64:       break;
            case Opcode::eTypeBytes:    break;
            }
            return invalid_opcode;
        }

        uint32_t
        GetOpcode32 (uint32_t invalid_opcode = UINT32_MAX) const
        {
            switch (m_type)
            {
            case Opcode::eTypeInvalid:  break;
            case Opcode::eType8:        return m_data.inst8;
            case Opcode::eType16:       return m_data.inst16;
            case Opcode::eType32:       return m_data.inst32;
            case Opcode::eType64:       break;
            case Opcode::eTypeBytes:    break;
            }
            return invalid_opcode;
        }

        uint64_t
        GetOpcode64 (uint64_t invalid_opcode = UINT64_MAX) const
        {
            switch (m_type)
            {
            case Opcode::eTypeInvalid:  break;
            case Opcode::eType8:        return m_data.inst8;
            case Opcode::eType16:       return m_data.inst16;
            case Opcode::eType32:       return m_data.inst32;
            case Opcode::eType64:       return m_data.inst64;
            case Opcode::eTypeBytes:    break;
            }
            return invalid_opcode;
        }

        void
        SetOpcode8 (uint8_t inst)
        {
            m_type = eType8;
            m_data.inst8 = inst;
        }

        void
        SetOpcode16 (uint16_t inst)
        {
            m_type = eType16;
            m_data.inst16 = inst;
        }

        void
        SetOpcode32 (uint32_t inst)
        {
            m_type = eType32;
            m_data.inst32 = inst;
        }

        void
        SetOpcode64 (uint64_t inst)
        {
            m_type = eType64;
            m_data.inst64 = inst;
        }

        void
        SetOpcodeBytes (const void *bytes, size_t length)
        {
            if (bytes && length > 0)
            {
                m_type = eTypeBytes;
                m_data.inst.length = length;
                assert (length < sizeof (m_data.inst.bytes));
                memcpy (m_data.inst.bytes, bytes, length);
            }
            else
            {
                m_type = eTypeInvalid;
                m_data.inst.length = 0;
            }
        }

        int
        Dump (Stream *s, uint32_t min_byte_width);

        const void *
        GetOpcodeBytes () const
        {
            if (m_type == Opcode::eTypeBytes)
                return m_data.inst.bytes;
            return NULL;
        }
        
        uint32_t
        GetByteSize () const
        {
            switch (m_type)
            {
            case Opcode::eTypeInvalid: break;
            case Opcode::eType8:     return sizeof(m_data.inst8);
            case Opcode::eType16:    return sizeof(m_data.inst16);
            case Opcode::eType32:    return sizeof(m_data.inst32);
            case Opcode::eType64:    return sizeof(m_data.inst64);
            case Opcode::eTypeBytes: return m_data.inst.length;
            }
            return 0;
        }


    protected:

        Opcode::Type m_type;
        union
        {
            uint8_t inst8;
            uint16_t inst16;
            uint32_t inst32;
            uint64_t inst64;
            struct 
            {
                uint8_t length;
                uint8_t bytes[16]; // This must be big enough to handle any opcode for any supported target.
            } inst;
        } m_data;
    };

} // namespace lldb_private

#endif	// lldb_Opcode_h
