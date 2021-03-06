﻿//
// driver_psmoveservice.cpp : Defines the client and server interfaces used by the SteamVR runtime.
//

//==================================================================================================
// Includes
//==================================================================================================
#include "driver_psmoveservice.h"

#include "ProtocolVersion.h"

#include <algorithm>
#include <sstream>
#include <string>

#include <assert.h>

#define _USE_MATH_DEFINES
#include <math.h>

#if defined( _WIN32 )
    #include <windows.h>
    #include <direct.h>
    #define getcwd _getcwd // suppress "deprecation" warning
#else
    #include <unistd.h>
#endif

//==================================================================================================
// Macros
//==================================================================================================

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
#define HMD_DLL_IMPORT extern "C" __declspec( dllimport )
#elif defined(GNUC) || defined(COMPILER_GCC) || defined(__GNUC__)
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#define HMD_DLL_IMPORT extern "C" 
#else
#error "Unsupported Platform."
#endif

#if _MSC_VER
#define strcasecmp(a, b) stricmp(a,b)
#pragma warning (disable: 4996) // 'This function or variable may be unsafe': snprintf
#define snprintf _snprintf
#endif

#define LOG_TOUCHPAD_EMULATION 0
#define LOG_REALIGN_TO_HMD 1

//==================================================================================================
// Constants
//==================================================================================================
static const float k_fScalePSMoveAPIToMeters = 0.01f;  // psmove driver in cm
static const float k_fRadiansToDegrees = 180.f / 3.14159265f;

static const int k_touchpadTouchMapping = (vr::EVRButtonId)31;
static const float k_defaultThumbstickDeadZoneRadius = 0.1f;

static const char *k_PSButtonNames[CPSMoveControllerLatest::k_EPSButtonID_Count] = {
    "ps",
    "left",
    "up",
    "down",
    "right",
    "move",
    "trackpad",
    "trigger",
    "triangle",
    "square",
    "circle",
    "cross",
    "select",
    "share",
    "start",
    "options",
    "l1",
    "l2",
    "l3",
    "r1",
    "r2",
    "r3"
};

static const int k_max_vr_buttons = 37;
static const char *k_VRButtonNames[k_max_vr_buttons] = {
    "system",               // k_EButton_System
    "application_menu",     // k_EButton_ApplicationMenu
    "grip",                 // k_EButton_Grip
    "dpad_left",            // k_EButton_DPad_Left
    "dpad_up",              // k_EButton_DPad_Up
    "dpad_right",           // k_EButton_DPad_Right
    "dpad_down",            // k_EButton_DPad_Down
    "a",                    // k_EButton_A
    "button_8",              // (vr::EVRButtonId)8
    "button_9",              // (vr::EVRButtonId)9
    "button_10",              // (vr::EVRButtonId)10
    "button_11",              // (vr::EVRButtonId)11
    "button_12",              // (vr::EVRButtonId)12
    "button_13",              // (vr::EVRButtonId)13
    "button_14",              // (vr::EVRButtonId)14
    "button_15",              // (vr::EVRButtonId)15
    "button_16",              // (vr::EVRButtonId)16
    "button_17",              // (vr::EVRButtonId)17
    "button_18",              // (vr::EVRButtonId)18
    "button_19",              // (vr::EVRButtonId)19
    "button_20",              // (vr::EVRButtonId)20
    "button_21",              // (vr::EVRButtonId)21
    "button_22",              // (vr::EVRButtonId)22
    "button_23",              // (vr::EVRButtonId)23
    "button_24",              // (vr::EVRButtonId)24
    "button_25",              // (vr::EVRButtonId)25
    "button_26",              // (vr::EVRButtonId)26
    "button_27",              // (vr::EVRButtonId)27
    "button_28",              // (vr::EVRButtonId)28
    "button_29",              // (vr::EVRButtonId)29
    "button_30",              // (vr::EVRButtonId)30
    "touchpad_touched",       // (vr::EVRButtonId)31 used to map to touchpad touched state in vr
    "touchpad",               // k_EButton_Axis0, k_EButton_SteamVR_Touchpad
    "trigger",                // k_EButton_Axis1, k_EButton_SteamVR_Trigger
    "axis_2",                 // k_EButton_Axis2
    "axis_3",                 // k_EButton_Axis3
    "axis_4",                 // k_EButton_Axis4
};

static const int k_max_vr_touchpad_directions = CPSMoveControllerLatest::k_EVRTouchpadDirection_Count;
static const char *k_VRTouchpadDirectionNames[k_max_vr_touchpad_directions] = {
	"none",
	"touchpad_left",
	"touchpad_up",
	"touchpad_right",
	"touchpad_down",
	"touchpad_up-left",
	"touchpad_up-right",
	"touchpad_down-left",
	"touchpad_down-right",
};

//==================================================================================================
// Globals
//==================================================================================================

CServerDriver_PSMoveService g_ServerTrackedDeviceProvider;
CWatchdogDriver_PSMoveService g_WatchdogDriverPSMoveService;

//==================================================================================================
// Logging helpers
//==================================================================================================

static vr::IVRDriverLog * s_pLogFile = NULL;

static bool InitDriverLog( vr::IVRDriverLog *pDriverLog )
{
    if ( s_pLogFile )
        return false;
    s_pLogFile = pDriverLog;
    return s_pLogFile != NULL;
}

static void CleanupDriverLog()
{
    s_pLogFile = NULL;
}

static void DriverLogVarArgs( const char *pMsgFormat, va_list args )
{
    char buf[1024];
#if defined( WIN32 )
    vsprintf_s( buf, pMsgFormat, args );
#else
    vsnprintf( buf, sizeof( buf ), pMsgFormat, args );
#endif

    if ( s_pLogFile )
        s_pLogFile->Log( buf );
}

/** Provides printf-style debug logging via the vr::IVRDriverLog interface provided by SteamVR
* during initialization.  Client logging ends up in vrclient_appname.txt and server logging
* ends up in vrserver.txt.
*/
static void DriverLog( const char *pMsgFormat, ... )
{
    va_list args;
    va_start( args, pMsgFormat );

    DriverLogVarArgs( pMsgFormat, args );

    va_end( args );
}


static std::string PSMVector3fToString( const PSMVector3f& position )
{
	std::ostringstream stringBuilder;
	stringBuilder << "(" << position.x << ", " << position.y << ", " << position.z << ")";
	return stringBuilder.str();
}


static std::string PSMQuatfToString(const PSMQuatf& rotation)
{
	std::ostringstream stringBuilder;
	stringBuilder << "(" << rotation.w << ", " << rotation.x << ", " << rotation.y << ", " << rotation.z << ")";
	return stringBuilder.str();
}


static std::string PSMPosefToString(const PSMPosef& pose)
{
	std::ostringstream stringBuilder;
	stringBuilder << "[Pos: " << PSMVector3fToString(pose.Position) << ", Rot:" << PSMQuatfToString(pose.Orientation) << "]";
	return stringBuilder.str();
}


//==================================================================================================
// Math Helpers
//==================================================================================================
// From: http://www.euclideanspace.com/maths/geometry/rotations/conversions/matrixToQuaternion/
static PSMQuatf openvrMatrixExtractPSMQuatf(const vr::HmdMatrix34_t &openVRTransform)
{
	PSMQuatf q;

	const float(&a)[3][4] = openVRTransform.m;
	const float trace = a[0][0] + a[1][1] + a[2][2];

	if (trace > 0)
	{
		const float s = 0.5f / sqrtf(trace + 1.0f);

		q.w = 0.25f / s;
		q.x = (a[2][1] - a[1][2]) * s;
		q.y = (a[0][2] - a[2][0]) * s;
		q.z = (a[1][0] - a[0][1]) * s;
	}
	else
	{
		if (a[0][0] > a[1][1] && a[0][0] > a[2][2])
		{
			const float s = 2.0f * sqrtf(1.0f + a[0][0] - a[1][1] - a[2][2]);

			q.w = (a[2][1] - a[1][2]) / s;
			q.x = 0.25f * s;
			q.y = (a[0][1] + a[1][0]) / s;
			q.z = (a[0][2] + a[2][0]) / s;
		}
		else if (a[1][1] > a[2][2])
		{
			const float s = 2.0f * sqrtf(1.0f + a[1][1] - a[0][0] - a[2][2]);

			q.w = (a[0][2] - a[2][0]) / s;
			q.x = (a[0][1] + a[1][0]) / s;
			q.y = 0.25f * s;
			q.z = (a[1][2] + a[2][1]) / s;
		}
		else
		{
			const float s = 2.0f * sqrtf(1.0f + a[2][2] - a[0][0] - a[1][1]);

			q.w = (a[1][0] - a[0][1]) / s;
			q.x = (a[0][2] + a[2][0]) / s;
			q.y = (a[1][2] + a[2][1]) / s;
			q.z = 0.25f * s;
		}
	}

	return q;
}

static PSMVector3f openvrMatrixExtractPSMVector3f(const vr::HmdMatrix34_t &openVRTransform)
{
	const float(&a)[3][4] = openVRTransform.m;
	PSMVector3f pos= {a[0][3], a[1][3], a[2][3]};

	return pos;
}

static PSMPosef openvrMatrixExtractPSMPosef(const vr::HmdMatrix34_t &openVRTransform)
{
	PSMPosef pose;
	pose.Orientation = openvrMatrixExtractPSMQuatf(openVRTransform);
	pose.Position = openvrMatrixExtractPSMVector3f(openVRTransform);

	return pose;
}

//==================================================================================================
// Watchdog Driver
//==================================================================================================

CWatchdogDriver_PSMoveService::CWatchdogDriver_PSMoveService()
	: m_pLogger(nullptr)
    , m_loggerMutex()
	, m_bWasConnected(false)
	, m_bExitSignaled({ false })
	, m_pWatchdogThread(nullptr)
{
	m_strPSMoveServiceAddress= PSMOVESERVICE_DEFAULT_ADDRESS;
	m_strServerPort= PSMOVESERVICE_DEFAULT_PORT;
}

vr::EVRInitError CWatchdogDriver_PSMoveService::Init( vr::IVRDriverContext *pDriverContext )
{
	VR_INIT_WATCHDOG_DRIVER_CONTEXT( pDriverContext );

	m_pLogger= vr::VRDriverLog();

	WatchdogLog("CWatchdogDriver_PSMoveService::Init - Called");

	vr::IVRSettings *pSettings = vr::VRSettings();
	if (pSettings != nullptr) 
	{
		char buf[256];
		vr::EVRSettingsError fetchError;

		pSettings->GetString("psmoveservice", "server_address", buf, sizeof(buf), &fetchError);
		if (fetchError == vr::VRSettingsError_None)
		{
			m_strPSMoveServiceAddress= buf;
			WatchdogLog("CWatchdogDriver_PSMoveService::Init - Overridden Server Address: %s.\n", m_strPSMoveServiceAddress.c_str());
		}
		else
		{
			WatchdogLog("CWatchdogDriver_PSMoveService::Init - Using Default Server Address: %s.\n", m_strPSMoveServiceAddress.c_str());
		}

		pSettings->GetString("psmoveservice", "server_port", buf, sizeof(buf), &fetchError);
		if (fetchError == vr::VRSettingsError_None)
		{
			m_strServerPort= buf;
			WatchdogLog("CWatchdogDriver_PSMoveService::Init - Overridden Server Port: %s.\n", m_strServerPort.c_str());
		}
		else
		{
			WatchdogLog("CWatchdogDriver_PSMoveService::Init - Using Default Server Port: %s.\n", m_strServerPort.c_str());
		}
	}
	else
	{
		WatchdogLog("CWatchdogDriver_PSMoveService::Init - Settings missing!");
	}

	// Watchdog mode on Windows starts a thread that listens for the 'Y' key on the keyboard to 
	// be pressed. A real driver should wait for a system button event or something else from the 
	// the hardware that signals that the VR system should start up.
	m_bExitSignaled = false;
	m_pWatchdogThread = new std::thread( &CWatchdogDriver_PSMoveService::WorkerThreadFunction, this);
	if ( !m_pWatchdogThread )
	{
		WatchdogLog("Unable to create watchdog thread\n");
		return vr::VRInitError_Driver_Failed;
	}

	return vr::VRInitError_None;
}

void CWatchdogDriver_PSMoveService::Cleanup()
{
	WatchdogLog("CWatchdogDriver_PSMoveService::Cleanup - Called");

	m_bExitSignaled = true;
	if ( m_pWatchdogThread )
	{
		WatchdogLog("CWatchdogDriver_PSMoveService::Cleanup - Stopping worker thread...");
		m_pWatchdogThread->join();
		delete m_pWatchdogThread;
		m_pWatchdogThread = nullptr;
		WatchdogLog("CWatchdogDriver_PSMoveService::Cleanup - Worker thread stopped.");
	}
	else
	{
		WatchdogLog("CWatchdogDriver_PSMoveService::Cleanup - No worker thread active.");
	}

	WatchdogLog("CWatchdogDriver_PSMoveService::Cleanup - Watchdog clean up complete.");
	m_pLogger= nullptr;

	VR_CLEANUP_WATCHDOG_DRIVER_CONTEXT()
}

