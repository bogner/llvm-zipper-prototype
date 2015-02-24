//===-- MIUtilStreamStdin.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

//++
// File:        MIUtilStreamStdin.h
//
// Overview:    CMICmnStreamStdin interface.
//
// Environment: Compilers:  Visual C++ 12.
//                          gcc (Ubuntu/Linaro 4.8.1-10ubuntu9) 4.8.1
//              Libraries:  See MIReadmetxt.
//
// Copyright:   None.
//--

#pragma once

// In-house headers:
#include "MIUtilString.h"
#include "MIUtilThreadBaseStd.h"
#include "MICmnBase.h"
#include "MIUtilSingletonBase.h"

//++ ============================================================================
// Details: MI common code class. Used to handle stream data from Stdin.
//          Singleton class using the Visitor pattern. A driver using the interface
//          provide can receive callbacks when a new line of data is received.
//          Each line is determined by a carriage return.
//          A singleton class.
// Gotchas: None.
// Authors: Illya Rudkin 10/02/2014.
// Changes: Factored out OS specific handling of reading stdin  - IOR 16/06/2014.
//--
class CMICmnStreamStdin : public CMICmnBase, public MI::ISingleton<CMICmnStreamStdin>
{
    // Give singleton access to private constructors
    friend MI::ISingleton<CMICmnStreamStdin>;

    // Methods:
  public:
    bool Initialize(void);
    bool Shutdown(void);
    //
    const CMIUtilString &GetPrompt(void) const;
    bool SetPrompt(const CMIUtilString &vNewPrompt);
    void SetEnablePrompt(const bool vbYes);
    bool GetEnablePrompt(void) const;
    const MIchar *ReadLine(CMIUtilString &vwErrMsg);

    // Methods:
  private:
    /* ctor */ CMICmnStreamStdin(void);
    /* ctor */ CMICmnStreamStdin(const CMICmnStreamStdin &);
    void operator=(const CMICmnStreamStdin &);

    // Overridden:
  private:
    // From CMICmnBase
    /* dtor */ virtual ~CMICmnStreamStdin(void);

    // Attributes:
  private:
    CMIUtilString m_strPromptCurrent; // Command line prompt as shown to the user
    bool m_bShowPrompt;               // True = Yes prompt is shown/output to the user (stdout), false = no prompt
    bool m_bRedrawPrompt;             // True = Prompt needs to be redrawn
    static const int m_constBufferSize = 2048;
    MIchar *m_pCmdBuffer;
};
