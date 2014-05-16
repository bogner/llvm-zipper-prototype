//===-- MICmnLLDBDebugSessionInfo.cpp ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

//++
// File:		MICmnLLDBDebugSessionInfo.cpp
//
// Overview:	CMICmnLLDBDebugSessionInfo implementation.
//
// Environment:	Compilers:	Visual C++ 12.
//							gcc (Ubuntu/Linaro 4.8.1-10ubuntu9) 4.8.1
//				Libraries:	See MIReadmetxt. 
//
// Copyright:	None.
//--

// Third party headers:
#include <lldb/API/SBThread.h>
#ifdef _WIN32
	#include <io.h>			// For the ::_access()
#else
	#include <unistd.h>		// For the ::access()
#endif // _WIN32

// In-house headers:
#include "MICmnLLDBDebugSessionInfo.h"
#include "MICmnLLDBDebugger.h"
#include "MICmnResources.h"
#include "MICmnMIResultRecord.h"
#include "MICmnMIValueConst.h"
#include "MICmnMIValueList.h"
#include "MICmnMIValueTuple.h"
#include "MICmdData.h"

//++ ------------------------------------------------------------------------------------
// Details:	CMICmnLLDBDebugSessionInfo constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmnLLDBDebugSessionInfo::CMICmnLLDBDebugSessionInfo( void )
// Todo: AD: Use of these singletons may need to be removed from the constructor
:	m_rLldbDebugger( CMICmnLLDBDebugger::Instance().GetTheDebugger() )
,	m_rLlldbListener( CMICmnLLDBDebugger::Instance().GetTheListener() )
,	m_nBrkPointCnt( 0 )
,	m_nBrkPointCntMax( INT32_MAX )
,	m_currentSelectedThread( LLDB_INVALID_THREAD_ID )
,	m_constStrSharedDataKeyWkDir( "Working Directory" )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmnLLDBDebugSessionInfo destructor.
// Type:	Overridable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmnLLDBDebugSessionInfo::~CMICmnLLDBDebugSessionInfo( void )
{
	Shutdown();
}

//++ ------------------------------------------------------------------------------------
// Details:	Initialize resources for *this broardcaster object.
// Type:	Method.
// Args:	None.
// Return:	MIstatus::success - Functionality succeeded.
//			MIstatus::failure - Functionality failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::Initialize( void )
{
	m_clientUsageRefCnt++;

	if( m_bInitialized )
		return MIstatus::success;

	m_nBrkPointCnt = 0;
	m_currentSelectedThread = LLDB_INVALID_THREAD_ID;
	CMICmnLLDBDebugSessionInfoVarObj::VarObjIdResetToZero();

	m_bInitialized = MIstatus::success;
	
	return m_bInitialized;
}

//++ ------------------------------------------------------------------------------------
// Details:	Release resources for *this broardcaster object.
// Type:	Method.
// Args:	None.
// Return:	MIstatus::success - Functionality succeeded.
//			MIstatus::failure - Functionality failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::Shutdown( void )
{
	if( --m_clientUsageRefCnt > 0 )
		return MIstatus::success;
	
	if( !m_bInitialized )
		return MIstatus::success;

	bool bOk = MIstatus::success;
	CMIUtilString errMsg;
	
	// Tidy up
	bOk = SharedDataDestroy();
	if( !bOk )
	{
		errMsg = CMIUtilString::Format( MIRSRC( IDS_DBGSESSION_ERR_SHARED_DATA_RELEASE ) );
		errMsg += "\n";
	}
	m_vecActiveThreadId.clear();
	CMICmnLLDBDebugSessionInfoVarObj::VarObjClear();

	m_bInitialized = false;

	return MIstatus::success;
}	