void CWatchdogDriver_PSMoveService::WorkerThreadFunction()
{
	WatchdogLog("CWatchdogDriver_PSMoveService::WatchdogThreadFunction - Entered\n");

	while ( !m_bExitSignaled )
	{
		if (!PSM_GetIsInitialized())
		{
			if (PSM_Initialize(m_strPSMoveServiceAddress.c_str(), m_strServerPort.c_str(), PSM_DEFAULT_TIMEOUT) != PSMResult_Success)
			{
				// Try re-connecting in 1 second
				std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
				continue;
			}
		}

		PSM_Update();

		if (PSM_WasSystemButtonPressed())
		{
			WatchdogLog("CWatchdogDriver_PSMoveService::WatchdogThreadFunction - System button pressed. Initiating wake up.\n");
			vr::VRWatchdogHost()->WatchdogWakeUp();
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}

	PSM_Shutdown();

	vr::VRDriverLog()->Log("CWatchdogDriver_PSMoveService::WatchdogThreadFunction - Exited\n");
}

void CWatchdogDriver_PSMoveService::WatchdogLogVarArgs( const char *pMsgFormat, va_list args )
{
    char buf[1024];
	#if defined( WIN32 )
    vsprintf_s( buf, pMsgFormat, args );
	#else
    vsnprintf( buf, sizeof( buf ), pMsgFormat, args );
	#endif

	if (m_pLogger)
	{
        std::lock_guard<std::mutex> guard(m_loggerMutex);

        m_pLogger->Log( buf );
	}
}

void CWatchdogDriver_PSMoveService::WatchdogLog( const char *pMsgFormat, ... )
{
    va_list args;
    va_start( args, pMsgFormat );

    DriverLogVarArgs( pMsgFormat, args );

    va_end( args );
}

//==================================================================================================
// Server Provider
//==================================================================================================

CServerDriver_PSMoveService::CServerDriver_PSMoveService()
    : m_bLaunchedPSMoveMonitor(false)
	, m_bInitialized(false)
{
	m_strPSMoveServiceAddress= PSMOVESERVICE_DEFAULT_ADDRESS;
	m_strServerPort= PSMOVESERVICE_DEFAULT_PORT;
}

CServerDriver_PSMoveService::~CServerDriver_PSMoveService()
{
	// 10/10/2015 benj:  vrserver is exiting without calling Cleanup() to balance Init()
	// causing std::thread to call std::terminate
	Cleanup();
}

vr::EVRInitError CServerDriver_PSMoveService::Init(
    vr::IVRDriverContext *pDriverContext )
{
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

    vr::EVRInitError initError = vr::VRInitError_None;

	InitDriverLog(vr::VRDriverLog());
	DriverLog("CServerDriver_PSMoveService::Init - called.\n");

	if (!m_bInitialized)
	{
		vr::IVRSettings *pSettings = vr::VRSettings();
		if (pSettings != nullptr) 
		{
			char buf[256];
			vr::EVRSettingsError fetchError;

			pSettings->GetString("psmove_settings", "psmove_filter_hmd_serial", buf, sizeof(buf), &fetchError);
			if (fetchError == vr::VRSettingsError_None)
			{
				m_strPSMoveHMDSerialNo = buf;
				std::transform(m_strPSMoveHMDSerialNo.begin(), m_strPSMoveHMDSerialNo.end(), m_strPSMoveHMDSerialNo.begin(), ::toupper);
			}

			pSettings->GetString("psmoveservice", "server_address", buf, sizeof(buf), &fetchError);
			if (fetchError == vr::VRSettingsError_None)
			{
				m_strPSMoveServiceAddress= buf;
				DriverLog("CServerDriver_PSMoveService::Init - Overridden Server Address: %s.\n", m_strPSMoveServiceAddress.c_str());
			}
			else
			{
				DriverLog("CServerDriver_PSMoveService::Init - Using Default Server Address: %s.\n", m_strPSMoveServiceAddress.c_str());
			}

			pSettings->GetString("psmoveservice", "server_port", buf, sizeof(buf), &fetchError);
			if (fetchError == vr::VRSettingsError_None)
			{
				m_strServerPort= buf;
				DriverLog("CServerDriver_PSMoveService::Init - Overridden Server Port: %s.\n", m_strServerPort.c_str());
			}
			else
			{
				DriverLog("CServerDriver_PSMoveService::Init - Using Default Server Port: %s.\n", m_strServerPort.c_str());
			}
		}
		else
		{
			DriverLog("CServerDriver_PSMoveService::Init - NULL settings!.\n");
		}

		DriverLog("CServerDriver_PSMoveService::Init - Initializing.\n");

		// By default, assume the psmove and openvr tracking spaces are the same
		m_worldFromDriverPose= *k_psm_pose_identity;

		// Note that reconnection is a non-blocking async request.
		// Returning true means we we're able to start trying to connect,
		// not that we are successfully connected yet.
		if (!ReconnectToPSMoveService())
		{
			initError = vr::VRInitError_Driver_Failed;
		}

		m_bInitialized = true;
	}
	else
	{
		DriverLog("CServerDriver_PSMoveService::Init - Already Initialized. Ignoring.\n");
	}

    return initError;
}

bool CServerDriver_PSMoveService::ReconnectToPSMoveService()
{
	DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - called.\n");

    if (PSM_GetIsInitialized())
    {
		DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Existing PSMoveService connection active. Shutting down...\n");
        PSM_Shutdown();
		DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Existing PSMoveService connection stopped.\n");
    }
	else
	{
		DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Existing PSMoveService connection NOT active.\n");
	}

	DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Starting PSMoveService connection...\n");
    bool bSuccess= PSM_InitializeAsync(m_strPSMoveServiceAddress.c_str(), m_strServerPort.c_str()) != PSMResult_Error;

	if (bSuccess)
	{
		DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Successfully requested connection\n");
	}
	else
	{
		DriverLog("CServerDriver_PSMoveService::ReconnectToPSMoveService - Failed to request connection!\n");
	}

	return bSuccess;
}

void CServerDriver_PSMoveService::Cleanup()
{
	if (m_bInitialized)
	{
		DriverLog("CServerDriver_PSMoveService::Cleanup - Shutting down connection...\n");
		PSM_Shutdown();
		DriverLog("CServerDriver_PSMoveService::Cleanup - Shutdown complete\n");

		m_bInitialized = false;
	}
}

const char * const *CServerDriver_PSMoveService::GetInterfaceVersions()
{
    return vr::k_InterfaceVersions;
}

vr::ITrackedDeviceServerDriver * CServerDriver_PSMoveService::FindTrackedDeviceDriver(
    const char * pchId)
{
    for (auto it = m_vecTrackedDevices.begin(); it != m_vecTrackedDevices.end(); ++it)
    {
        if ( 0 == strcmp( ( *it )->GetSteamVRIdentifier(), pchId ) )
        {
            return *it;
        }
    }

    return nullptr;
}

void CServerDriver_PSMoveService::RunFrame()
{
    // Update any controllers that are currently listening
    PSM_UpdateNoPollMessages();

    // Poll events queued up by the call to PSM_UpdateNoPollMessages()
    PSMMessage mesg;
    while (PSM_PollNextMessage(&mesg, sizeof(PSMMessage)) == PSMResult_Success)
    {
        switch (mesg.payload_type)
        {
        case PSMMessage::_messagePayloadType_Response:
            HandleClientPSMoveResponse(&mesg);
            break;
        case PSMMessage::_messagePayloadType_Event:
            HandleClientPSMoveEvent(&mesg);
            break;
        }
    }

    // Update all active tracked devices
    for (auto it = m_vecTrackedDevices.begin(); it != m_vecTrackedDevices.end(); ++it)
    {
        CPSMoveTrackedDeviceLatest *pTrackedDevice = *it;

        switch (pTrackedDevice->GetTrackedDeviceClass())
        {
        case vr::TrackedDeviceClass_Controller:
            {
                CPSMoveControllerLatest *pController = static_cast<CPSMoveControllerLatest *>(pTrackedDevice);

                pController->Update();
            } break;
        case vr::TrackedDeviceClass_TrackingReference:
            {
                CPSMoveTrackerLatest *pTracker = static_cast<CPSMoveTrackerLatest *>(pTrackedDevice);

                pTracker->Update();
            } break;
        default:
            assert(0 && "unreachable");
        }
    }
}

bool CServerDriver_PSMoveService::ShouldBlockStandbyMode()
{
    return false;
}

void CServerDriver_PSMoveService::EnterStandby()
{
}

void CServerDriver_PSMoveService::LeaveStandby()
{
}

// -- Event Handling -----
void CServerDriver_PSMoveService::HandleClientPSMoveEvent(
    const PSMMessage *message)
{
    switch (message->event_data.event_type)
    {
    // Client Events
    case PSMEventMessage::PSMEvent_connectedToService:
        HandleConnectedToPSMoveService();
        break;
    case PSMEventMessage::PSMEvent_failedToConnectToService:
        HandleFailedToConnectToPSMoveService();
        break;
    case PSMEventMessage::PSMEvent_disconnectedFromService:
        HandleDisconnectedFromPSMoveService();
        break;

    // Service Events
    case PSMEventMessage::PSMEvent_opaqueServiceEvent: 
        // We don't care about any opaque service events
        break;
    case PSMEventMessage::PSMEvent_controllerListUpdated:
        HandleControllerListChanged();
        break;
    case PSMEventMessage::PSMEvent_trackerListUpdated:
        HandleTrackerListChanged();
        break;
    case PSMEventMessage::PSMEvent_hmdListUpdated:
        // don't care
        break;
	case PSMEventMessage::PSMEvent_systemButtonPressed:
        // don't care
        break;
    //###HipsterSloth $TODO - Need a notification for when a tracker pose changes
    }
}

void CServerDriver_PSMoveService::HandleConnectedToPSMoveService()
{
	DriverLog("CServerDriver_PSMoveService::HandleConnectedToPSMoveService - Request controller and tracker lists\n");

	PSMRequestID request_id;
	PSM_GetServiceVersionStringAsync(&request_id);
	PSM_RegisterCallback(request_id, CServerDriver_PSMoveService::HandleServiceVersionResponse, this);
}

void CServerDriver_PSMoveService::HandleServiceVersionResponse(
	const PSMResponseMessage *response,
    void *userdata)
{
    PSMResult ResultCode = response->result_code;
    PSMResponseHandle response_handle = response->opaque_response_handle;
    CServerDriver_PSMoveService *thisPtr = static_cast<CServerDriver_PSMoveService *>(userdata);

    switch (ResultCode)
    {
    case PSMResult::PSMResult_Success:
        {
			const std::string service_version= response->payload.service_version.version_string;
			const std::string local_version= PSM_GetClientVersionString();

			if (service_version == local_version)
			{
				DriverLog("CServerDriver_PSMoveService::HandleServiceVersionResponse - Received expected protocol version %s\n", service_version.c_str());

				// Ask the service for a list of connected controllers
				// Response handled in HandleControllerListReponse()
				PSM_GetControllerListAsync(nullptr);

				// Ask the service for a list of connected trackers
				// Response handled in HandleTrackerListReponse()
				PSM_GetTrackerListAsync(nullptr);
			}
			else
			{
				DriverLog("CServerDriver_PSMoveService::HandleServiceVersionResponse - Protocol mismatch! Expected %s, got %s. Please reinstall the PSMove Driver!\n",
						local_version.c_str(), service_version.c_str());
				thisPtr->Cleanup();
			}
        } break;
    case PSMResult::PSMResult_Error:
    case PSMResult::PSMResult_Canceled:
        {
			DriverLog("CServerDriver_PSMoveService::HandleServiceVersionResponse - Failed to get protocol version\n");
        } break;
    }
}


void CServerDriver_PSMoveService::HandleFailedToConnectToPSMoveService()
{
	DriverLog("CServerDriver_PSMoveService::HandleFailedToConnectToPSMoveService - Called\n");

    // Immediately attempt to reconnect to the service
    ReconnectToPSMoveService();
}

void CServerDriver_PSMoveService::HandleDisconnectedFromPSMoveService()
{
	DriverLog("CServerDriver_PSMoveService::HandleDisconnectedFromPSMoveService - Called\n");

    for (auto it = m_vecTrackedDevices.begin(); it != m_vecTrackedDevices.end(); ++it)
    {
        CPSMoveTrackedDeviceLatest *pDevice = *it;

        pDevice->Deactivate();
    }

    // Immediately attempt to reconnect to the service
    ReconnectToPSMoveService();
}

void CServerDriver_PSMoveService::HandleControllerListChanged()
{
	DriverLog("CServerDriver_PSMoveService::HandleControllerListChanged - Called\n");

    // Ask the service for a list of connected controllers
    // Response handled in HandleControllerListReponse()
    PSM_GetControllerListAsync(nullptr);
}

void CServerDriver_PSMoveService::HandleTrackerListChanged()
{
	DriverLog("CServerDriver_PSMoveService::HandleTrackerListChanged - Called\n");

    // Ask the service for a list of connected trackers
    // Response handled in HandleTrackerListReponse()
    PSM_GetTrackerListAsync(nullptr);
}

// -- Response Handling -----
void CServerDriver_PSMoveService::HandleClientPSMoveResponse(
    const PSMMessage *message)
{	
    switch (message->response_data.payload_type)
    {
    case PSMResponseMessage::_responsePayloadType_Empty:
        DriverLog("NotifyClientPSMoveResponse - request id %d returned result %s.\n",
            message->response_data.request_id, 
            (message->response_data.result_code == PSMResult::PSMResult_Success) ? "ok" : "error");
        break;
    case PSMResponseMessage::_responsePayloadType_ControllerList:
        DriverLog("NotifyClientPSMoveResponse - Controller Count = %d (request id %d).\n", 
            message->response_data.payload.controller_list.count, message->response_data.request_id);
        HandleControllerListReponse(&message->response_data.payload.controller_list, message->response_data.opaque_request_handle);
        break;
	case PSMResponseMessage::_responsePayloadType_TrackerList:
        DriverLog("NotifyClientPSMoveResponse - Tracker Count = %d (request id %d).\n",
            message->response_data.payload.tracker_list.count, message->response_data.request_id);
        HandleTrackerListReponse(&message->response_data.payload.tracker_list);
        break;
    default:
        DriverLog("NotifyClientPSMoveResponse - Unhandled response (request id %d).\n", message->response_data.request_id);
    }
}

void CServerDriver_PSMoveService::HandleControllerListReponse(
    const PSMControllerList *controller_list,
	const PSMResponseHandle response_handle)
{
	DriverLog("CServerDriver_PSMoveService::HandleControllerListReponse - Received %d controllers\n", controller_list->count);

	bool bAnyNaviControllers= false;
    for (int list_index = 0; list_index < controller_list->count; ++list_index)
    {
        PSMControllerID psmControllerId = controller_list->controller_id[list_index];
        PSMControllerType psmControllerType = controller_list->controller_type[list_index];
		std::string psmControllerSerial(controller_list->controller_serial[list_index]);

        switch (psmControllerType)
        {
        case PSMControllerType::PSMController_Move:
			DriverLog("CServerDriver_PSMoveService::HandleControllerListReponse - Allocate PSMove(%d)\n", psmControllerId);
            AllocateUniquePSMoveController(psmControllerId, psmControllerSerial);
            break;
        case PSMControllerType::PSMController_Navi:
			// Take care of this is the second pass once all of the PSMove controllers have been setup
			bAnyNaviControllers= true;
            break;
        case PSMControllerType::PSMController_DualShock4:
			DriverLog("CServerDriver_PSMoveService::HandleControllerListReponse - Allocate PSDualShock4(%d)\n", psmControllerId);
            AllocateUniqueDualShock4Controller(psmControllerId, psmControllerSerial);
            break;
        default:
            break;
        }
    }

	if (bAnyNaviControllers)
	{
		for (int list_index = 0; list_index < controller_list->count; ++list_index)
		{
			int controller_id = controller_list->controller_id[list_index];
			PSMControllerType controller_type = controller_list->controller_type[list_index];
			std::string ControllerSerial(controller_list->controller_serial[list_index]);
			std::string ParentControllerSerial(controller_list->parent_controller_serial[list_index]);

			if (controller_type == PSMControllerType::PSMController_Navi)
			{
				DriverLog("CServerDriver_PSMoveService::HandleControllerListReponse - Attach PSNavi(%d)\n", controller_id);
				AttachPSNaviToParentController(controller_id, ControllerSerial, ParentControllerSerial);
			}
		}
	}
}

void CServerDriver_PSMoveService::HandleTrackerListReponse(
    const PSMTrackerList *tracker_list)
{
	DriverLog("CServerDriver_PSMoveService::HandleTrackerListReponse - Received %d trackers\n", tracker_list->count);

    for (int list_index = 0; list_index < tracker_list->count; ++list_index)
    {
        const PSMClientTrackerInfo *trackerInfo = &tracker_list->trackers[list_index];

        AllocateUniquePSMoveTracker(trackerInfo);
    }
}

void CServerDriver_PSMoveService::SetHMDTrackingSpace(
    const PSMPosef &origin_pose)
{
	#if LOG_REALIGN_TO_HMD != 0
		DriverLog("Begin CServerDriver_PSMoveService::SetHMDTrackingSpace()\n");
	#endif

    m_worldFromDriverPose = origin_pose;

    // Tell all the devices that the relationship between the psmove and the OpenVR
    // tracking spaces changed
    for (auto it = m_vecTrackedDevices.begin(); it != m_vecTrackedDevices.end(); ++it)
    {
        CPSMoveTrackedDeviceLatest *pDevice = *it;

        pDevice->RefreshWorldFromDriverPose();
    }
}

static void GenerateControllerSteamVRIdentifier( char *p, int psize, int controller )
{
    snprintf(p, psize, "psmove_controller%d", controller);
}

void CServerDriver_PSMoveService::AllocateUniquePSMoveController(PSMControllerID psmControllerID, const std::string &psmControllerSerial)
{
    char svrIdentifier[256];
    GenerateControllerSteamVRIdentifier(svrIdentifier, sizeof(svrIdentifier), psmControllerID);

    if ( !FindTrackedDeviceDriver(svrIdentifier) )
    {	
		std::string psmSerialNo = psmControllerSerial;
		std::transform(psmSerialNo.begin(), psmSerialNo.end(), psmSerialNo.begin(), ::toupper);

		if (0 != m_strPSMoveHMDSerialNo.compare(psmSerialNo)) 
		{
			DriverLog( "added new psmove controller id: %d, serial: %s\n", psmControllerID, psmSerialNo.c_str());

            CPSMoveControllerLatest *TrackedDevice= 
                new CPSMoveControllerLatest( psmControllerID, PSMControllerType::PSMController_Move, psmSerialNo.c_str());
			m_vecTrackedDevices.push_back(TrackedDevice);

			if (vr::VRServerDriverHost())
			{
				vr::VRServerDriverHost()->TrackedDeviceAdded(TrackedDevice->GetSteamVRIdentifier(), vr::TrackedDeviceClass_Controller, TrackedDevice);
			}
		}
		else
		{
			DriverLog("skipped new psmove controller as configured for HMD tracking, serial: %s\n", psmSerialNo.c_str());
		}
    }
}


void CServerDriver_PSMoveService::AllocateUniqueDualShock4Controller(PSMControllerID psmControllerID, const std::string &psmControllerSerial)
{
    char svrIdentifier[256];
    GenerateControllerSteamVRIdentifier(svrIdentifier, sizeof(svrIdentifier), psmControllerID);

    if (!FindTrackedDeviceDriver(svrIdentifier))
    {
		std::string psmSerialNo = psmControllerSerial;
		std::transform(psmSerialNo.begin(), psmSerialNo.end(), psmSerialNo.begin(), ::toupper);

		DriverLog( "added new dualshock4 controller id: %d, serial: %s\n", psmControllerID, psmSerialNo.c_str());

        CPSMoveControllerLatest *TrackedDevice= 
            new CPSMoveControllerLatest(psmControllerID, PSMControllerType::PSMController_DualShock4, psmSerialNo.c_str());
		m_vecTrackedDevices.push_back(TrackedDevice);

		if (vr::VRServerDriverHost())
		{
			vr::VRServerDriverHost()->TrackedDeviceAdded(TrackedDevice->GetSteamVRIdentifier(), vr::TrackedDeviceClass_Controller, TrackedDevice);
		}
    }
}

void CServerDriver_PSMoveService::AttachPSNaviToParentController(PSMControllerID NaviControllerID, const std::string &NaviControllerSerial, const std::string &ParentControllerSerial)
{
	bool bFoundParent= false;

	std::string naviSerialNo = NaviControllerSerial;
	std::string parentSerialNo = ParentControllerSerial;
    std::transform(naviSerialNo.begin(), naviSerialNo.end(), naviSerialNo.begin(), ::toupper);
	std::transform(parentSerialNo.begin(), parentSerialNo.end(), parentSerialNo.begin(), ::toupper);

	for (CPSMoveTrackedDeviceLatest *trackedDevice : m_vecTrackedDevices)
	{
		if (trackedDevice->GetTrackedDeviceClass() == vr::TrackedDeviceClass_Controller)
		{
			CPSMoveControllerLatest *test_controller= static_cast<CPSMoveControllerLatest *>(trackedDevice);
			const std::string testSerialNo= test_controller->getPSMControllerSerialNo();
				
			if (testSerialNo == parentSerialNo)
			{
				bFoundParent= true;

				if (test_controller->getPSMControllerType() == PSMController_Move)
				{
					if (test_controller->AttachChildPSMController(NaviControllerID, PSMController_Navi, naviSerialNo))
					{
						DriverLog("Attached navi controller serial %s to controller serial %s\n", naviSerialNo.c_str(), parentSerialNo.c_str());
					}
					else
					{
						DriverLog("Failed to attach navi controller serial %s to controller serial %s\n", naviSerialNo.c_str(), parentSerialNo.c_str());
					}
				}
				else
				{
					DriverLog("Failed to attach navi controller serial %s to non-psmove controller serial %s\n", naviSerialNo.c_str(), parentSerialNo.c_str());
				}

				break;
			}
		}
	}

	if (!bFoundParent)
	{
		DriverLog("Failed to find parent controller serial %s for navi controller serial %s\n", parentSerialNo.c_str(), naviSerialNo.c_str());
	}
}

static void GenerateTrackerSerialNumber(char *p, int psize, int tracker)
{
    snprintf(p, psize, "psmove_tracker%d", tracker);
}

void CServerDriver_PSMoveService::AllocateUniquePSMoveTracker(const PSMClientTrackerInfo *trackerInfo)
{
    char svrIdentifier[256];
    GenerateTrackerSerialNumber(svrIdentifier, sizeof(svrIdentifier), trackerInfo->tracker_id);

    if (!FindTrackedDeviceDriver(svrIdentifier))
    {
        DriverLog("added new tracker device %s\n", svrIdentifier);
        CPSMoveTrackerLatest *TrackerDevice= new CPSMoveTrackerLatest(trackerInfo);
        m_vecTrackedDevices.push_back(TrackerDevice);

        if (vr::VRServerDriverHost())
        {
            vr::VRServerDriverHost()->TrackedDeviceAdded(TrackerDevice->GetSteamVRIdentifier(), vr::TrackedDeviceClass_TrackingReference, TrackerDevice);
        }
    }
}


// The monitor_psmove is a companion program which can display overlay prompts for us
// and tell us the pose of the HMD at the moment we want to calibrate.
void CServerDriver_PSMoveService::LaunchPSMoveMonitor_Internal( const char * pchDriverInstallDir )
{
#if LOG_REALIGN_TO_HMD != 0
	DriverLog("Entered CServerDriver_PSMoveService::LaunchPSMoveMonitor_Internal(%s)\n", pchDriverInstallDir);
#endif

    m_bLaunchedPSMoveMonitor = true;

    std::ostringstream path_and_executable_string_builder;

    path_and_executable_string_builder << pchDriverInstallDir;
#if defined( _WIN64 )
    path_and_executable_string_builder << "\\bin\\win64";
#elif defined( _WIN32 )
    path_and_executable_string_builder << "\\bin\\win32";
#elif defined(__APPLE__) 
    path_and_executable_string_builder << "/bin/osx";
#else 
    #error Do not know how to launch psmove_monitor
#endif


#if defined( _WIN32 ) || defined( _WIN64 )
	path_and_executable_string_builder << "\\monitor_psmove.exe";
	const std::string monitor_path_and_exe = path_and_executable_string_builder.str();

	std::ostringstream args_string_builder;
	args_string_builder << "monitor_psmove.exe \"" << pchDriverInstallDir << "\\resources\"";
	const std::string monitor_args = args_string_builder.str();
	
	char monitor_args_cstr[1024];
	strncpy_s(monitor_args_cstr, monitor_args.c_str(), sizeof(monitor_args_cstr) - 1);
	monitor_args_cstr[sizeof(monitor_args_cstr) - 1] = '\0';

	#if LOG_REALIGN_TO_HMD != 0
		DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor_Internal() monitor_psmove windows full path: %s\n", monitor_path_and_exe.c_str());
		DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor_Internal() monitor_psmove windows args: %s\n", monitor_args_cstr);
	#endif

	STARTUPINFOA sInfoProcess = { 0 };
	sInfoProcess.cb = sizeof(STARTUPINFOW);
	PROCESS_INFORMATION pInfoStartedProcess;
	BOOL bSuccess = CreateProcessA(monitor_path_and_exe.c_str(), monitor_args_cstr, NULL, NULL, FALSE, 0, NULL, NULL, &sInfoProcess, &pInfoStartedProcess);
	DWORD ErrorCode = (bSuccess == TRUE) ? 0 : GetLastError();

	DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor_Internal() Start monitor_psmove CreateProcessA() result: %d.\n", ErrorCode);

#elif defined(__APPLE__) 
    pid_t processId;
    if ((processId = fork()) == 0)
    {
		path_and_executable_string_builder << "\\monitor_psmove";

		const std::string monitor_exe_path = path_and_executable_string_builder.str();
        const char * argv[] = { monitor_exe_path.c_str(), pchDriverInstallDir, NULL };
        
        if (execv(app, argv) < 0)
        {
            DriverLog( "Failed to exec child process\n");
        }
    }
    else if (processId < 0)
    {
        DriverLog( "Failed to fork child process\n");
        perror("fork error");
    }
#else 
#error Do not know how to launch psmove config tool
#endif
}

/** Launch monitor_psmove if needed (requested by devices as they activate) */
void CServerDriver_PSMoveService::LaunchPSMoveMonitor()
{
    if ( m_bLaunchedPSMoveMonitor )
	{
        return;
	}

	#if LOG_REALIGN_TO_HMD != 0
	DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor() - Called\n");
	#endif

    //###HipsterSloth $TODO - Ideally we would get the install path as a property, but this property fetch doesn't seem to work...
	//vr::ETrackedPropertyError errorCode;
	//std::string driverInstallDir= vr::VRProperties()->GetStringProperty(requestingDevicePropertyHandle, vr::Prop_InstallPath_String, &errorCode);

    //...so for now, just assume that we're running out of the steamvr folder
    char szCurrentDirectory[MAX_PATH];
    if (getcwd(szCurrentDirectory, MAX_PATH - 1) != 0)
    {
        DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor() - vrserver working directory: %s\n", szCurrentDirectory);

	    std::ostringstream driverInstallDirBuilder;
        driverInstallDirBuilder << szCurrentDirectory;
        #if defined( _WIN64 ) || defined( _WIN32 )
            driverInstallDirBuilder << "\\drivers\\psmove";
        #else
            driverInstallDirBuilder << "/drivers/psmove";
        #endif
	    const std::string driverInstallDir = driverInstallDirBuilder.str();

	    LaunchPSMoveMonitor_Internal( driverInstallDir.c_str() );
    }
    else
    {
        DriverLog("CServerDriver_PSMoveService::LaunchPSMoveMonitor() - Failed to fetch current directory\n");
    }
}

//==================================================================================================
// Tracked Device Driver
//==================================================================================================

CPSMoveTrackedDeviceLatest::CPSMoveTrackedDeviceLatest()
    : m_ulPropertyContainer(vr::k_ulInvalidPropertyContainer)
    , m_unSteamVRTrackedDeviceId(vr::k_unTrackedDeviceIndexInvalid)
{
    memset(&m_Pose, 0, sizeof(m_Pose));
    m_Pose.result = vr::TrackingResult_Uninitialized;

    // By default, assume that the tracked devices are in the tracking space as OpenVR
    m_Pose.qWorldFromDriverRotation.w = 1.f;
    m_Pose.qWorldFromDriverRotation.x = 0.f;
    m_Pose.qWorldFromDriverRotation.y = 0.f;
    m_Pose.qWorldFromDriverRotation.z = 0.f;
    m_Pose.vecWorldFromDriverTranslation[0] = 0.f;
    m_Pose.vecWorldFromDriverTranslation[1] = 0.f;
    m_Pose.vecWorldFromDriverTranslation[2] = 0.f;

    m_firmware_revision = 0x0001;
    m_hardware_revision = 0x0001;

	m_lastHMDPoseInMeters= *k_psm_pose_identity;
	m_lastHMDPoseTime= std::chrono::time_point<std::chrono::high_resolution_clock>();
	m_bIsLastHMDPoseValid= false;
	m_hmdResultCallback = nullptr;
	m_hmdResultUserData = nullptr;
}

CPSMoveTrackedDeviceLatest::~CPSMoveTrackedDeviceLatest()
{
}

// Shared Implementation of vr::ITrackedDeviceServerDriver
vr::EVRInitError CPSMoveTrackedDeviceLatest::Activate(vr::TrackedDeviceIndex_t unObjectId)
{
	vr::CVRPropertyHelpers *properties= vr::VRProperties();

    DriverLog("CPSMoveTrackedDeviceLatest::Activate: %s is object id %d\n", GetSteamVRIdentifier(), unObjectId);
	m_ulPropertyContainer = properties->TrackedDeviceToPropertyContainer( unObjectId );
    m_unSteamVRTrackedDeviceId = unObjectId;

	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_Firmware_UpdateAvailable_Bool, false);
	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_Firmware_ManualUpdate_Bool, false);
	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_ContainsProximitySensor_Bool, false);
	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_HasCamera_Bool, false);
	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_Firmware_ForceUpdateRequired_Bool, false);
	properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_DeviceCanPowerOff_Bool, false);
	properties->SetUint64Property(m_ulPropertyContainer, vr::Prop_HardwareRevision_Uint64, m_hardware_revision);
	properties->SetUint64Property(m_ulPropertyContainer, vr::Prop_FirmwareVersion_Uint64, m_firmware_revision);

    return vr::VRInitError_None;
}

void CPSMoveTrackedDeviceLatest::Deactivate() 
{
    DriverLog("CPSMoveTrackedDeviceLatest::Deactivate: %s was object id %d\n", GetSteamVRIdentifier(), m_unSteamVRTrackedDeviceId);
    m_unSteamVRTrackedDeviceId = vr::k_unTrackedDeviceIndexInvalid;
}

void CPSMoveTrackedDeviceLatest::EnterStandby()
{
    //###HipsterSloth $TODO - No good way to do this at the moment
}

void *CPSMoveTrackedDeviceLatest::GetComponent(const char *pchComponentNameAndVersion)
{
    return NULL;
}

void CPSMoveTrackedDeviceLatest::DebugRequest(
    const char * pchRequest,
    char * pchResponseBuffer,
    uint32_t unResponseBufferSize)
{
	std::istringstream ss( pchRequest );
	std::string strCmd;

	ss >> strCmd;
	if (strCmd == "psmove:hmd_pose")
	{
		#if LOG_REALIGN_TO_HMD != 0
			DriverLog( "CPSMoveTrackedDeviceLatest::DebugRequest(): %s\n", strCmd.c_str() );
		#endif

		// monitor_psmove is calling us back with HMD tracking information
		vr::HmdMatrix34_t hmdTransform;


		#if LOG_REALIGN_TO_HMD != 0
			std::ostringstream matrixStringBuilder;
			matrixStringBuilder << "hmdTransform.m:\n\t";
		#endif

		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				ss >> hmdTransform.m[i][j];
				#if LOG_REALIGN_TO_HMD != 0
					matrixStringBuilder << "[" << hmdTransform.m[i][j] << "] ";
				#endif
			}
			#if LOG_REALIGN_TO_HMD != 0
				matrixStringBuilder << "\n\t";
			#endif
		}

		m_lastHMDPoseInMeters = openvrMatrixExtractPSMPosef(hmdTransform);

		#if LOG_REALIGN_TO_HMD != 0
			matrixStringBuilder << "\n";
			DriverLog(matrixStringBuilder.str().c_str());

			DriverLog("Extracted pose: %s \n", PSMPosefToString(m_lastHMDPoseInMeters).c_str() );
		#endif

		m_lastHMDPoseTime = std::chrono::high_resolution_clock::now();

		if (m_hmdResultCallback != nullptr)
		{
			m_hmdResultCallback(m_lastHMDPoseInMeters, m_hmdResultUserData);
			m_hmdResultCallback = nullptr;
			m_hmdResultUserData = nullptr;
		}
	}
}