//++ ------------------------------------------------------------------------------------
// Details:	Command instances can create and share data between other instances of commands.
//			This function takes down those resources build up over the use of the commands.
//			This function should be called when the creation and running of command has 
//			stopped i.e. application shutdown.
// Type:	Method.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::SharedDataDestroy( void )
{
	m_mapKeyToStringValue.clear();
	m_vecVarObj.clear();

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Command instances can create and share data between other instances of commands.
//			This function adds new data to the shared data. Using the same ID more than
//			once replaces any previous matching data keys.
// Type:	Method.
// Args:	vKey	- (R) A non empty unique data key to retrieve by.
//			vData	- (R) Data to be added to the share.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::SharedDataAdd( const CMIUtilString & vKey, const CMIUtilString & vData )
{
	if( vKey.empty() )
		return MIstatus::failure;

	MapPairKeyToStringValue_t pr( vKey, vData );
	m_mapKeyToStringValue.insert( pr );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Command instances can create and share data between other instances of commands.
//			This function retrieves data from the shared data container.
// Type:	Method.
// Args:	vKey	- (R) A non empty unique data key to retrieve by.
//			vData	- (W) Data.
// Return:	bool - True = data found, false = key now found.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::SharedDataRetrieve( const CMIUtilString & vKey, CMIUtilString & vwData )
{
	const MapKeyToStringValue_t::const_iterator it = m_mapKeyToStringValue.find( vKey );
	if( it != m_mapKeyToStringValue.end() )
	{
		vwData = (*it).second;
		return true;
	}

	return false;
}

//++ ------------------------------------------------------------------------------------
// Details:	Retrieve the specified thread's frame information.
// Type:	Method.
// Args:	vCmdData		- (R) A command's information.
//			vThreadIdx		- (R) Thread index.
//			vwrThreadFrames	- (W) Frame data.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::GetThreadFrames( const SMICmdData & vCmdData, const MIuint vThreadIdx, CMICmnMIValueTuple & vwrThreadFrames ) 
{
	lldb::SBThread thread = m_lldbProcess.GetThreadByIndexID( vThreadIdx );
	const uint32_t nFrames = thread.GetNumFrames();
	if( nFrames == 0 )
	{
		vwrThreadFrames = CMICmnMIValueTuple();
		return MIstatus::success;
	}

	CMICmnMIValueTuple miValueTupleAll;
	for( MIuint nLevel = 0; nLevel < nFrames; nLevel++ )
	{
		lldb::SBFrame frame = thread.GetFrameAtIndex( nLevel );
		lldb::addr_t pc = 0;
		CMIUtilString fnName;
		CMIUtilString fileName;
		CMIUtilString path; 
		MIuint nLine = 0;
		if( !GetFrameInfo( frame, pc, fnName, fileName, path, nLine ) )
			return MIstatus::failure;

		// Function args
		CMICmnMIValueList miValueList( true );
		const MIuint vMaskVarTypes = 0x1000;
		if( !MIResponseFormVariableInfo( frame, vMaskVarTypes, miValueList ) )
			return MIstatus::failure;

		const MIchar * pUnknown = "??";
		if( fnName != pUnknown )
		{
			std::replace( fnName.begin(), fnName.end(), ')', ' ' );
			std::replace( fnName.begin(), fnName.end(), '(', ' ' );
			std::replace( fnName.begin(), fnName.end(), '\'', ' ' );
		}

		const CMIUtilString strLevel( CMIUtilString::Format( "%d", nLevel ) );
		const CMICmnMIValueConst miValueConst( strLevel );
		const CMICmnMIValueResult miValueResult( "level", miValueConst );
		miValueTupleAll.Add( miValueResult );
		
		CMICmnMIValueTuple miValueTuple( miValueResult );
		if( !MIResponseFormFrameInfo( pc, fnName, miValueList.GetString(), fileName, path, nLine, miValueTuple ) )
			return MIstatus::failure;
	}

	vwrThreadFrames = miValueTupleAll;

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Return the resolved file's path for the given file.
// Type:	Method.
// Args:	vCmdData		- (R) A command's information.
//			vPath			- (R) Original path.
//			vwrResolvedPath	- (W) Resolved path.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::ResolvePath( const SMICmdData & vCmdData, const CMIUtilString & vPath, CMIUtilString & vwrResolvedPath )
{
	// ToDo: Verify this code as it does not work as vPath is always empty

	CMIUtilString strResolvedPath;
	if( !SharedDataRetrieve( "Working Directory", strResolvedPath ) )
	{
		vwrResolvedPath = "";
		SetErrorDescription( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_SHARED_DATA_NOT_FOUND ), vCmdData.strMiCmd.c_str(), "Working Directory" ) );
		return MIstatus::failure;
	}

	vwrResolvedPath = vPath;

	return ResolvePath( strResolvedPath, vwrResolvedPath );
}

//++ ------------------------------------------------------------------------------------
// Details:	Return the resolved file's path for the given file.
// Type:	Method.
// Args:	vstrUnknown		- (R)	String assigned to path when resolved path is empty.
//			vwrResolvedPath	- (RW)	The original path overwritten with resolved path.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::ResolvePath( const CMIUtilString & vstrUnknown, CMIUtilString & vwrResolvedPath )
{
	if( vwrResolvedPath.size() < 1 )
	{
		vwrResolvedPath = vstrUnknown;
		return MIstatus::success;
	}

	bool bOk = MIstatus::success;

	CMIUtilString::VecString_t vecPathFolders;
	const MIuint nSplits = vwrResolvedPath.Split( "/", vecPathFolders );
	MIuint nFoldersBack = 1; // 1 is just the file (last element of vector)
	while( bOk && (vecPathFolders.size() >= nFoldersBack) )
	{
		CMIUtilString strTestPath;
		MIuint nFoldersToAdd = nFoldersBack;
		while( nFoldersToAdd > 0 )
		{
			strTestPath += "/";
			strTestPath += vecPathFolders[ vecPathFolders.size() - nFoldersToAdd ];
			nFoldersToAdd--;
		}
		bool bYesAccessible = false;
		bOk = AccessPath( strTestPath, bYesAccessible );
		if( bYesAccessible )
		{
			vwrResolvedPath = strTestPath;
			return MIstatus::success;
		}
		else
			nFoldersBack++;
	}
	
	// No files exist in the union of working directory and debuginfo path
    // Simply use the debuginfo path and let the IDE handle it.
    
	return bOk;
}

//++ ------------------------------------------------------------------------------------
// Details:	Determine the given file path exists or not.
// Type:	Method.
// Args:	vPath				- (R) File name path.
//			vwbYesAccessible	- (W) True - file exists, false = does not exist.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::AccessPath( const CMIUtilString & vPath, bool & vwbYesAccessible )
{
#ifdef _WIN32
	vwbYesAccessible = (::_access( vPath.c_str(), 0 ) == 0);
#else
	vwbYesAccessible = (::access( vPath.c_str(), 0 ) == 0);
#endif // _WIN32

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vCmdData		- (R) A command's information.
//			vrThread		- (R) LLDB thread object.
//			vwrMIValueTuple	- (W) MI value tuple object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormThreadInfo( const SMICmdData & vCmdData, const lldb::SBThread & vrThread, CMICmnMIValueTuple & vwrMIValueTuple )
{
	lldb::SBThread & rThread = const_cast< lldb::SBThread & >( vrThread );
	
	CMICmnMIValueTuple miValueTupleFrame;
	if( !GetThreadFrames( vCmdData, rThread.GetIndexID(), miValueTupleFrame ) )
		return MIstatus::failure;

	const bool bSuspended = rThread.IsSuspended();
	const lldb::StopReason eReason = rThread.GetStopReason();
	const bool bValidReason = !((eReason == lldb::eStopReasonNone) || (eReason == lldb::eStopReasonInvalid));
	const CMIUtilString strState( (bSuspended || bValidReason) ? "stopped" : "running" );
	
	// Add "id"
	const CMIUtilString strId( CMIUtilString::Format( "%d", rThread.GetIndexID() ) );
	const CMICmnMIValueConst miValueConst1( strId );
	const CMICmnMIValueResult miValueResult1( "id", miValueConst1 );
	if( !vwrMIValueTuple.Add( miValueResult1 ) )
		return MIstatus::failure;

	// Add "target-id"
	const char * pThreadName = rThread.GetName();
	const MIuint len = (pThreadName != nullptr) ? CMIUtilString( pThreadName ).length() : 0;
	const bool bHaveName = ((pThreadName != nullptr) && (len > 0) && (len < 32) && CMIUtilString::IsAllValidAlphaAndNumeric( *pThreadName ) );	// 32 is arbitary number 
	const MIchar * pThrdFmt = bHaveName ? "%s" : "Thread %d";	
	CMIUtilString strThread;
	if( bHaveName )
		strThread = CMIUtilString::Format( pThrdFmt, pThreadName );
	else
		strThread = CMIUtilString::Format( pThrdFmt, rThread.GetIndexID() );
	const CMICmnMIValueConst miValueConst2( strThread );
	const CMICmnMIValueResult miValueResult2( "target-id", miValueConst2 );
	if( !vwrMIValueTuple.Add( miValueResult2 ) )
		return MIstatus::failure;

	// Add "frame"
	const CMICmnMIValueResult miValueResult3( "frame", miValueTupleFrame );
	if( !vwrMIValueTuple.Add( miValueResult3 ) )
		return MIstatus::failure;

	// Add "state"
	const CMICmnMIValueConst miValueConst4( strState );
	const CMICmnMIValueResult miValueResult4( "state", miValueConst4 );
	if( !vwrMIValueTuple.Add( miValueResult4 ) )
		return MIstatus::failure;

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vrFrame			- (R)	LLDB thread object.
//			vMaskVarTypes	- (R)	0x1000 = arguments, 
//									0x0100 = locals,
//									0x0010 = statics,
//									0x0001 = in scope only.
//			vwrMIValueList	- (W)	MI value list object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormVariableInfo( const lldb::SBFrame & vrFrame, const MIuint vMaskVarTypes, CMICmnMIValueList & vwrMiValueList )
{
	bool bOk = MIstatus::success;
	lldb::SBFrame & rFrame = const_cast< lldb::SBFrame & >( vrFrame );
	
	const bool bArg = (vMaskVarTypes & 0x1000);
	const bool bLocals = (vMaskVarTypes & 0x0100);
	const bool bStatics = (vMaskVarTypes & 0x0010);
	const bool bInScopeOnly = (vMaskVarTypes & 0x0001);
	const MIchar * pUnkwn = "??";
	lldb::SBValueList listArg = rFrame.GetVariables( bArg, bLocals, bStatics, bInScopeOnly );
	const MIuint nArgs = listArg.GetSize();
	for( MIuint i = 0; bOk && (i < nArgs); i++ )
	{
		lldb::SBValue val = listArg.GetValueAtIndex( i );
		const char * pValue = val.GetValue();
		pValue = (pValue != nullptr) ? pValue : pUnkwn;
		const char * pName = val.GetName();
		pName = (pName != nullptr) ? pName : pUnkwn;
		const CMICmnMIValueConst miValueConst( pName );
		const CMICmnMIValueResult miValueResult( "name", miValueConst );
		CMICmnMIValueTuple miValueTuple( miValueResult );
		const CMICmnMIValueConst miValueConst2( pValue );
		const CMICmnMIValueResult miValueResult2( "value", miValueConst2 );
		miValueTuple.Add( miValueResult2 );
		bOk = vwrMiValueList.Add( miValueTuple );
	}

	return bOk;
}

//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vrThread		- (R) LLDB thread object.
//			vwrMIValueTuple	- (W) MI value tuple object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormFrameInfo( const lldb::SBThread & vrThread, const MIuint vnLevel, CMICmnMIValueTuple & vwrMiValueTuple )
{
	lldb::SBThread & rThread = const_cast< lldb::SBThread & >( vrThread );
	
	lldb::SBFrame frame = rThread.GetFrameAtIndex( vnLevel );
	lldb::addr_t pc = 0;
	CMIUtilString fnName;
	CMIUtilString fileName;
	CMIUtilString path; 
	MIuint nLine = 0;
	if( !GetFrameInfo( frame, pc, fnName, fileName, path, nLine ) )
		return MIstatus::failure;
	
	CMICmnMIValueList miValueList( true );
	const MIuint vMaskVarTypes = 0x1000;
	if( !MIResponseFormVariableInfo( frame, vMaskVarTypes, miValueList ) )
		return MIstatus::failure;

	// MI print "{level=\"0\",addr=\"0x%08llx\",func=\"%s\",args=[%s],file=\"%s\",fullname=\"%s\",line=\"%d\"}"
	const CMIUtilString strLevel( CMIUtilString::Format( "%d", vnLevel ) );
	const CMICmnMIValueConst miValueConst( strLevel );
	const CMICmnMIValueResult miValueResult( "level", miValueConst );
	CMICmnMIValueTuple miValueTuple( miValueResult );
	if( !MIResponseFormFrameInfo( pc, fnName, miValueList.GetString(), fileName, path, nLine, miValueTuple ) )
		return MIstatus::failure;

	vwrMiValueTuple = miValueTuple;

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Retrieve the frame information from LLDB frame object.
// Type:	Method.
// Args:	vrFrame			- (R) LLDB thread object.
//			vPc				- (W) Address number.
//			vFnName			- (W) Function name.
//			vFileName		- (W) File name text.
//			vPath			- (W) Full file name and path text.
//			vnLine			- (W) File line number.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::GetFrameInfo( const lldb::SBFrame & vrFrame, lldb::addr_t & vwPc, CMIUtilString & vwFnName, CMIUtilString & vwFileName, CMIUtilString & vwPath, MIuint & vwnLine )
{
	lldb::SBFrame & rFrame = const_cast< lldb::SBFrame & >( vrFrame );

	static char pBuffer[ MAX_PATH ];
	const MIuint nBytes = rFrame.GetLineEntry().GetFileSpec().GetPath( &pBuffer[ 0 ], sizeof( pBuffer ) );
	CMIUtilString strResolvedPath( &pBuffer[ 0 ] );
	const MIchar * pUnkwn = "??";
	if( !ResolvePath( pUnkwn, strResolvedPath ) )
		return MIstatus::failure;
	vwPath = strResolvedPath;

	vwPc = rFrame.GetPC();
	
	const MIchar * pFnName = rFrame.GetFunctionName();
	vwFnName = (pFnName != nullptr) ? pFnName : pUnkwn;
	
	const MIchar * pFileName = rFrame.GetLineEntry().GetFileSpec().GetFilename();
	vwFileName = (pFileName != nullptr) ? pFileName : pUnkwn;
	
	vwnLine = rFrame.GetLineEntry().GetLine();
	
	return MIstatus::success;
}
	
//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vPc				- (R) Address number.
//			vFnName			- (R) Function name.
//			vArgs			- (R) Variable information MI response.
//			vFileName		- (R) File name text.
//			vPath			- (R) Full file name and path text.
//			vnLine			- (R) File line number.
//			vwrMIValueTuple	- (W) MI value tuple object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormFrameInfo( const lldb::addr_t vPc, const CMIUtilString & vFnName, const CMIUtilString & vArgs, const CMIUtilString & vFileName, const CMIUtilString & vPath, const MIuint vnLine, CMICmnMIValueTuple & vwrMiValueTuple )
{
	const CMIUtilString strAddr( CMIUtilString::Format( "0x%08llx", vPc ) );
	const CMICmnMIValueConst miValueConst2( strAddr );
	const CMICmnMIValueResult miValueResult2( "addr", miValueConst2 );
	if( !vwrMiValueTuple.Add( miValueResult2 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst3( vFnName );
	const CMICmnMIValueResult miValueResult3( "func", miValueConst3 );
	if( !vwrMiValueTuple.Add( miValueResult3 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst8( vArgs, true );
	const CMICmnMIValueResult miValueResult4( "args", miValueConst8 );
	if( !vwrMiValueTuple.Add( miValueResult4 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst5( vFileName );
	const CMICmnMIValueResult miValueResult5( "file", miValueConst5 );
	if( !vwrMiValueTuple.Add( miValueResult5 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst6( vPath );
	const CMICmnMIValueResult miValueResult6( "fullname", miValueConst6 );
	if( !vwrMiValueTuple.Add( miValueResult6 ) )
		return MIstatus::failure;
	const CMIUtilString strLine( CMIUtilString::Format( "%d", vnLine ) );
	const CMICmnMIValueConst miValueConst7( strLine );
	const CMICmnMIValueResult miValueResult7( "line", miValueConst7 );
	if( !vwrMiValueTuple.Add( miValueResult7 ) )
		return MIstatus::failure;

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vPc				- (R) Address number.
//			vFnName			- (R) Function name.
//			vFileName		- (R) File name text.
//			vPath			- (R) Full file name and path text.
//			vnLine			- (R) File line number.
//			vwrMIValueTuple	- (W) MI value tuple object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormBrkPtFrameInfo( const lldb::addr_t vPc, const CMIUtilString & vFnName, const CMIUtilString & vFileName, const CMIUtilString & vPath, const MIuint vnLine, CMICmnMIValueTuple & vwrMiValueTuple )
{
	const CMIUtilString strAddr( CMIUtilString::Format( "0x%08llx", vPc ) );
	const CMICmnMIValueConst miValueConst2( strAddr );
	const CMICmnMIValueResult miValueResult2( "addr", miValueConst2 );
	if( !vwrMiValueTuple.Add( miValueResult2 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst3( vFnName );
	const CMICmnMIValueResult miValueResult3( "func", miValueConst3 );
	if( !vwrMiValueTuple.Add( miValueResult3 ) )
		return MIstatus::failure;
	const CMICmnMIValueConst miValueConst5( vFileName );
	const CMICmnMIValueResult miValueResult5( "file", miValueConst5 );
	if( !vwrMiValueTuple.Add( miValueResult5 ) )
		return MIstatus::failure;
	const CMIUtilString strN5 = CMIUtilString::Format( "%s/%s", vPath.c_str(), vFileName.c_str() );
	const CMICmnMIValueConst miValueConst6( strN5 );
	const CMICmnMIValueResult miValueResult6( "fullname", miValueConst6 );
	if( !vwrMiValueTuple.Add( miValueResult6 ) )
		return MIstatus::failure;
	const CMIUtilString strLine( CMIUtilString::Format( "%d", vnLine ) );
	const CMICmnMIValueConst miValueConst7( strLine );
	const CMICmnMIValueResult miValueResult7( "line", miValueConst7 );
	if( !vwrMiValueTuple.Add( miValueResult7 ) )
		return MIstatus::failure;

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Form MI partial response by appending more MI value type objects to the 
//			tuple type object past in.
// Type:	Method.
// Args:	vId							- (R) Break point ID.
//			vStrType					- (R) Break point type. 
//			vbDisp						- (R) True = "del", false = "keep".
//			vbEnabled					- (R) True = enabled, false = disabled break point.
//			vPc							- (R) Address number.
//			vFnName						- (R) Function name.
//			vFileName					- (R) File name text.
//			vPath						- (R) Full file name and path text.
//			vnLine						- (R) File line number.
//			vbHaveArgOptionThreadGrp	- (R) True = include MI field, false = do not include "thread-groups".
//			vStrOptThrdGrp				- (R) Thread group number.
//			vnTimes						- (R) The count of the breakpoint existence.
//			vStrOrigLoc					- (R) The name of the break point.
//			vwrMIValueTuple	- (W) MI value tuple object.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmnLLDBDebugSessionInfo::MIResponseFormBrkPtInfo( const lldb::break_id_t vId, const CMIUtilString & vStrType, const bool vbDisp, const bool vbEnabled, const lldb::addr_t vPc, const CMIUtilString & vFnName, const CMIUtilString & vFileName, const CMIUtilString & vPath, const MIuint vnLine, const bool vbHaveArgOptionThreadGrp, const CMIUtilString & vStrOptThrdGrp, const MIuint & vnTimes, const CMIUtilString & vStrOrigLoc, CMICmnMIValueTuple & vwrMiValueTuple )
{
	// MI print "=breakpoint-modified,bkpt={number=\"%d\",type=\"breakpoint\",disp=\"%s\",enabled=\"%c\",addr=\"0x%08x\", func=\"%s\",file=\"%s\",fullname=\"%s/%s\",line=\"%d\",times=\"%d\",original-location=\"%s\"}"
	
	// "number="
	const CMIUtilString strN = CMIUtilString::Format( "%d", vId );
	const CMICmnMIValueConst miValueConst( strN );
	const CMICmnMIValueResult miValueResult( "number", miValueConst );
	CMICmnMIValueTuple miValueTuple( miValueResult );
	// "type="
	const CMICmnMIValueConst miValueConst2( vStrType );
	const CMICmnMIValueResult miValueResult2( "type", miValueConst2 );
	bool bOk = miValueTuple.Add( miValueResult2 );
	// "disp="
	const CMICmnMIValueConst miValueConst3( vbDisp ? "del" : "keep" );
	const CMICmnMIValueResult miValueResult3( "disp", miValueConst3 );
	bOk = bOk && miValueTuple.Add( miValueResult3 );
	// "enabled="
	const CMICmnMIValueConst miValueConst4( vbEnabled ? "y" : "n" );
	const CMICmnMIValueResult miValueResult4( "enabled", miValueConst4 );
	bOk = bOk && miValueTuple.Add( miValueResult4 );
	// "addr="
	// "func="
	// "file="
	// "fullname="
	// "line="
	bOk = bOk && CMICmnLLDBDebugSessionInfo::Instance().MIResponseFormBrkPtFrameInfo( vPc, vFnName, vFileName, vPath, vnLine, miValueTuple );
	if( vbHaveArgOptionThreadGrp )
	{
		const CMICmnMIValueConst miValueConst( vStrOptThrdGrp );
		const CMICmnMIValueList miValueList( miValueConst );
		const CMICmnMIValueResult miValueResult( "thread-groups", miValueList );
		bOk = bOk && miValueTuple.Add( miValueResult );
	}
	// "times="
	const CMIUtilString strN4 = CMIUtilString::Format( "%d", vnTimes );
	const CMICmnMIValueConst miValueConstB( strN4 );
	const CMICmnMIValueResult miValueResultB( "times", miValueConstB );
	bOk = bOk && miValueTuple.Add( miValueResultB );
	// "original-location="
	const CMICmnMIValueConst miValueConstC( vStrOrigLoc );
	const CMICmnMIValueResult miValueResultC( "original-location", miValueConstC );
	bOk = bOk && miValueTuple.Add( miValueResultC );

	vwrMiValueTuple = miValueTuple;

	return MIstatus::success;
}