void CPSMoveTrackedDeviceLatest::RequestLatestHMDPose(
	float maxPoseAgeMilliseconds,
	CPSMoveTrackedDeviceLatest::t_hmd_request_callback callback,
	void *userdata)
{
	#if LOG_REALIGN_TO_HMD != 0
		DriverLog("Begin CPSMoveTrackedDeviceLatest::RequestLatestHMDPose()\n");
	#endif

	assert(m_hmdResultCallback == nullptr || m_hmdResultCallback == callback);

	if (m_hmdResultCallback == nullptr)
	{
		bool bUsedCachedHMDPose;

		if (m_bIsLastHMDPoseValid)
		{
			std::chrono::duration<float, std::milli> hmdPoseAge =
				std::chrono::high_resolution_clock::now() - m_lastHMDPoseTime;

			bUsedCachedHMDPose = hmdPoseAge.count() < maxPoseAgeMilliseconds;
		}
		else
		{
			bUsedCachedHMDPose = false;
		}

		if (bUsedCachedHMDPose)
		{
			// Give the callback the cached pose immediately
			if (callback != nullptr)
			{
				callback(m_lastHMDPoseInMeters, userdata);
			}
		}
		else
		{
			static vr::VREvent_Data_t nodata = { 0 };

			// Register the callback
			m_hmdResultCallback = callback;
			m_hmdResultUserData = userdata;

			// Ask monitor_psmove to tell us the latest HMD pose
			vr::VRServerDriverHost()->VendorSpecificEvent(
				m_unSteamVRTrackedDeviceId,
				(vr::EVREventType) (vr::VREvent_VendorSpecific_Reserved_Start + 0),
				nodata,
				0);
		}
	}
}

vr::DriverPose_t CPSMoveTrackedDeviceLatest::GetPose()
{
    // This is only called at startup to synchronize with the driver.
    // Future updates are driven by our thread calling TrackedDevicePoseUpdated()
    return m_Pose;
}

// CPSMoveTrackedDeviceLatest Interface
vr::ETrackedDeviceClass CPSMoveTrackedDeviceLatest::GetTrackedDeviceClass() const
{
    return vr::TrackedDeviceClass_Invalid;
}

bool CPSMoveTrackedDeviceLatest::IsActivated() const
{
    return m_unSteamVRTrackedDeviceId != vr::k_unTrackedDeviceIndexInvalid;
}

void CPSMoveTrackedDeviceLatest::Update()
{
}

void CPSMoveTrackedDeviceLatest::RefreshWorldFromDriverPose()
{
	#if LOG_REALIGN_TO_HMD != 0
		DriverLog( "Begin CServerDriver_PSMoveService::RefreshWorldFromDriverPose() for device %s\n", GetSteamVRIdentifier() );
	#endif

    const PSMPosef worldFromDriverPose = g_ServerTrackedDeviceProvider.GetWorldFromDriverPose();

	#if LOG_REALIGN_TO_HMD != 0
		DriverLog("worldFromDriverPose: %s \n", PSMPosefToString(worldFromDriverPose).c_str());
	#endif

    // Transform used to convert from PSMove Tracking space to OpenVR Tracking Space
    m_Pose.qWorldFromDriverRotation.w = worldFromDriverPose.Orientation.w;
    m_Pose.qWorldFromDriverRotation.x = worldFromDriverPose.Orientation.x;
    m_Pose.qWorldFromDriverRotation.y = worldFromDriverPose.Orientation.y;
    m_Pose.qWorldFromDriverRotation.z = worldFromDriverPose.Orientation.z;
    m_Pose.vecWorldFromDriverTranslation[0] = worldFromDriverPose.Position.x;
    m_Pose.vecWorldFromDriverTranslation[1] = worldFromDriverPose.Position.y;
    m_Pose.vecWorldFromDriverTranslation[2] = worldFromDriverPose.Position.z;
}

const char *CPSMoveTrackedDeviceLatest::GetSteamVRIdentifier() const
{
    return m_strSteamVRSerialNo.c_str();
}

//==================================================================================================
// Controller Driver
//==================================================================================================

CPSMoveControllerLatest::CPSMoveControllerLatest( 
	PSMControllerID psmControllerId, 
	PSMControllerType psmControllerType,
	const char *psmSerialNo)
    : CPSMoveTrackedDeviceLatest()
    , m_nPSMControllerId(psmControllerId)
	, m_PSMControllerType(psmControllerType)
    , m_PSMControllerView(nullptr)
    , m_nPSMChildControllerId(-1)
	, m_PSMChildControllerType(PSMControllerType::PSMController_None)
    , m_PSMChildControllerView(nullptr)
    , m_nPoseSequenceNumber(0)
    , m_bIsBatteryCharging(false)
    , m_fBatteryChargeFraction(1.f)
	, m_bRumbleSuppressed(false)
    , m_pendingHapticPulseDuration(0)
    , m_lastTimeRumbleSent()
    , m_lastTimeRumbleSentValid(false)
	, m_resetPoseButtonPressTime()
	, m_bResetPoseRequestSent(false)
	, m_resetAlignButtonPressTime()
	, m_bResetAlignRequestSent(false)
	, m_bUsePSNaviDPadRecenter(false)
	, m_bUsePSNaviDPadRealign(false)
	, m_fVirtuallExtendControllersZMeters(0.0f)
	, m_fVirtuallExtendControllersYMeters(0.0f)
	, m_bDelayAfterTouchpadPress(false)
	, m_bTouchpadWasActive(false)
	, m_bUseSpatialOffsetAfterTouchpadPressAsTouchpadAxis(false)
	, m_touchpadDirectionsUsed(false)
	, m_fControllerMetersInFrontOfHmdAtCalibration(0.f)
	, m_posMetersAtTouchpadPressTime(*k_psm_float_vector3_zero)
	, m_driverSpaceRotationAtTouchpadPressTime(*k_psm_quaternion_identity)
	, m_bUseControllerOrientationInHMDAlignment(false)
	, m_triggerAxisIndex(1)
	, m_thumbstickDeadzone(k_defaultThumbstickDeadZoneRadius)
	, m_bThumbstickTouchAsPress(true)
{
    char svrIdentifier[256];
    GenerateControllerSteamVRIdentifier(svrIdentifier, sizeof(svrIdentifier), psmControllerId);
    m_strSteamVRSerialNo = svrIdentifier;

	m_lastTouchpadPressTime = std::chrono::high_resolution_clock::now();

	if (psmSerialNo != NULL) {
		m_strPSMControllerSerialNo = psmSerialNo;
	}

    // Tell PSM Client API that we are listening to this controller id
	PSM_AllocateControllerListener(psmControllerId);
    m_PSMControllerView = PSM_GetController(psmControllerId);

    memset(&m_ControllerState, 0, sizeof(vr::VRControllerState_t));
	m_trackingStatus = vr::TrackingResult_Uninitialized;

    // Load config from steamvr.vrsettings
    vr::IVRSettings *pSettings= vr::VRSettings();

    // Map every button to the system button initially
    memset(psButtonIDToVRButtonID, vr::k_EButton_SteamVR_Trigger, k_EPSControllerType_Count*k_EPSButtonID_Count*sizeof(vr::EVRButtonId));

	// Map every button to not be associated with any touchpad direction, initially
	memset(psButtonIDToVrTouchpadDirection, k_EVRTouchpadDirection_None, k_EPSControllerType_Count*k_EPSButtonID_Count*sizeof(vr::EVRButtonId));

	if (pSettings != nullptr)
	{
		// Load PSMove button/touchpad remapping from the settings for all possible controller buttons
		if (psmControllerType == PSMController_Move)
		{
			// Parent controller button mappings
	
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_PS, vr::k_EButton_System, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Move, vr::k_EButton_SteamVR_Touchpad, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Trigger, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Triangle, (vr::EVRButtonId)8, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Square, (vr::EVRButtonId)9, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Circle, (vr::EVRButtonId)10, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Cross, (vr::EVRButtonId)11, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Select, vr::k_EButton_Grip, k_EVRTouchpadDirection_None, psmControllerId);
			LoadButtonMapping(pSettings, k_EPSControllerType_Move, k_EPSButtonID_Start, vr::k_EButton_ApplicationMenu, k_EVRTouchpadDirection_None, psmControllerId);
	
			// Attached child controller button mappings
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_PS, vr::k_EButton_System, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Left, vr::k_EButton_DPad_Left, k_EVRTouchpadDirection_Left);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Up, (vr::EVRButtonId)10, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Right, vr::k_EButton_DPad_Right, k_EVRTouchpadDirection_Right);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Down, (vr::EVRButtonId)10, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Move, vr::k_EButton_SteamVR_Touchpad, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Circle, (vr::EVRButtonId)10, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_Cross, (vr::EVRButtonId)11, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_L1, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_L2, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_Navi, k_EPSButtonID_L3, vr::k_EButton_Grip, k_EVRTouchpadDirection_None);

			// Trigger mapping
			m_triggerAxisIndex = LoadInt(pSettings, "psmove", "trigger_axis_index", 1);

			// Touch pad settings
			m_bDelayAfterTouchpadPress = 
				LoadBool(pSettings, "psmove_touchpad", "delay_after_touchpad_press", m_bDelayAfterTouchpadPress);
			m_bUseSpatialOffsetAfterTouchpadPressAsTouchpadAxis= 
				LoadBool(pSettings, "psmove", "use_spatial_offset_after_touchpad_press_as_touchpad_axis", false);
			m_fMetersPerTouchpadAxisUnits= 
				LoadFloat(pSettings, "psmove", "meters_per_touchpad_units", .075f);

			// Chack for PSNavi up/down mappings
			char remapButtonToButtonString[32];
			vr::EVRSettingsError fetchError;

			pSettings->GetString("psnavi_button", k_PSButtonNames[k_EPSButtonID_Up], remapButtonToButtonString, 32, &fetchError);
			if (fetchError != vr::VRSettingsError_None)
			{
				pSettings->GetString("psnavi_touchpad", k_PSButtonNames[k_EPSButtonID_Up], remapButtonToButtonString, 32, &fetchError);
				if (fetchError != vr::VRSettingsError_None)
				{
					m_bUsePSNaviDPadRealign = true;
				}
			}

			pSettings->GetString("psnavi_button", k_PSButtonNames[k_EPSButtonID_Down], remapButtonToButtonString, 32, &fetchError);
			if (fetchError != vr::VRSettingsError_None)
			{
				pSettings->GetString("psnavi_touchpad", k_PSButtonNames[k_EPSButtonID_Down], remapButtonToButtonString, 32, &fetchError);
				if (fetchError != vr::VRSettingsError_None)
				{
					m_bUsePSNaviDPadRecenter = true;
				}
			}

			// General Settings
			m_bRumbleSuppressed= LoadBool(pSettings, "psmove_settings", "rumble_suppressed", m_bRumbleSuppressed);
			m_fVirtuallExtendControllersYMeters = LoadFloat(pSettings, "psmove_settings", "psmove_extend_y", 0.0f);
			m_fVirtuallExtendControllersZMeters = LoadFloat(pSettings, "psmove_settings", "psmove_extend_z", 0.0f);
			m_fControllerMetersInFrontOfHmdAtCalibration= 
				LoadFloat(pSettings, "psmove", "m_fControllerMetersInFrontOfHmdAtCallibration", 0.06f);
			m_bUseControllerOrientationInHMDAlignment= LoadBool(pSettings, "psmove_settings", "use_orientation_in_alignment", true);

			m_thumbstickDeadzone = 
				fminf(fmaxf(LoadFloat(pSettings, "psnavi_settings", "thumbstick_deadzone_radius", k_defaultThumbstickDeadZoneRadius), 0.f), 0.99f);
			m_bThumbstickTouchAsPress= LoadBool(pSettings, "psnavi_settings", "thumbstick_touch_as_press", true);

			#if LOG_TOUCHPAD_EMULATION != 0
			DriverLog("use_spatial_offset_after_touchpad_press_as_touchpad_axis: %d\n", m_bUseSpatialOffsetAfterTouchpadPressAsTouchpadAxis);
			DriverLog("meters_per_touchpad_units: %f\n", m_fMetersPerTouchpadAxisUnits);
			#endif

			#if LOG_REALIGN_TO_HMD != 0
			DriverLog("m_fControllerMetersInFrontOfHmdAtCalibration(psmove): %f\n", m_fControllerMetersInFrontOfHmdAtCalibration);
			#endif
		}
		else if (psmControllerType == PSMController_DualShock4)
		{
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_PS, vr::k_EButton_System, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Left, vr::k_EButton_DPad_Left, k_EVRTouchpadDirection_Left);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Up, vr::k_EButton_DPad_Up, k_EVRTouchpadDirection_Up);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Right, vr::k_EButton_DPad_Right, k_EVRTouchpadDirection_Right);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Down, vr::k_EButton_DPad_Down, k_EVRTouchpadDirection_Down);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Trackpad, vr::k_EButton_SteamVR_Touchpad, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Triangle, (vr::EVRButtonId)8, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Square, (vr::EVRButtonId)9, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Circle, (vr::EVRButtonId)10, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Cross, (vr::EVRButtonId)11, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Share, vr::k_EButton_ApplicationMenu, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_Options, vr::k_EButton_ApplicationMenu, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_L1, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_L2, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_L3, vr::k_EButton_Grip, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_R1, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_R2, vr::k_EButton_SteamVR_Trigger, k_EVRTouchpadDirection_None);
			LoadButtonMapping(pSettings, k_EPSControllerType_DS4, k_EPSButtonID_R3, vr::k_EButton_Grip, k_EVRTouchpadDirection_None);

			// General Settings
			m_bRumbleSuppressed= LoadBool(pSettings, "dualshock4_settings", "rumble_suppressed", m_bRumbleSuppressed);
			m_fControllerMetersInFrontOfHmdAtCalibration= 
				LoadFloat(pSettings, "dualshock4_settings", "cm_in_front_of_hmd_at_calibration", 16.f) / 100.f;

			#if LOG_REALIGN_TO_HMD != 0
			DriverLog("m_fControllerMetersInFrontOfHmdAtCalibration(ds4): %f\n", m_fControllerMetersInFrontOfHmdAtCalibration);
			#endif
		}
	}
}

CPSMoveControllerLatest::~CPSMoveControllerLatest()
{
	if (m_PSMChildControllerView != nullptr)
	{
		PSM_FreeControllerListener(m_PSMChildControllerView->ControllerID);
		m_PSMChildControllerView= nullptr;
	}

	PSM_FreeControllerListener(m_PSMControllerView->ControllerID);
    m_PSMControllerView= nullptr;
}

void CPSMoveControllerLatest::LoadButtonMapping(
    vr::IVRSettings *pSettings,
	const CPSMoveControllerLatest::ePSControllerType controllerType,
    const CPSMoveControllerLatest::ePSButtonID psButtonID,
    const vr::EVRButtonId defaultVRButtonID,
	const eVRTouchpadDirection defaultTouchpadDirection,
	int controllerId)
{

    vr::EVRButtonId vrButtonID = defaultVRButtonID;
	eVRTouchpadDirection vrTouchpadDirection = defaultTouchpadDirection;

    if (pSettings != nullptr)
    {
        const char *szPSButtonName = k_PSButtonNames[psButtonID];
        char remapButtonToButtonString[32];
        vr::EVRSettingsError fetchError;

		const char *szButtonSectionName= "";
		const char *szTouchpadSectionName= "";
		switch (controllerType)
		{
		case CPSMoveControllerLatest::k_EPSControllerType_Move:
			szButtonSectionName= "psmove";
			szTouchpadSectionName= "psmove_touchpad_directions";
			break;
		case CPSMoveControllerLatest::k_EPSControllerType_DS4:
			szButtonSectionName= "dualshock4_button";
			szTouchpadSectionName= "dualshock4_touchpad";
			break;
		case CPSMoveControllerLatest::k_EPSControllerType_Navi:
			szButtonSectionName= "psnavi_button";
			szTouchpadSectionName= "psnavi_touchpad";
			break;
		}

        pSettings->GetString(szButtonSectionName, szPSButtonName, remapButtonToButtonString, 32, &fetchError);

        if (fetchError == vr::VRSettingsError_None)
        {
            for (int vr_button_index = 0; vr_button_index < k_max_vr_buttons; ++vr_button_index)
            {
                if (strcasecmp(remapButtonToButtonString, k_VRButtonNames[vr_button_index]) == 0)
                {
                    vrButtonID = static_cast<vr::EVRButtonId>(vr_button_index);
                    break;
                }
            }
        }

		const char *numId = "";
		if (controllerId == 0) numId = "0";
		else if (controllerId == 1) numId = "1";
		else if (controllerId == 2) numId = "2";
		else if (controllerId == 3) numId = "3";
		else if (controllerId == 4) numId = "4";
		else if (controllerId == 5) numId = "5";
		else if (controllerId == 6) numId = "6";
		else if (controllerId == 7) numId = "7";
		else if (controllerId == 8) numId = "8";
		else if (controllerId == 9) numId = "9";

		if (strcmp(numId, "") != 0)
		{
			char buffer[64];
			strcpy(buffer, szButtonSectionName);
			strcat(buffer, "_");
			strcat(buffer, numId);
			szButtonSectionName = buffer;
			pSettings->GetString(szButtonSectionName, szPSButtonName, remapButtonToButtonString, 32, &fetchError);

			if (fetchError == vr::VRSettingsError_None)
			{
				for (int vr_button_index = 0; vr_button_index < k_max_vr_buttons; ++vr_button_index)
				{
					if (strcasecmp(remapButtonToButtonString, k_VRButtonNames[vr_button_index]) == 0)
					{
						vrButtonID = static_cast<vr::EVRButtonId>(vr_button_index);
						break;
					}
				}
			}
		}

		char remapButtonToTouchpadDirectionString[32];
		pSettings->GetString(szTouchpadSectionName, szPSButtonName, remapButtonToTouchpadDirectionString, 32, &fetchError);

		if (fetchError == vr::VRSettingsError_None)
		{
			for (int vr_touchpad_direction_index = 0; vr_touchpad_direction_index < k_max_vr_touchpad_directions; ++vr_touchpad_direction_index)
			{
				if (strcasecmp(remapButtonToTouchpadDirectionString, k_VRTouchpadDirectionNames[vr_touchpad_direction_index]) == 0)
				{
					vrTouchpadDirection = static_cast<eVRTouchpadDirection>(vr_touchpad_direction_index);
					break;
				}
			}
		}

		if (strcmp(numId, "") != 0)
		{
			char buffer[64];
			strcpy(buffer, szTouchpadSectionName);
			strcat(buffer, "_");
			strcat(buffer, numId); 
			szTouchpadSectionName = buffer;
			pSettings->GetString(szTouchpadSectionName, szPSButtonName, remapButtonToTouchpadDirectionString, 32, &fetchError);

			if (fetchError == vr::VRSettingsError_None)
			{
				for (int vr_touchpad_direction_index = 0; vr_touchpad_direction_index < k_max_vr_touchpad_directions; ++vr_touchpad_direction_index)
				{
					if (strcasecmp(remapButtonToTouchpadDirectionString, k_VRTouchpadDirectionNames[vr_touchpad_direction_index]) == 0)
					{
						vrTouchpadDirection = static_cast<eVRTouchpadDirection>(vr_touchpad_direction_index);
						break;
    }
				}
			}
		}
    }

    // Save the mapping
	assert(controllerType >= 0 && controllerType < k_EPSControllerType_Count);
	assert(psButtonID >= 0 && psButtonID < k_EPSButtonID_Count);
    psButtonIDToVRButtonID[controllerType][psButtonID] = vrButtonID;
	psButtonIDToVrTouchpadDirection[controllerType][psButtonID] = vrTouchpadDirection;
}

bool CPSMoveControllerLatest::LoadBool(
    vr::IVRSettings *pSettings,
	const char *pchSection,
	const char *pchSettingsKey,
	const bool bDefaultValue)
{
	vr::EVRSettingsError eError;
	bool bResult= pSettings->GetBool(pchSection, pchSettingsKey, &eError);

	if (eError != vr::VRSettingsError_None)
	{
		bResult= bDefaultValue;
	}
	
	return bResult;
}

int CPSMoveControllerLatest::LoadInt(
    vr::IVRSettings *pSettings,
	const char *pchSection,
	const char *pchSettingsKey,
	const int iDefaultValue)
{
	vr::EVRSettingsError eError;
	int iResult= pSettings->GetInt32(pchSection, pchSettingsKey, &eError);

	if (eError != vr::VRSettingsError_None)
	{
		iResult= iDefaultValue;
	}
	
	return iResult;
}

float CPSMoveControllerLatest::LoadFloat(
    vr::IVRSettings *pSettings,
	const char *pchSection,
	const char *pchSettingsKey,
	const float fDefaultValue)
{
	vr::EVRSettingsError eError;
	float fResult= pSettings->GetFloat(pchSection, pchSettingsKey, &eError);

	if (eError != vr::VRSettingsError_None)
	{
		fResult= fDefaultValue;
	}
	
	return fResult;
}

vr::EVRInitError CPSMoveControllerLatest::Activate(vr::TrackedDeviceIndex_t unObjectId)
{
    vr::EVRInitError result = CPSMoveTrackedDeviceLatest::Activate(unObjectId);

    if (result == vr::VRInitError_None)    
	{
		DriverLog("CPSMoveControllerLatest::Activate - Controller %d Activated\n", unObjectId);

		g_ServerTrackedDeviceProvider.LaunchPSMoveMonitor();

		PSMRequestID requestId;
		if (PSM_StartControllerDataStreamAsync(
				m_PSMControllerView->ControllerID, 
				PSMStreamFlags_includePositionData | PSMStreamFlags_includePhysicsData, 
				&requestId) == PSMResult_Success)
		{
			PSM_RegisterCallback(requestId, CPSMoveControllerLatest::start_controller_response_callback, this);
		}

		// Setup controller properties
		{
			vr::CVRPropertyHelpers *properties= vr::VRProperties();

			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceOff_String, "{psmove}controller_status_off.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearching_String, "{psmove}controller_status_ready.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{psmove}controller_status_ready_alert.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReady_String, "{psmove}controller_status_ready.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{psmove}controller_status_ready_alert.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceNotReady_String, "{psmove}controller_status_error.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceStandby_String, "{psmove}controller_status_ready.png");
			properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceAlertLow_String, "{psmove}controller_status_ready_low.png");

			properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_WillDriftInYaw_Bool, false);
			properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_DeviceIsWireless_Bool, true);
			properties->SetBoolProperty(m_ulPropertyContainer, vr::Prop_DeviceProvidesBatteryStatus_Bool, m_PSMControllerType == PSMController_Move);

			properties->SetInt32Property(m_ulPropertyContainer, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
			// We are reporting a "trackpad" type axis for better compatibility with Vive games
			properties->SetInt32Property(m_ulPropertyContainer, vr::Prop_Axis0Type_Int32, vr::k_eControllerAxis_TrackPad);
			properties->SetInt32Property(m_ulPropertyContainer, vr::Prop_Axis1Type_Int32, vr::k_eControllerAxis_Trigger);

			uint64_t ulRetVal= 0;
			for (int buttonIndex = 0; buttonIndex < static_cast<int>(k_EPSButtonID_Count); ++buttonIndex)
			{
				ulRetVal |= vr::ButtonMaskFromId( psButtonIDToVRButtonID[m_PSMControllerType][buttonIndex] );

				if( psButtonIDToVrTouchpadDirection[m_PSMControllerType][buttonIndex] != k_EVRTouchpadDirection_None )
				{
					ulRetVal |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
				}
			}
			properties->SetUint64Property(m_ulPropertyContainer, vr::Prop_SupportedButtons_Uint64, ulRetVal);

			// The {psmove} syntax lets us refer to rendermodels that are installed
			// in the driver's own resources/rendermodels directory.  The driver can
			// still refer to SteamVR models like "generic_hmd".
            char model_label[32]= "\0";
            switch(m_PSMControllerType)
			{
			case PSMController_Move:
                snprintf(model_label, sizeof(model_label), "psmove_%d", m_PSMControllerView->ControllerID);
                properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, "{psmove}psmove_controller");
				break;
			case PSMController_DualShock4:
                snprintf(model_label, sizeof(model_label), "dualshock4_%d", m_PSMControllerView->ControllerID);
				properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, "{psmove}dualshock4_controller");
				break;
			default:
                snprintf(model_label, sizeof(model_label), "unknown");
				properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, "generic_controller");
			}
            properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_ModeLabel_String, model_label);
		}
    }

    return result;
}

void CPSMoveControllerLatest::start_controller_response_callback(
    const PSMResponseMessage *response, void *userdata)
{
    CPSMoveControllerLatest *controller= reinterpret_cast<CPSMoveControllerLatest *>(userdata);

    if (response->result_code == PSMResult::PSMResult_Success)
    {
		DriverLog("CPSMoveControllerLatest::start_controller_response_callback - Controller stream started\n");
    }
}

void CPSMoveControllerLatest::Deactivate()
{
	DriverLog("CPSMoveControllerLatest::Deactivate - Controller stream stopped\n");
    PSM_StopControllerDataStreamAsync(m_PSMControllerView->ControllerID, nullptr);
}

void *CPSMoveControllerLatest::GetComponent(const char *pchComponentNameAndVersion)
{
    if (!strcasecmp(pchComponentNameAndVersion, vr::IVRControllerComponent_Version))
    {
        return (vr::IVRControllerComponent*)this;
    }

    return NULL;
}

vr::VRControllerState_t CPSMoveControllerLatest::GetControllerState()
{
    return m_ControllerState;
}

bool CPSMoveControllerLatest::TriggerHapticPulse( uint32_t unAxisId, uint16_t usPulseDurationMicroseconds )
{
    m_pendingHapticPulseDuration = usPulseDurationMicroseconds;
    UpdateRumbleState();

    return true;
}

void CPSMoveControllerLatest::SendButtonUpdates( ButtonUpdate ButtonEvent, uint64_t ulMask )
{
    if ( !ulMask )
        return;

    for ( int i = 0; i< vr::k_EButton_Max; i++ )
    {
        vr::EVRButtonId button = ( vr::EVRButtonId )i;

        uint64_t bit = ButtonMaskFromId( button );

        if ( bit & ulMask )
        {
            ( vr::VRServerDriverHost()->*ButtonEvent )( m_unSteamVRTrackedDeviceId, button, 0.0 );
        }
    }
}

void CPSMoveControllerLatest::UpdateControllerState()
{
    assert(m_PSMControllerView != nullptr);
    assert(m_PSMControllerView->IsConnected);

    vr::VRControllerState_t NewState = { 0 };

    // Changing unPacketNum tells anyone polling state that something might have
    // changed.  We don't try to be precise about that here.
    NewState.unPacketNum = m_ControllerState.unPacketNum + 1;
   
    switch (m_PSMControllerView->ControllerType)
    {
    case PSMController_Move:
        {
            const PSMPSMove &clientView = m_PSMControllerView->ControllerState.PSMoveState;

			bool bStartRealignHMDTriggered =
				(clientView.StartButton == PSMButtonState_PRESSED && clientView.SelectButton == PSMButtonState_PRESSED) ||
				(clientView.StartButton == PSMButtonState_PRESSED && clientView.SelectButton == PSMButtonState_DOWN) ||
				(clientView.StartButton == PSMButtonState_DOWN && clientView.SelectButton == PSMButtonState_PRESSED);

			// Check if the PSMove has a PSNavi child
			const bool bHasChildNavi= 
				m_PSMChildControllerView != nullptr && 
				m_PSMChildControllerView->ControllerType == PSMController_Navi;

			// See if the recenter button has been held for the requisite amount of time
			bool bRecenterRequestTriggered = false;
			{
				PSMButtonState resetPoseButtonState = clientView.SelectButton;
				PSMButtonState resetAlignButtonState;

				// Use PSNavi D-pad up/down if they are free
				if (bHasChildNavi)
				{
					if (m_bUsePSNaviDPadRealign)
					{
						resetAlignButtonState = m_PSMChildControllerView->ControllerState.PSNaviState.DPadUpButton;

						switch (resetAlignButtonState)
						{
						case PSMButtonState_PRESSED:
							{
								m_resetAlignButtonPressTime = std::chrono::high_resolution_clock::now();
							} break;
						case PSMButtonState_DOWN:
							{
								if (!m_bResetAlignRequestSent)
								{
									const float k_hold_duration_milli = 1000.f;
									std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();
									std::chrono::duration<float, std::milli> pressDurationMilli = now - m_resetAlignButtonPressTime;

									if (pressDurationMilli.count() >= k_hold_duration_milli)
									{
										bStartRealignHMDTriggered = true;
									}
								}
							} break;
						case PSMButtonState_RELEASED:
							{
								m_bResetAlignRequestSent = false;
							} break;
						}
					}
					
					if (m_bUsePSNaviDPadRecenter)
					{
						resetPoseButtonState = m_PSMChildControllerView->ControllerState.PSNaviState.DPadDownButton;
					}
				}

				switch (resetPoseButtonState)
				{
				case PSMButtonState_PRESSED:
					{
						m_resetPoseButtonPressTime = std::chrono::high_resolution_clock::now();
					} break;
				case PSMButtonState_DOWN:
					{
						if (!m_bResetPoseRequestSent)
						{
							const float k_hold_duration_milli = (bHasChildNavi) ? 1000.f : 250.f;
							std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();
							std::chrono::duration<float, std::milli> pressDurationMilli = now - m_resetPoseButtonPressTime;

							if (pressDurationMilli.count() >= k_hold_duration_milli)
							{
								bRecenterRequestTriggered = true;
							}
						}
					} break;
				case PSMButtonState_RELEASED:
					{
						m_bResetPoseRequestSent = false;
					} break;
				}
			}

            // If START was just pressed while and SELECT was held or vice versa,
			// recenter the controller orientation pose and start the realignment of the controller to HMD tracking space.
            if (bStartRealignHMDTriggered)                
            {
				PSMVector3f controllerBallPointedUpEuler = {(float)M_PI_2, 0.0f, 0.0f};
				PSMQuatf controllerBallPointedUpQuat = PSM_QuatfCreateFromAngles(&controllerBallPointedUpEuler);

				#if LOG_REALIGN_TO_HMD != 0
				DriverLog("CPSMoveControllerLatest::UpdateControllerState(): Calling StartRealignHMDTrackingSpace() in response to controller chord.\n");
				#endif

				PSM_ResetControllerOrientationAsync(m_PSMControllerView->ControllerID, &controllerBallPointedUpQuat, nullptr);
				m_bResetPoseRequestSent = true;

				StartRealignHMDTrackingSpace();
				m_bResetAlignRequestSent = true;
            }
			else if (bRecenterRequestTriggered)
			{
				DriverLog("CPSMoveControllerLatest::UpdateControllerState(): Calling ClientPSMoveAPI::reset_orientation() in response to controller button press.\n");

				PSM_ResetControllerOrientationAsync(m_PSMControllerView->ControllerID, k_psm_quaternion_identity, nullptr);
				m_bResetPoseRequestSent = true;
			}
			else 
			{
				// Process all the button mappings 
				// ------

				// Handle buttons/virtual touchpad buttons on the psmove
				m_touchpadDirectionsUsed = false;
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Circle, clientView.CircleButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Cross, clientView.CrossButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Move, clientView.MoveButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_PS, clientView.PSButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Select, clientView.SelectButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Square, clientView.SquareButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Start, clientView.StartButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Triangle, clientView.TriangleButton, &NewState);
				UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Move, k_EPSButtonID_Trigger, clientView.TriggerButton, &NewState);

				// Handle buttons/virtual touchpad buttons on the psnavi
				if (bHasChildNavi)
				{
					const PSMPSNavi &naviClientView = m_PSMChildControllerView->ControllerState.PSNaviState;

					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Circle, naviClientView.CircleButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Cross, naviClientView.CrossButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_PS, naviClientView.PSButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Up, naviClientView.DPadUpButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Down, naviClientView.DPadDownButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Left, naviClientView.DPadLeftButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_Right, naviClientView.DPadRightButton, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_L1, naviClientView.L1Button, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_L2, naviClientView.L2Button, &NewState);
					UpdateControllerStateFromPsMoveButtonState(k_EPSControllerType_Navi, k_EPSButtonID_L3, naviClientView.L3Button, &NewState);
				}

				// Touchpad handling
				if (!m_touchpadDirectionsUsed)
				{
					static const uint64_t s_kTouchpadButtonMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);

					// PSNavi TouchPad Handling (i.e. thumbstick as touchpad)
					if (bHasChildNavi)
					{
						const PSMPSNavi &naviClientView = m_PSMChildControllerView->ControllerState.PSNaviState;
						const float thumbStickX = naviClientView.Stick_XAxis;
						const float thumbStickY = naviClientView.Stick_YAxis;
						const float thumbStickRadialDist= sqrtf(thumbStickX*thumbStickX + thumbStickY*thumbStickY);

						if (thumbStickRadialDist >= m_thumbstickDeadzone)
						{
							// Rescale the thumbstick position to hide the dead zone
							const float rescaledRadius= (thumbStickRadialDist - m_thumbstickDeadzone) / (1.f - m_thumbstickDeadzone);

							// Set the thumbstick axis
							NewState.rAxis[0].x = (rescaledRadius / thumbStickRadialDist) * thumbStickX;
							NewState.rAxis[0].y = (rescaledRadius / thumbStickRadialDist) * thumbStickY;

							// Also make sure the touchpad is considered "touched" 
							// if the thumbstick is outside of the deadzone
							NewState.ulButtonTouched |= s_kTouchpadButtonMask;

							// If desired, also treat the touch as a press
							if (m_bThumbstickTouchAsPress)
							{
								NewState.ulButtonPressed |= s_kTouchpadButtonMask;
							}
						}
					}
					// Virtual TouchPad h=Handling (i.e. controller spatial offset as touchpad) 
					else if (m_bUseSpatialOffsetAfterTouchpadPressAsTouchpadAxis)
					{
						bool bTouchpadIsActive = (NewState.ulButtonPressed & s_kTouchpadButtonMask) || (NewState.ulButtonTouched & s_kTouchpadButtonMask);

						if (bTouchpadIsActive)
						{
							bool bIsNewTouchpadLocation = true;

							if (m_bDelayAfterTouchpadPress)
							{					
								std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

								if (!m_bTouchpadWasActive)
								{
									const float k_max_touchpad_press = 2000.0; // time until coordinates are reset, otherwise assume in last location.
									std::chrono::duration<double, std::milli> timeSinceActivated = now - m_lastTouchpadPressTime;

									bIsNewTouchpadLocation = timeSinceActivated.count() >= k_max_touchpad_press;
								}
								m_lastTouchpadPressTime = now;

							}

							if (bIsNewTouchpadLocation)
							{
								if (!m_bTouchpadWasActive)
								{
									// Just pressed.
									const PSMPSMove &view = m_PSMControllerView->ControllerState.PSMoveState;
									m_driverSpaceRotationAtTouchpadPressTime = view.Pose.Orientation;

									GetMetersPosInRotSpace(&m_driverSpaceRotationAtTouchpadPressTime, &m_posMetersAtTouchpadPressTime);

									#if LOG_TOUCHPAD_EMULATION != 0
									DriverLog("Touchpad pressed! At (%f, %f, %f) meters relative to orientation\n",
										m_posMetersAtTouchpadPressTime.x, m_posMetersAtTouchpadPressTime.y, m_posMetersAtTouchpadPressTime.z);
									#endif
								}
								else
								{
									// Held!
									PSMVector3f newPosMeters;
									GetMetersPosInRotSpace(&m_driverSpaceRotationAtTouchpadPressTime, &newPosMeters);

									PSMVector3f offsetMeters = PSM_Vector3fSubtract(&newPosMeters, &m_posMetersAtTouchpadPressTime);

									#if LOG_TOUCHPAD_EMULATION != 0
									DriverLog("Touchpad held! Relative position (%f, %f, %f) meters\n",
										offsetMeters.x, offsetMeters.y, offsetMeters.z);
									#endif

									NewState.rAxis[0].x = offsetMeters.x / m_fMetersPerTouchpadAxisUnits;
									NewState.rAxis[0].x = fminf(fmaxf(NewState.rAxis[0].x, -1.0f), 1.0f);

									NewState.rAxis[0].y = -offsetMeters.z / m_fMetersPerTouchpadAxisUnits;
									NewState.rAxis[0].y = fminf(fmaxf(NewState.rAxis[0].y, -1.0f), 1.0f);

									#if LOG_TOUCHPAD_EMULATION != 0
									DriverLog("Touchpad axis at (%f, %f) \n",
										NewState.rAxis[0].x, NewState.rAxis[0].y);
									#endif
								}
							}
						}

						// Remember if the touchpad was active the previous frame for edge detection
						m_bTouchpadWasActive = bTouchpadIsActive;
					}
				}

				// Touchpad SteamVR Events
				if (NewState.rAxis[0].x != m_ControllerState.rAxis[0].x || 
					NewState.rAxis[0].y != m_ControllerState.rAxis[0].y)
				{					
					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, 0, NewState.rAxis[0]);
				}

				// PSMove Trigger handling
				NewState.rAxis[m_triggerAxisIndex].x = clientView.TriggerValue / 255.f;
				NewState.rAxis[m_triggerAxisIndex].y = 0.f;

				// Attached PSNavi Trigger handling
				if (bHasChildNavi)
				{
					const PSMPSNavi &naviClientView = m_PSMChildControllerView->ControllerState.PSNaviState;

					NewState.rAxis[m_triggerAxisIndex].x = fmaxf(NewState.rAxis[m_triggerAxisIndex].x, naviClientView.TriggerValue / 255.f);
				}

				// Trigger SteamVR Events
				if (NewState.rAxis[m_triggerAxisIndex].x != m_ControllerState.rAxis[m_triggerAxisIndex].x)
				{
					if (NewState.rAxis[m_triggerAxisIndex].x > 0.1f)
					{
						NewState.ulButtonTouched |= vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(vr::k_EButton_Axis0 + m_triggerAxisIndex));
					}

					if (NewState.rAxis[m_triggerAxisIndex].x > 0.8f)
					{
						NewState.ulButtonPressed |= vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(vr::k_EButton_Axis0 + m_triggerAxisIndex));
					}

					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, m_triggerAxisIndex, NewState.rAxis[m_triggerAxisIndex]);
				}

				// Update the battery charge state
				UpdateBatteryChargeState(m_PSMControllerView->ControllerState.PSMoveState.BatteryValue);
			}
        } break;
    case PSMController_DualShock4:
        {
            const PSMDualShock4 &clientView = m_PSMControllerView->ControllerState.PSDS4State;

			const bool bStartRealignHMDTriggered =
				(clientView.ShareButton == PSMButtonState_PRESSED && clientView.OptionsButton == PSMButtonState_PRESSED) ||
				(clientView.ShareButton == PSMButtonState_PRESSED && clientView.OptionsButton == PSMButtonState_DOWN) ||
				(clientView.ShareButton == PSMButtonState_DOWN && clientView.OptionsButton == PSMButtonState_PRESSED);

			// See if the recenter button has been held for the requisite amount of time
			bool bRecenterRequestTriggered = false;
			{
				PSMButtonState resetPoseButtonState = clientView.OptionsButton;

				switch (resetPoseButtonState)
				{
				case PSMButtonState_PRESSED:
					{
						m_resetPoseButtonPressTime = std::chrono::high_resolution_clock::now();
					} break;
				case PSMButtonState_DOWN:
				{
					if (!m_bResetPoseRequestSent)
					{
						const float k_hold_duration_milli = 250.f;
						std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();
						std::chrono::duration<float, std::milli> pressDurationMilli = now - m_resetPoseButtonPressTime;

						if (pressDurationMilli.count() >= k_hold_duration_milli)
						{
							bRecenterRequestTriggered = true;
						}
					}
				} break;
				case PSMButtonState_RELEASED:
					{
						m_bResetPoseRequestSent = false;
					} break;
				}
			}

			// If SHARE was just pressed while and OPTIONS was held or vice versa,
			// recenter the controller orientation pose and start the realignment of the controller to HMD tracking space.
			if (bStartRealignHMDTriggered)
			{
				#if LOG_REALIGN_TO_HMD != 0
				DriverLog("CPSMoveControllerLatest::UpdateControllerState(): Calling StartRealignHMDTrackingSpace() in response to controller chord.\n");
				#endif

				PSM_ResetControllerOrientationAsync(m_PSMControllerView->ControllerID, k_psm_quaternion_identity, nullptr);
				m_bResetPoseRequestSent = true;

				StartRealignHMDTrackingSpace();
			}
			else if (bRecenterRequestTriggered)
			{
				DriverLog("CPSMoveControllerLatest::UpdateControllerState(): Calling ClientPSMoveAPI::reset_orientation() in response to controller button press.\n");

				PSM_ResetControllerOrientationAsync(m_PSMControllerView->ControllerID, k_psm_quaternion_identity, nullptr);
				m_bResetPoseRequestSent = true;
			}
			else
			{
				if (clientView.L1Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_L1]);
				if (clientView.L2Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_L2]);
				if (clientView.L3Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_L3]);
				if (clientView.R1Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_R1]);
				if (clientView.R2Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_R2]);
				if (clientView.R3Button)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_R3]);

				if (clientView.CircleButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Circle]);
				if (clientView.CrossButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Cross]);
				if (clientView.SquareButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Square]);
				if (clientView.TriangleButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Triangle]);

				if (clientView.DPadUpButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Up]);
				if (clientView.DPadDownButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Down]);
				if (clientView.DPadLeftButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Left]);
				if (clientView.DPadRightButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Right]);

				if (clientView.OptionsButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Options]);
				if (clientView.ShareButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Share]);
				if (clientView.TrackPadButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_Trackpad]);
				if (clientView.PSButton)
					NewState.ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[k_EPSControllerType_DS4][k_EPSButtonID_PS]);

				NewState.rAxis[0].x = clientView.LeftAnalogX;
				NewState.rAxis[0].y = -clientView.LeftAnalogY;

				NewState.rAxis[1].x = clientView.LeftTriggerValue;
				NewState.rAxis[1].y = 0.f;

				NewState.rAxis[2].x = clientView.RightAnalogX;
				NewState.rAxis[2].y = -clientView.RightAnalogY;

				NewState.rAxis[3].x = clientView.RightTriggerValue;
				NewState.rAxis[3].y = 0.f;

				if (NewState.rAxis[0].x != m_ControllerState.rAxis[0].x || NewState.rAxis[0].y != m_ControllerState.rAxis[0].y)
					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, 0, NewState.rAxis[0]);
				if (NewState.rAxis[1].x != m_ControllerState.rAxis[1].x)
					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, 1, NewState.rAxis[1]);

				if (NewState.rAxis[2].x != m_ControllerState.rAxis[2].x || NewState.rAxis[2].y != m_ControllerState.rAxis[2].y)
					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, 2, NewState.rAxis[2]);
				if (NewState.rAxis[3].x != m_ControllerState.rAxis[3].x)
					vr::VRServerDriverHost()->TrackedDeviceAxisUpdated(m_unSteamVRTrackedDeviceId, 3, NewState.rAxis[3]);
			}
        } break;
    }

    // All pressed buttons are touched
    NewState.ulButtonTouched |= NewState.ulButtonPressed;

    uint64_t ulChangedTouched = NewState.ulButtonTouched ^ m_ControllerState.ulButtonTouched;
    uint64_t ulChangedPressed = NewState.ulButtonPressed ^ m_ControllerState.ulButtonPressed;

    SendButtonUpdates( &vr::IVRServerDriverHost::TrackedDeviceButtonTouched, ulChangedTouched & NewState.ulButtonTouched );
    SendButtonUpdates( &vr::IVRServerDriverHost::TrackedDeviceButtonPressed, ulChangedPressed & NewState.ulButtonPressed );
    SendButtonUpdates( &vr::IVRServerDriverHost::TrackedDeviceButtonUnpressed, ulChangedPressed & ~NewState.ulButtonPressed );
    SendButtonUpdates( &vr::IVRServerDriverHost::TrackedDeviceButtonUntouched, ulChangedTouched & ~NewState.ulButtonTouched );

    m_ControllerState = NewState;
}


void CPSMoveControllerLatest::UpdateControllerStateFromPsMoveButtonState(
	ePSControllerType controllerType,
	ePSButtonID buttonId,
	PSMButtonState buttonState, 
	vr::VRControllerState_t* pControllerStateToUpdate)
{
	if (buttonState & PSMButtonState_PRESSED || buttonState & PSMButtonState_DOWN)
	{
		if (psButtonIDToVRButtonID[controllerType][buttonId] == k_touchpadTouchMapping) {
			pControllerStateToUpdate->ulButtonTouched |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
		}
		else {
			pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(psButtonIDToVRButtonID[controllerType][buttonId]);

			if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_Left)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = -1.0f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_Right)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = 1.0f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_Up)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].y = 1.0f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_Down)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].y = -1.0f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_UpLeft)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = -0.707f;
				pControllerStateToUpdate->rAxis[0].y = 0.707f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_UpRight)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = 0.707f;
				pControllerStateToUpdate->rAxis[0].y = 0.707f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_DownLeft)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = -0.707f;
				pControllerStateToUpdate->rAxis[0].y = -0.707f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
			else if (psButtonIDToVrTouchpadDirection[controllerType][buttonId] == k_EVRTouchpadDirection_DownRight)
			{
				m_touchpadDirectionsUsed = true;
				pControllerStateToUpdate->rAxis[0].x = 0.707f;
				pControllerStateToUpdate->rAxis[0].y = -0.707f;
				pControllerStateToUpdate->ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
			}
		}
	}
}

PSMQuatf ExtractHMDYawQuaternion(const PSMQuatf &q)
{
    // Convert the quaternion to a basis matrix
    const PSMMatrix3f hmd_orientation = PSM_Matrix3fCreateFromQuatf(&q);

    // Extract the forward (z-axis) vector from the basis
    const PSMVector3f forward = PSM_Matrix3fBasisZ(&hmd_orientation);
	PSMVector3f forward2d = {forward.x, 0.f, forward.z};
	forward2d= PSM_Vector3fNormalizeWithDefault(&forward2d, k_psm_float_vector3_k);

    // Compute the yaw angle (amount the z-axis has been rotated to it's current facing)
    const float cos_yaw = PSM_Vector3fDot(&forward, k_psm_float_vector3_k);
    float half_yaw = acosf(fminf(fmaxf(cos_yaw, -1.f), 1.f)) / 2.f;

	// Flip the sign of the yaw angle depending on if forward2d is to the left or right of global forward
	PSMVector3f yaw_axis = PSM_Vector3fCross(k_psm_float_vector3_k, &forward2d);
	if (PSM_Vector3fDot(&yaw_axis, k_psm_float_vector3_j) < 0)
	{
		half_yaw = -half_yaw;
	}

    // Convert this yaw rotation back into a quaternion
    PSMQuatf yaw_quaternion =
        PSM_QuatfCreate(
            cosf(half_yaw), // w = cos(theta/2)
            0.f, sinf(half_yaw), 0.f); // (x, y, z) = sin(theta/2)*axis, where axis = (0, 1, 0)

    return yaw_quaternion;
}

PSMQuatf ExtractPSMoveYawQuaternion(const PSMQuatf &q)
{
	// Convert the quaternion to a basis matrix
	const PSMMatrix3f psmove_basis = PSM_Matrix3fCreateFromQuatf(&q);

	// Extract the forward (negative z-axis) vector from the basis
	const PSMVector3f global_forward = {0.f, 0.f, -1.f};
	const PSMVector3f &forward = PSM_Matrix3fBasisY(&psmove_basis);
	PSMVector3f forward2d = {forward.x, 0.f, forward.z};
	forward2d= PSM_Vector3fNormalizeWithDefault(&forward2d, &global_forward);

	// Compute the yaw angle (amount the z-axis has been rotated to it's current facing)
	const float cos_yaw = PSM_Vector3fDot(&forward, &global_forward);
	float yaw = acosf(fminf(fmaxf(cos_yaw, -1.f), 1.f));

	// Flip the sign of the yaw angle depending on if forward2d is to the left or right of global forward
	const PSMVector3f &global_up = *k_psm_float_vector3_j;
	PSMVector3f yaw_axis = PSM_Vector3fCross(&global_forward, &forward2d);
	if (PSM_Vector3fDot(&yaw_axis, &global_up) < 0)
	{
		yaw = -yaw;
	}

	// Convert this yaw rotation back into a quaternion
	PSMVector3f eulerPitch= {(float)1.57079632679489661923, 0.f, 0.f}; // pitch 90 up first
	PSMVector3f eulerYaw= {0, yaw, 0};
	PSMQuatf quatPitch= PSM_QuatfCreateFromAngles(&eulerPitch);
	PSMQuatf quatYaw= PSM_QuatfCreateFromAngles(&eulerYaw);
	PSMQuatf yaw_quaternion =
		PSM_QuatfConcat(
			&quatPitch, // pitch 90 up first
			&quatYaw); // Then apply the yaw

	return yaw_quaternion;
}

void CPSMoveControllerLatest::GetMetersPosInRotSpace(const PSMQuatf *rotation, PSMVector3f* out_position)
{
	const PSMPSMove &view = m_PSMControllerView->ControllerState.PSMoveState;
	const PSMVector3f &position = view.Pose.Position;

	PSMVector3f unrotatedPositionMeters= PSM_Vector3fScale(&position, k_fScalePSMoveAPIToMeters);
	PSMQuatf viewOrientationInverse	= PSM_QuatfConjugate(rotation);

	*out_position = PSM_QuatfRotateVector(&viewOrientationInverse, &unrotatedPositionMeters);
}

void CPSMoveControllerLatest::StartRealignHMDTrackingSpace()
{
	#if LOG_REALIGN_TO_HMD != 0
		DriverLog( "Begin CPSMoveControllerLatest::StartRealignHMDTrackingSpace()\n" );
	#endif

	if (m_trackingStatus != vr::TrackingResult_Calibrating_InProgress)
	{
		m_trackingStatus = vr::TrackingResult_Calibrating_InProgress;
		RequestLatestHMDPose(0.f, CPSMoveControllerLatest::FinishRealignHMDTrackingSpace, this);
	}
}

void CPSMoveControllerLatest::FinishRealignHMDTrackingSpace(
	const PSMPosef &hmd_pose_raw_meters, 
	void *userdata)
{

	CPSMoveControllerLatest* pThis = (CPSMoveControllerLatest*)userdata;

	#if LOG_REALIGN_TO_HMD != 0
		DriverLog("Begin CPSMoveControllerLatest::FinishRealignHMDTrackingSpace()\n");
	#endif

	PSMPosef hmd_pose_meters = hmd_pose_raw_meters;
	DriverLog("hmd_pose_meters(raw): %s \n", PSMPosefToString(hmd_pose_meters).c_str());

	// Make the HMD orientation only contain a yaw
	hmd_pose_meters.Orientation = ExtractHMDYawQuaternion(hmd_pose_raw_meters.Orientation);
	DriverLog("hmd_pose_meters(yaw-only): %s \n", PSMPosefToString(hmd_pose_meters).c_str());

	// We have the transform of the HMD in world space. 
	// However the HMD and the controller aren't quite aligned depending on the controller type:
	PSMQuatf controllerOrientationInHmdSpaceQuat= *k_psm_quaternion_identity;
	PSMVector3f controllerLocalOffsetFromHmdPosition= *k_psm_float_vector3_zero;
	if (pThis->m_PSMControllerType == PSMControllerType::PSMController_Move)
	{
		// Rotation) The controller's local -Z axis (from the center to the glowing ball) is currently pointed 
		//    in the direction of the HMD's local +Y axis, 
		// Translation) The controller's position is a few inches ahead of the HMD's on the HMD's local -Z axis. 
		PSMVector3f eulerPitch= {(float)M_PI_2, 0.0f, 0.0f};
		controllerOrientationInHmdSpaceQuat = PSM_QuatfCreateFromAngles(&eulerPitch);
		controllerLocalOffsetFromHmdPosition = {0.0f, 0.0f, -1.0f * pThis->m_fControllerMetersInFrontOfHmdAtCalibration};
	}
	else if (pThis->m_PSMControllerType == PSMControllerType::PSMController_DualShock4)
	{
		// Translation) The controller's position is a few inches ahead of the HMD's on the HMD's local -Z axis. 
		controllerLocalOffsetFromHmdPosition = {0.0f, 0.0f, -1.0f * pThis->m_fControllerMetersInFrontOfHmdAtCalibration};
	}

	// Transform the HMD's world space transform to where we expect the controller's world space transform to be.
	PSMPosef controllerPoseRelativeToHMD =
		PSM_PosefCreate(&controllerLocalOffsetFromHmdPosition, &controllerOrientationInHmdSpaceQuat);

	DriverLog("controllerPoseRelativeToHMD: %s \n", PSMPosefToString(controllerPoseRelativeToHMD).c_str());

	// Compute the expected controller pose in HMD tracking space (i.e. "World Space")
	PSMPosef controller_world_space_pose = PSM_PosefConcat(&controllerPoseRelativeToHMD, &hmd_pose_meters);
	DriverLog("controller_world_space_pose: %s \n", PSMPosefToString(controller_world_space_pose).c_str());


	// We now have the transform of the controller in world space -- controller_world_space_pose

	// We also have the transform of the controller in driver space -- psmove_pose_meters

	// We need the transform that goes from driver space to world space -- driver_pose_to_world_pose
	// psmove_pose_meters * driver_pose_to_world_pose = controller_world_space_pose
	// psmove_pose_meters.inverse() * psmove_pose_meters * driver_pose_to_world_pose = psmove_pose_meters.inverse() * controller_world_space_pose
	// driver_pose_to_world_pose = psmove_pose_meters.inverse() * controller_world_space_pose
	CPSMoveControllerLatest *controller = reinterpret_cast<CPSMoveControllerLatest *>(userdata);

	// Get the current pose from the controller view instead of using the driver's cached
	// value because the user may have triggered a pose reset, in which case the driver's
	// cached pose might not yet be up to date by the time this callback is triggered.
	PSMPosef controller_pose_meters = *k_psm_pose_identity;
	if (pThis->m_PSMControllerType == PSMControllerType::PSMController_Move)
	{
		controller_pose_meters = controller->m_PSMControllerView->ControllerState.PSMoveState.Pose;
	}
	else if (pThis->m_PSMControllerType == PSMControllerType::PSMController_DualShock4)
	{
		controller_pose_meters = controller->m_PSMControllerView->ControllerState.PSDS4State.Pose;
	}
	DriverLog("controller_pose_meters(raw): %s \n", PSMPosefToString(controller_pose_meters).c_str());

	// PSMove Position is in cm, but OpenVR stores position in meters
	controller_pose_meters.Position= PSM_Vector3fScale(&controller_pose_meters.Position, k_fScalePSMoveAPIToMeters);

	if (pThis->m_PSMControllerType == PSMControllerType::PSMController_Move)
	{
		if (pThis->m_bUseControllerOrientationInHMDAlignment)
		{
			// Extract only the yaw from the controller orientation (assume it's mostly held upright)
			controller_pose_meters.Orientation = ExtractPSMoveYawQuaternion(controller_pose_meters.Orientation);
			DriverLog("controller_pose_meters(yaw-only): %s \n", PSMPosefToString(controller_pose_meters).c_str());
		}
		else
		{
			const PSMVector3f eulerPitch= {(float)M_PI_2, 0.0f, 0.0f};

			controller_pose_meters.Orientation = PSM_QuatfCreateFromAngles(&eulerPitch);
			DriverLog("controller_pose_meters(no-rotation): %s \n", PSMPosefToString(controller_pose_meters).c_str());
		}
	}
	else if (pThis->m_PSMControllerType == PSMControllerType::PSMController_DualShock4)
	{
		controller_pose_meters.Orientation = *k_psm_quaternion_identity;
		DriverLog("controller_pose_meters(no-rotation): %s \n", PSMPosefToString(controller_pose_meters).c_str());
	}	

	PSMPosef controller_pose_inv = PSM_PosefInverse(&controller_pose_meters);
	DriverLog("controller_pose_inv: %s \n", PSMPosefToString(controller_pose_inv).c_str());

	PSMPosef driver_pose_to_world_pose = PSM_PosefConcat(&controller_pose_inv, &controller_world_space_pose);
	DriverLog("driver_pose_to_world_pose: %s \n", PSMPosefToString(driver_pose_to_world_pose).c_str());

	PSMPosef test_composed_controller_world_space = PSM_PosefConcat(&controller_pose_meters, &driver_pose_to_world_pose);
	DriverLog("test_composed_controller_world_space: %s \n", PSMPosefToString(test_composed_controller_world_space).c_str());

	g_ServerTrackedDeviceProvider.SetHMDTrackingSpace(driver_pose_to_world_pose);
}

void CPSMoveControllerLatest::UpdateTrackingState()
{
    assert(m_PSMControllerView != nullptr);
    assert(m_PSMControllerView->IsConnected);

	// The tracking status will be one of the following states:
    m_Pose.result = m_trackingStatus;

    m_Pose.deviceIsConnected = m_PSMControllerView->IsConnected;

    // These should always be false from any modern driver.  These are for Oculus DK1-like
    // rotation-only tracking.  Support for that has likely rotted in vrserver.
    m_Pose.willDriftInYaw = false;
    m_Pose.shouldApplyHeadModel = false;

    switch (m_PSMControllerView->ControllerType)
    {
    case PSMControllerType::PSMController_Move:
        {
            const PSMPSMove &view= m_PSMControllerView->ControllerState.PSMoveState;

            // No prediction since that's already handled in the psmove service
            m_Pose.poseTimeOffset = 0.f;

            // No transform due to the current HMD orientation
            m_Pose.qDriverFromHeadRotation.w = 1.f;
            m_Pose.qDriverFromHeadRotation.x = 0.0f;
            m_Pose.qDriverFromHeadRotation.y = 0.0f;
            m_Pose.qDriverFromHeadRotation.z = 0.0f;
            m_Pose.vecDriverFromHeadTranslation[0] = 0.f;
            m_Pose.vecDriverFromHeadTranslation[1] = 0.f;
            m_Pose.vecDriverFromHeadTranslation[2] = 0.f;            

            // Set position
            {
                const PSMVector3f &position = view.Pose.Position;

                m_Pose.vecPosition[0] = position.x * k_fScalePSMoveAPIToMeters;
                m_Pose.vecPosition[1] = position.y * k_fScalePSMoveAPIToMeters;
                m_Pose.vecPosition[2] = position.z * k_fScalePSMoveAPIToMeters;
            }

			// virtual extend controllers
			if (m_fVirtuallExtendControllersYMeters != 0.0f || m_fVirtuallExtendControllersZMeters != 0.0f)
			{
				const PSMQuatf &orientation = view.Pose.Orientation;

				PSMVector3f shift = {(float)m_Pose.vecPosition[0], (float)m_Pose.vecPosition[1], (float)m_Pose.vecPosition[2]};

				if (m_fVirtuallExtendControllersZMeters != 0.0f) {

					PSMVector3f local_forward = {0, 0, -1};
					PSMVector3f global_forward = PSM_QuatfRotateVector(&orientation, &local_forward);

					shift = PSM_Vector3fScaleAndAdd(&global_forward, m_fVirtuallExtendControllersZMeters, &shift);
				}

				if (m_fVirtuallExtendControllersYMeters != 0.0f) {

					PSMVector3f local_forward = {0, -1, 0};
					PSMVector3f global_forward = PSM_QuatfRotateVector(&orientation, &local_forward);

					shift = PSM_Vector3fScaleAndAdd(&global_forward, m_fVirtuallExtendControllersYMeters, &shift);
				}

				m_Pose.vecPosition[0] = shift.x;
				m_Pose.vecPosition[1] = shift.y;
				m_Pose.vecPosition[2] = shift.z;
			}

            // Set rotational coordinates
            {
                const PSMQuatf &orientation = view.Pose.Orientation;

                m_Pose.qRotation.w = orientation.w;
                m_Pose.qRotation.x = orientation.x;
                m_Pose.qRotation.y = orientation.y;
                m_Pose.qRotation.z = orientation.z;
            }

            // Set the physics state of the controller
            {
                const PSMPhysicsData &physicsData= view.PhysicsData;

                m_Pose.vecVelocity[0] = physicsData.LinearVelocityCmPerSec.x * k_fScalePSMoveAPIToMeters;
                m_Pose.vecVelocity[1] = physicsData.LinearVelocityCmPerSec.y * k_fScalePSMoveAPIToMeters;
                m_Pose.vecVelocity[2] = physicsData.LinearVelocityCmPerSec.z * k_fScalePSMoveAPIToMeters;

                m_Pose.vecAcceleration[0] = physicsData.LinearAccelerationCmPerSecSqr.x * k_fScalePSMoveAPIToMeters;
                m_Pose.vecAcceleration[1] = physicsData.LinearAccelerationCmPerSecSqr.y * k_fScalePSMoveAPIToMeters;
                m_Pose.vecAcceleration[2] = physicsData.LinearAccelerationCmPerSecSqr.z * k_fScalePSMoveAPIToMeters;

                m_Pose.vecAngularVelocity[0] = physicsData.AngularVelocityRadPerSec.x;
                m_Pose.vecAngularVelocity[1] = physicsData.AngularVelocityRadPerSec.y;
                m_Pose.vecAngularVelocity[2] = physicsData.AngularVelocityRadPerSec.z;

                m_Pose.vecAngularAcceleration[0] = physicsData.AngularAccelerationRadPerSecSqr.x;
                m_Pose.vecAngularAcceleration[1] = physicsData.AngularAccelerationRadPerSecSqr.y;
                m_Pose.vecAngularAcceleration[2] = physicsData.AngularAccelerationRadPerSecSqr.z;
            }

            m_Pose.poseIsValid = 
				m_PSMControllerView->ControllerState.PSMoveState.bIsPositionValid && 
				m_PSMControllerView->ControllerState.PSMoveState.bIsOrientationValid;

            // This call posts this pose to shared memory, where all clients will have access to it the next
            // moment they want to predict a pose.
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated( m_unSteamVRTrackedDeviceId, m_Pose, sizeof( vr::DriverPose_t ) );
        } break;
    case PSMControllerType::PSMController_DualShock4:
        {
            const PSMDualShock4 &view = m_PSMControllerView->ControllerState.PSDS4State;

            // No prediction since that's already handled in the psmove service
            m_Pose.poseTimeOffset = 0.f;

            // Rotate -90 degrees about the x-axis from the current HMD orientation
            m_Pose.qDriverFromHeadRotation.w = 1.f;
            m_Pose.qDriverFromHeadRotation.x = 0.0f;
            m_Pose.qDriverFromHeadRotation.y = 0.0f;
            m_Pose.qDriverFromHeadRotation.z = 0.0f;
            m_Pose.vecDriverFromHeadTranslation[0] = 0.f;
            m_Pose.vecDriverFromHeadTranslation[1] = 0.f;
            m_Pose.vecDriverFromHeadTranslation[2] = 0.f;

            // Set position
            {
                const PSMVector3f &position = view.Pose.Position;

                m_Pose.vecPosition[0] = position.x * k_fScalePSMoveAPIToMeters;
                m_Pose.vecPosition[1] = position.y * k_fScalePSMoveAPIToMeters;
                m_Pose.vecPosition[2] = position.z * k_fScalePSMoveAPIToMeters;
            }

            // Set rotational coordinates
            {
                const PSMQuatf &orientation = view.Pose.Orientation;

                m_Pose.qRotation.w = orientation.w;
                m_Pose.qRotation.x = orientation.x;
                m_Pose.qRotation.y = orientation.y;
                m_Pose.qRotation.z = orientation.z;
            }

            // Set the physics state of the controller
            // TODO: Physics data is too noisy for the DS4 right now, causes jitter
            {
                const PSMPhysicsData &physicsData= view.PhysicsData;

                m_Pose.vecVelocity[0] = 0.f; // physicsData.Velocity.i * k_fScalePSMoveAPIToMeters;
                m_Pose.vecVelocity[1] = 0.f; // physicsData.Velocity.j * k_fScalePSMoveAPIToMeters;
                m_Pose.vecVelocity[2] = 0.f; // physicsData.Velocity.k * k_fScalePSMoveAPIToMeters;

                m_Pose.vecAcceleration[0] = 0.f; // physicsData.Acceleration.i * k_fScalePSMoveAPIToMeters;
                m_Pose.vecAcceleration[1] = 0.f; // physicsData.Acceleration.j * k_fScalePSMoveAPIToMeters;
                m_Pose.vecAcceleration[2] = 0.f; // physicsData.Acceleration.k * k_fScalePSMoveAPIToMeters;

                m_Pose.vecAngularVelocity[0] = 0.f; // physicsData.AngularVelocity.i;
                m_Pose.vecAngularVelocity[1] = 0.f; // physicsData.AngularVelocity.j;
                m_Pose.vecAngularVelocity[2] = 0.f; // physicsData.AngularVelocity.k;

                m_Pose.vecAngularAcceleration[0] = 0.f; // physicsData.AngularAcceleration.i;
                m_Pose.vecAngularAcceleration[1] = 0.f; // physicsData.AngularAcceleration.j;
                m_Pose.vecAngularAcceleration[2] = 0.f; // physicsData.AngularAcceleration.k;
            }

            m_Pose.poseIsValid =
				m_PSMControllerView->ControllerState.PSDS4State.bIsPositionValid && 
				m_PSMControllerView->ControllerState.PSDS4State.bIsOrientationValid;

            // This call posts this pose to shared memory, where all clients will have access to it the next
            // moment they want to predict a pose.
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated( m_unSteamVRTrackedDeviceId, m_Pose, sizeof( vr::DriverPose_t ) );
        } break;
    }
}

void CPSMoveControllerLatest::UpdateRumbleState()
{
	if (!m_bRumbleSuppressed)
	{
		const float k_max_rumble_update_rate = 33.f; // Don't bother trying to update the rumble faster than 30fps (33ms)
		const float k_max_pulse_microseconds = 1000.f; // Docs suggest max pulse duration of 5ms, but we'll call 1ms max

		std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();
		bool bTimoutElapsed = true;

		if (m_lastTimeRumbleSentValid)
		{
			std::chrono::duration<double, std::milli> timeSinceLastSend = now - m_lastTimeRumbleSent;

			bTimoutElapsed = timeSinceLastSend.count() >= k_max_rumble_update_rate;
		}

		// See if a rumble request hasn't come too recently
		if (bTimoutElapsed)
		{
			float rumble_fraction = static_cast<float>(m_pendingHapticPulseDuration) / k_max_pulse_microseconds;

			// Unless a zero rumble intensity was explicitly set, 
			// don't rumble less than 35% (no enough to feel)
			if (m_pendingHapticPulseDuration != 0)
			{
				if (rumble_fraction < 0.35f)
				{
					// rumble values less 35% isn't noticeable
					rumble_fraction = 0.35f;
				}
			}

			// Keep the pulse intensity within reasonable bounds
			if (rumble_fraction > 1.f)
			{
				rumble_fraction = 1.f;
			}

			// Actually send the rumble to the server
			PSM_SetControllerRumble(m_PSMControllerView->ControllerID, PSMControllerRumbleChannel_All, rumble_fraction);

			// Remember the last rumble we went and when we sent it
			m_lastTimeRumbleSent = now;
			m_lastTimeRumbleSentValid = true;

			// Reset the pending haptic pulse duration.
			// If another call to TriggerHapticPulse() is made later, it will stomp this value.
			// If no call to TriggerHapticPulse() is made later, then the next call to UpdateRumbleState()
			// in k_max_rumble_update_rate milliseconds will set the rumble_fraction to 0.f
			// This effectively makes the shortest rumble pulse k_max_rumble_update_rate milliseconds.
			m_pendingHapticPulseDuration = 0;
		}
	}
	else
	{
		// Reset the pending haptic pulse duration since rumble is suppressed.
		m_pendingHapticPulseDuration = 0;
	}
}

void CPSMoveControllerLatest::UpdateBatteryChargeState(
    PSMBatteryState newBatteryEnum)
{
	bool bIsBatteryCharging= false;
	float fBatteryChargeFraction= 0.f;

	switch (newBatteryEnum)
	{
	case PSMBattery_0:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 0.f;
		break;
	case PSMBattery_20:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 0.2f;
		break;
	case PSMBattery_40:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 0.4f;
		break;
	case PSMBattery_60:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 0.6f;
		break;
	case PSMBattery_80:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 0.8f;
		break;
	case PSMBattery_100:
		bIsBatteryCharging= false;
		fBatteryChargeFraction= 1.f;
		break;
	case PSMBattery_Charging:
		bIsBatteryCharging= true;
		fBatteryChargeFraction= 0.99f; // Don't really know the charge amount in this case
		break;
	case PSMBattery_Charged:
		bIsBatteryCharging= true;
		fBatteryChargeFraction= 1.f;
		break;
	}

	if (bIsBatteryCharging != m_bIsBatteryCharging)
	{
		m_bIsBatteryCharging= bIsBatteryCharging;
		vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, vr::Prop_DeviceIsCharging_Bool, m_bIsBatteryCharging);
	}

	if (fBatteryChargeFraction != m_fBatteryChargeFraction)
	{
		m_fBatteryChargeFraction= fBatteryChargeFraction;
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, vr::Prop_DeviceBatteryPercentage_Float, m_fBatteryChargeFraction);
	}
}

void CPSMoveControllerLatest::Update()
{
    CPSMoveTrackedDeviceLatest::Update();

    if (IsActivated() && m_PSMControllerView->IsConnected)
    {
        int seq_num= m_PSMControllerView->OutputSequenceNum;

        // Only other updating incoming state if it actually changed
        if (m_nPoseSequenceNumber != seq_num)
        {
            m_nPoseSequenceNumber = seq_num;

            UpdateTrackingState();
            UpdateControllerState();
        }

        // Update the outgoing state
        UpdateRumbleState();
    }
}

void CPSMoveControllerLatest::RefreshWorldFromDriverPose()
{
	CPSMoveTrackedDeviceLatest::RefreshWorldFromDriverPose();

	// Mark the calibration process as done
	// once we have setup the world from driver pose
	m_trackingStatus = vr::TrackingResult_Running_OK;
}

bool CPSMoveControllerLatest::AttachChildPSMController(
	int ChildControllerId,
	PSMControllerType ChildControllerType,
	const std::string &ChildControllerSerialNo)
{
	bool bSuccess= false;

	if (m_nPSMChildControllerId == -1 && 
		m_nPSMChildControllerId != m_nPSMControllerId && 
		PSM_AllocateControllerListener(ChildControllerId) == PSMResult_Success)
	{
		m_nPSMChildControllerId= ChildControllerId;
		m_PSMChildControllerType= ChildControllerType;
		m_PSMChildControllerView= PSM_GetController(m_nPSMChildControllerId);

		PSMRequestID request_id;
		if (PSM_StartControllerDataStreamAsync(ChildControllerId, PSMStreamFlags_defaultStreamOptions, &request_id)) 
		{
			PSM_RegisterCallback(request_id, CPSMoveControllerLatest::start_controller_response_callback, this);
			bSuccess= true;
		}
		else
		{
			PSM_FreeControllerListener(ChildControllerId);
		}
	}

	return bSuccess;
}

//==================================================================================================
// Tracker Driver
//==================================================================================================

CPSMoveTrackerLatest::CPSMoveTrackerLatest(const PSMClientTrackerInfo *trackerInfo)
    : CPSMoveTrackedDeviceLatest()
    , m_nTrackerId(trackerInfo->tracker_id)
{
    char buf[256];
    GenerateTrackerSerialNumber(buf, sizeof(buf), trackerInfo->tracker_id);
    m_strSteamVRSerialNo = buf;

    SetClientTrackerInfo(trackerInfo);
}

CPSMoveTrackerLatest::~CPSMoveTrackerLatest()
{
}

vr::EVRInitError CPSMoveTrackerLatest::Activate(uint32_t unObjectId)
{
    vr::EVRInitError result = CPSMoveTrackedDeviceLatest::Activate(unObjectId);

    if (result == vr::VRInitError_None)
    {
		vr::CVRPropertyHelpers *properties= vr::VRProperties();

		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_FieldOfViewLeftDegrees_Float, m_tracker_info.tracker_hfov / 2.f);
		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_FieldOfViewRightDegrees_Float, m_tracker_info.tracker_hfov / 2.f);
		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_FieldOfViewTopDegrees_Float, m_tracker_info.tracker_vfov / 2.f);
		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_FieldOfViewBottomDegrees_Float, m_tracker_info.tracker_vfov / 2.f);
		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_TrackingRangeMinimumMeters_Float, m_tracker_info.tracker_znear * k_fScalePSMoveAPIToMeters);
		properties->SetFloatProperty(m_ulPropertyContainer, vr::Prop_TrackingRangeMaximumMeters_Float, m_tracker_info.tracker_zfar * k_fScalePSMoveAPIToMeters);

		properties->SetInt32Property(m_ulPropertyContainer, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_TrackingReference);

		// The {psmove} syntax lets us refer to rendermodels that are installed
		// in the driver's own resources/rendermodels directory.  The driver can
		// still refer to SteamVR models like "generic_hmd".
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, "{psmove}ps3eye_tracker");

		char model_label[16]= "\0";
		snprintf(model_label, sizeof(model_label), "ps3eye_%d", m_tracker_info.tracker_id);
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_ModeLabel_String, model_label);

		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceOff_String, "{psmove}base_status_off.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearching_String, "{psmove}base_status_ready.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{psmove}base_status_ready_alert.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReady_String, "{psmove}base_status_ready.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{psmove}base_status_ready_alert.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceNotReady_String, "{psmove}base_status_error.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceStandby_String, "{psmove}base_status_standby.png");
		properties->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceAlertLow_String, "{psmove}base_status_ready_low.png");

        // Poll the latest WorldFromDriverPose transform we got from the service
        // Transform used to convert from PSMove Tracking space to OpenVR Tracking Space
        RefreshWorldFromDriverPose();
    }

    return result;
}

void CPSMoveTrackerLatest::Deactivate()
{
}

void CPSMoveTrackerLatest::SetClientTrackerInfo(
    const PSMClientTrackerInfo *trackerInfo)
{
    m_tracker_info = *trackerInfo;

    //### HipsterSloth $TODO expose on the pose state if calibration is currently active
    //m_Pose.result = vr::TrackingResult_Calibrating_InProgress;
    m_Pose.result = vr::TrackingResult_Running_OK;

    m_Pose.deviceIsConnected = true;

    // Yaw can't drift because the tracker never moves (hopefully)
    m_Pose.willDriftInYaw = false;
    m_Pose.shouldApplyHeadModel = false;

    // No prediction since that's already handled in the psmove service
    m_Pose.poseTimeOffset = 0.f;

    // Poll the latest WorldFromDriverPose transform we got from the service
    // Transform used to convert from PSMove Tracking space to OpenVR Tracking Space
    RefreshWorldFromDriverPose();

    // No transform due to the current HMD orientation
    m_Pose.qDriverFromHeadRotation.w = 1.f;
    m_Pose.qDriverFromHeadRotation.x = 0.0f;
    m_Pose.qDriverFromHeadRotation.y = 0.0f;
    m_Pose.qDriverFromHeadRotation.z = 0.0f;
    m_Pose.vecDriverFromHeadTranslation[0] = 0.f;
    m_Pose.vecDriverFromHeadTranslation[1] = 0.f;
    m_Pose.vecDriverFromHeadTranslation[2] = 0.f;

    // Set position
    {
        const PSMVector3f &position = m_tracker_info.tracker_pose.Position;

        m_Pose.vecPosition[0] = position.x * k_fScalePSMoveAPIToMeters;
        m_Pose.vecPosition[1] = position.y * k_fScalePSMoveAPIToMeters;
        m_Pose.vecPosition[2] = position.z * k_fScalePSMoveAPIToMeters;
    }

    // Set rotational coordinates
    {
        const PSMQuatf &orientation = m_tracker_info.tracker_pose.Orientation;

        m_Pose.qRotation.w = orientation.w;
        m_Pose.qRotation.x = orientation.x;
        m_Pose.qRotation.y = orientation.y;
        m_Pose.qRotation.z = orientation.z;
    }

    m_Pose.poseIsValid = true;
}

void CPSMoveTrackerLatest::Update()
{
    CPSMoveTrackedDeviceLatest::Update();

    // This call posts this pose to shared memory, where all clients will have access to it the next
    // moment they want to predict a pose.
	vr::VRServerDriverHost()->TrackedDevicePoseUpdated( m_unSteamVRTrackedDeviceId, m_Pose, sizeof( vr::DriverPose_t ) );
}

bool CPSMoveTrackerLatest::HasTrackerId(int TrackerID)
{
    return TrackerID == m_nTrackerId;
}

//==================================================================================================
// Driver Factory
//==================================================================================================

HMD_DLL_EXPORT
void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode)
{
    if (0 == strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName))
    {
        return &g_ServerTrackedDeviceProvider;
    }
    if (0 == strcmp(vr::IVRWatchdogProvider_Version, pInterfaceName))
    {
        return &g_WatchdogDriverPSMoveService;
    }

    if (pReturnCode)
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;

    return NULL;
}
