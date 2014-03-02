// FSeconomy.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include "FSeconomy.h"

#include <commctrl.h>
#include <string.h>
#include <shellapi.h>
#include <Windows.h>
#include <math.h>

#define MAX_LOADSTRING 100


#ifdef _DEBUG
#define VERSION "1.1.4" 
#define HOST "192.168.1.100"
#define PORT 8080
#else
#define VERSION "1.1.4"
#define HOST "192.168.1.100"
#define PORT 8080
#endif

#define AGENT "/fseconomy/fsagent"
#define WEBSITE "http://192.168.1.100:8080/fseconomy/"

#define EQUIP_IFR	(1<<0)
#define EQUIP_GPS	(1<<1
#define EQUIP_AP	(1<<2)


#define FF_OFFLINE		0
#define FF_GROUND		1
#define FF_DEPARTED		2

#define TYPE_PISTON		0
#define TYPE_JET		1
#define TYPE_TURBOPROP	5

// Global Variables:
HINSTANCE hInst;								// current instance
HWND hwndMain;									// Main window
HWND statusBar;									// Status bar
HWND content;									// Window content
TCHAR szTitle[MAX_LOADSTRING];					// The title bar text
TCHAR szWindowClass[MAX_LOADSTRING];			// the main window class name

char user[80], password[80];					// Account info
int commStatus = 0;								// FSUIPC status

double volatile FSposLat, FSposLon;
long checkinBy;
int flightFase = FF_OFFLINE;
int accounting=1;
int equipment;
double engineTime, engineTicks, nightTime, envFactor, envTime;
double currentWeight;
int payloadWeight, totalWeight;
int arrivedTicks;
SHORT slewMode;
LONGLONG ticks;
char logString[1024];

int ReadFSUIPC(DWORD, DWORD, void *);
/***************************************************************************************/
class engineCheck
{
protected:
	int engines;
public:
	engineCheck(int);
	virtual void getVariables() {};
	virtual void run(int) {};
	virtual char *toString(char *) { return NULL; };
};

engineCheck::engineCheck(int engines)
{
	this->engines = engines;
}
/***************************************************************************************/
class engineCheckCHT : public engineCheck
{
	double oldCht[4], cht[4];
	double result[4];
public:
	virtual void run(int);
	virtual void getVariables();
	virtual char *toString(char *);
	engineCheckCHT(int);
};

engineCheckCHT::engineCheckCHT(int engines):engineCheck(engines)
{
	oldCht[0] = oldCht[1] = oldCht[2] = oldCht[3] = 0.0;
	result[0] = result[1] = result[2] = result[3] = 0.0;
}
void engineCheckCHT::getVariables()
{
	for (int c=0; c < engines; c++)
		ReadFSUIPC(0x08e8 + c * 152, 8, &cht[c]);
}
void engineCheckCHT::run(int seconds)
{
	for (int c=0; c < engines; c++)
	{
		if (oldCht[c])
		{
			double diff = fabs(cht[c] - oldCht[c])/(double)seconds;
			if (diff > 1)
			{
				result[c] += diff;
			}
		}
		oldCht[c] = cht[c];

		
	}
}
char *engineCheckCHT::toString(char *buffer)
{
	for (int c=0; c < engines; c++)
	{
		char var[1024];
		sprintf(var, "%sheat%d=%d", c > 0 ? "&" : "", c + 1, (int) result[c]);
		strcat(buffer, var);
	}
	return buffer;
}
/***************************************************************************************/

class engineCheckMixture : public engineCheck
{
	LONG altitude;
	SHORT mixture[4];
	int result[4];
public:
	virtual void run(int);
	virtual void getVariables();
	virtual char *toString(char *);
	engineCheckMixture(int);
};

engineCheckMixture::engineCheckMixture(int engines):engineCheck(engines)
{
	result[0] = result[1] = result[2] = result[3] = 0;
}
void engineCheckMixture::getVariables()
{
	ReadFSUIPC(0x574, 4, &altitude);
	for (int c=0; c < engines; c++)
		ReadFSUIPC(0x0890 + c * 152, 2, &mixture[c]);
}
void engineCheckMixture::run(int seconds)
{
	for (int c=0; c < engines; c++)
	{
		if (mixture[c] > 16000 && altitude > 1000)
			result[c]+=seconds;
	}
}
char *engineCheckMixture::toString(char *buffer)
{
	for (int c=0; c < engines; c++)
	{
		char var[1024];
		sprintf(var, "%smixture%d=%d", c > 0 ? "&" : "", c + 1, result[c]);
		strcat(buffer, var);
	}
	return buffer;
}
/***************************************************************************************/
class engineMonitor
{
	engineCheck *checks[2];
	int amount;
public:
	engineMonitor(int, int);
	~engineMonitor();
	char *engineMonitor::toString(char *);
	void getVariables();
	void run(int);
};

engineMonitor::engineMonitor(int aircraftType, int engines)
{
	if (aircraftType == TYPE_PISTON)
	{
		this->checks[0] = new engineCheckMixture(engines);
		this->checks[1] = new engineCheckCHT(engines);
		this->amount = 2;
	} else
		this->amount = 0;
}
engineMonitor::~engineMonitor()
{
	for (int c=0; c < amount; c++)
		delete checks[c];
}
void engineMonitor::getVariables()
{
	for (int c=0; c < amount; c++)
		checks[c]->getVariables();
}
void engineMonitor::run(int seconds)
{
	if (seconds > 0)
		for (int c=0; c < amount; c++)
			checks[c]->run(seconds);
}
char *engineMonitor::toString(char *buffer)
{
	for (int c=0; c < amount; c++)
	{
		if (c > 0)
			strcat(buffer, "&");
		checks[c]->toString(buffer);
	}
	return buffer;
}

/***************************************************************************************/
class parameters
{
public:
	parameters();
	~parameters();
	char *find(char *, int, int *);
	char *find(char *);
	void add(char *, char *);
private:
	char **names;
	char **values;
	int entries;
};

parameters *flightParams = NULL;
engineMonitor *monitor = NULL;

// Forward declarations of functions included in this code module:
ATOM				MyRegisterClass(HINSTANCE hInstance);
BOOL				InitInstance(HINSTANCE, int);
LRESULT CALLBACK	WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	About(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	Account(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	Weight(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK	Reporting(HWND, UINT, WPARAM, LPARAM);

BOOL setContentItem(char *cargo, char *amount, char *destination);
BOOL removeAllContent();


parameters::parameters()
{
	names = NULL;
	values = NULL;
	entries = 0;
}
parameters::~parameters()
{
	for (int c=0; c < entries; c++)
	{
		free(names[c]);
		free(values[c]);
	}
	if (names)
		free(names);
	if (values)
		free(values);
}
void parameters::add(char *name, char *value)
{
	entries++;
	names = (char **)realloc(names, (sizeof(char*) * entries));
	values = (char **)realloc(values, (sizeof(char*) * entries));
	names[entries-1] = strdup(name);
	values[entries-1] = strdup(value);
}
char *parameters::find(char *name, int from, int *location)
{
	if (from >= entries)
		return NULL;
	for (int c=from; c < entries; c++)
		if (strcmp(names[c], name) == 0)
		{
			if (location)
				*location = c;
			return values[c];
		}
	return NULL;
}
char *parameters::find(char *name)
{
	return find(name, 0, NULL);
}

void showError(char *message)
{
	LPVOID lpMsgBuf;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
   		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &lpMsgBuf,0,NULL);
	MessageBox( NULL, (LPCSTR)lpMsgBuf, message, MB_OK|MB_ICONINFORMATION );
	LocalFree( lpMsgBuf );
}

int Communicate(char *request, int retryMode, char *parameters, ...)
{
	HINTERNET handle=NULL, hInternet=NULL, connectHandle=NULL;
	char buffer[1000], params[512];
	int returnValue=-1;
	char header[]="Content-Type: application/x-www-form-urlencoded";
	DWORD dwReserved = 0;
	if (flightParams)
		delete flightParams;

	flightParams = new ::parameters();

	if (InternetAttemptConnect(dwReserved) != ERROR_SUCCESS )
	{
		if (!retryMode)
			showError("Error connecting to the internet");
		return -1;
	}
	hInternet = InternetOpen("fsagent", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (hInternet)
		connectHandle = InternetConnect(hInternet, HOST, PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
	if (connectHandle)
		handle = HttpOpenRequest(connectHandle, "POST", AGENT, NULL, NULL, NULL, INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_UI|INTERNET_FLAG_PRAGMA_NOCACHE|INTERNET_FLAG_RELOAD, 0);

	if (!handle)
	{
		SendMessage(statusBar, SB_SETTEXT, 1, (LPARAM) "Service currently unavailable");
		if (connectHandle)
			InternetCloseHandle(connectHandle);
		if (hInternet)
			InternetCloseHandle(hInternet);
		return -1;
	}
	if (parameters)
	{
		va_list ap;
		va_start(ap, parameters);
		_vsnprintf(params, 512, parameters, ap);
		va_end(ap);
	}
	sprintf(buffer,"action=%s&version=%s&user=%s&pass=%s%s%s", request, VERSION, user, password, parameters?"&":"", parameters?params:"");

	if (!HttpSendRequest(handle, header, (DWORD) strlen(header), buffer, (DWORD) strlen(buffer)))
	{
		SendMessage(statusBar, SB_SETTEXT, 1, (LPARAM) "Service currently unavailable");
	} else
	{
		char response[10240];
		DWORD response_len=10240;
		int status;
		HttpQueryInfo(handle, HTTP_QUERY_STATUS_CODE, response, &response_len, NULL);
		response[response_len]='\0';
		sscanf(response, "%d", &status);
		InternetReadFile(handle, response, 10240, &response_len);
		response[response_len]='\0';

		if (status == 403)
		{
			SendMessage(statusBar, SB_SETTEXT, 1, (LPARAM) "Invalid account information");
		} else if (status == 200)
			SendMessage(statusBar, SB_SETTEXT, 1, (LPARAM) "Network connected");
		else SendMessage(statusBar, SB_SETTEXT, 1, (LPARAM) "Network error");

		char *pointer;
		int responseCount = 0;
		for (pointer = strtok(response, "\r\n"); pointer; pointer=strtok(NULL, "\r\n"))
		{
			char *where = strchr(pointer, '=');
			if (!where)
				continue;
			*(where++)='\0';
			if (strcmp(pointer, "mess") == 0)
			{
				char *start = where;
				while (where=strchr(where, '|'))
					*where='\n';
				if (retryMode)
					flightParams->add("mess", start);
				else
					MessageBox(NULL, start, "FS Economy", MB_OK|MB_ICONINFORMATION|MB_SETFOREGROUND);
			} else
			{
				flightParams->add(pointer, where);
			}
		}
		returnValue = status;
	}
	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
	InternetCloseHandle(handle);
	InternetCloseHandle(connectHandle);
	InternetCloseHandle(hInternet);
	return returnValue;
}
int testAccount()
{
	return Communicate("test", 0, NULL)==200 ;
}

void enableMenu()
{
	commStatus = 1;
	HMENU menu = GetSubMenu(GetMenu(hwndMain), 1);
	EnableMenuItem(GetMenu(hwndMain), 1, MF_BYPOSITION|MF_ENABLED);
	EnableMenuItem(menu, 0, MF_BYPOSITION|MF_ENABLED);
	EnableMenuItem(menu, 1, MF_BYPOSITION|MF_ENABLED);
	EnableMenuItem(menu, 2, MF_BYPOSITION|MF_GRAYED);
	DrawMenuBar(hwndMain);
	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
}
void disableMenu()
{
	HMENU menu = GetSubMenu(GetMenu(hwndMain), 1);
	EnableMenuItem(GetMenu(hwndMain), 1, MF_BYPOSITION|MF_GRAYED);
	EnableMenuItem(menu, 0, MF_BYPOSITION|MF_GRAYED);
	EnableMenuItem(menu, 1, MF_BYPOSITION|MF_GRAYED);
	EnableMenuItem(menu, 2, MF_BYPOSITION|MF_GRAYED);
	DrawMenuBar(hwndMain);
	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
}
void disableStart()
{
	HMENU menu = GetSubMenu(GetMenu(hwndMain), 1);
	EnableMenuItem(menu, 1, MF_BYPOSITION|MF_GRAYED);
	EnableMenuItem(menu, 2, MF_BYPOSITION|MF_ENABLED);
	DrawMenuBar(hwndMain);
	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
}
void enableStart()
{
	HMENU menu = GetSubMenu(GetMenu(hwndMain), 1);
	EnableMenuItem(menu, 1, MF_BYPOSITION|MF_ENABLED);
	EnableMenuItem(menu, 2, MF_BYPOSITION|MF_GRAYED);
	DrawMenuBar(hwndMain);
	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
}

void fsDown()
{
	disableMenu();
	commStatus = 0;
	FSUIPC_Close();
}

int ReadFSUIPC(DWORD dwOffset, DWORD dwSize, void *buffer)
{
	DWORD dwResult;
	if (commStatus == 0)
		return 0;
	if (FSUIPC_Read(dwOffset, dwSize, buffer, &dwResult) == TRUE)
		return 1;
	fsDown();
	return 0;
}
int WriteFSUIPC(DWORD dwOffset, DWORD dwSize, void *buffer)
{
	DWORD dwResult;
	if (commStatus == 0)
		return 0;
	if (FSUIPC_Write(dwOffset, dwSize, buffer, &dwResult) == TRUE)
		return 1;
	fsDown();
	return 0;
}
int process()
{
	DWORD dwResult;
	if (FSUIPC_Process(&dwResult) == TRUE)
		return 1;
	fsDown();
	return 0;
}
char *currentAircraft()
{
	static char result[256];
	result[0]='\0';
	if (!ReadFSUIPC(0x3d00, 256, result))
		return NULL;
	if (!process())
		return NULL;
	return result;
}

int initFSUIPC()
{
	DWORD dwResult;
	if (FSUIPC_Open(SIM_ANY, &dwResult))
	{
		static char chOurKey[] = "PK4KP41I5SLF";
		if (FSUIPC_Write(0x8001, 12, chOurKey, &dwResult))
			FSUIPC_Process(&dwResult); // Process the request(s)

		SendMessage(statusBar, SB_SETTEXT, 0, (LPARAM) "FS Linked");
		return 1;
	}
	SendMessage(statusBar, SB_SETTEXT, 0, (LPARAM) "FS not Linked");
	return 0;
}
void error()
{
	PVOID lpMsgBuf;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
   		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &lpMsgBuf,0,NULL);
	MessageBox( NULL, (LPCSTR) lpMsgBuf, "GetLastError", MB_OK|MB_ICONINFORMATION );
	LocalFree( lpMsgBuf );
}

void testAircraft(HWND hwnd)
{
	char message[]="Please load your aircraft in flightsimulator first.\nPress OK to check if the aircraft is known.";
	if (MessageBox(NULL, message, "Test your aircraft", MB_OKCANCEL|MB_ICONINFORMATION )!= IDOK)
		return;
	char *id = currentAircraft();
	if (!id)
		return;
	DWORD center, leftMain, leftAux, rightMain, rightAux, leftTip, rightTip;
	DWORD center2, center3, ext1, ext2;

	if (!ReadFSUIPC(0xb78, 4, &center)
		||!ReadFSUIPC(0xb80, 4, &leftMain)
		||!ReadFSUIPC(0xb88, 4, &leftAux)
		||!ReadFSUIPC(0xb90, 4, &leftTip)
		||!ReadFSUIPC(0xb98, 4, &rightMain)
		||!ReadFSUIPC(0xba0, 4, &rightAux)
		||!ReadFSUIPC(0xba8, 4, &rightTip)
		||!ReadFSUIPC(0x1248, 4, &center2)
		||!ReadFSUIPC(0x1250, 4, &center3)
		||!ReadFSUIPC(0x1258, 4, &ext1)
		||!ReadFSUIPC(0x1260, 4, &ext2)
		)
    		return;
	if (!process())
			return;

	Communicate("addAircraft", 0, "aircraft=%s&c=%d&lm=%d&la=%d&lt=%d&rm=%d&ra=%d&rt=%d&c2=%d&c3=%d&x1=%d&x2=%d", id,
		center, leftMain, leftAux, leftTip, rightMain, rightAux, rightTip, center2, center3, ext1, ext2);
}

void setAccount()
{
	char buffer[1025], *pointer;
	GetFullPathName("fseco.ini", 1024, buffer, &pointer);
	if (!WritePrivateProfileString("main", "user", user, buffer))
		error();
	WritePrivateProfileString("main", "password", password, buffer);
	if (!testAccount())
	{
		MessageBox(NULL, "Invalid username/password", "FSEconomy", MB_OK|MB_ICONINFORMATION);
	} else
		MessageBox(NULL, "Account accepted", "FSEconomy", MB_OK|MB_ICONINFORMATION);
}

void fillAccount()
{
	char buffer[1025], *pointer;
	GetFullPathName("fseco.ini", 1024, buffer, &pointer);

	GetPrivateProfileString("main", "user", "", user, 80, buffer);
	GetPrivateProfileString("main", "password", "", password, 80, buffer);
	if (user[0])
	{
		testAccount();
	} else SendMessage(statusBar, SB_SETTEXT, 0, (LPARAM) "No network account");
}

void doCancel(char *reason)
{
	char failed = 0;
	WriteFSUIPC(0x3bd6, 1, &failed);	// Fail ADF
	WriteFSUIPC(0x3be1, 1, &failed);	// Nav 1
	WriteFSUIPC(0x3be2, 1, &failed);	// Nav 2

	process();
	flightFase = FF_OFFLINE;
	removeAllContent();
	enableStart();
	Communicate("cancel", 0, "");
	char message[1024]="Your flight was cancelled.";
	if (reason)
	{
		strcat(message, "\nReason: ");
		strcat(message, reason);
	}
	MessageBox(NULL, message, "FS Economy", MB_OK|MB_ICONINFORMATION|MB_SETFOREGROUND);
}
void doArrive(HWND hWnd)
{
	DWORD center, leftMain, leftAux, rightMain, rightAux, leftTip, rightTip;
	DWORD center2, center3, ext1, ext2;
	double dcenter, dleftMain, dleftAux, drightMain, drightAux, dleftTip, drightTip;
	double dcenter2, dcenter3, dext1, dext2;
	flightFase = FF_OFFLINE;
	
	ReadFSUIPC(0xb74, 4, &center);
	ReadFSUIPC(0xb7c, 4, &leftMain);
	ReadFSUIPC(0xb84, 4, &leftAux);
	ReadFSUIPC(0xb8c, 4, &leftTip);
	ReadFSUIPC(0xb94, 4, &rightMain);
	ReadFSUIPC(0xb9c, 4, &rightAux);
	ReadFSUIPC(0xba4, 4, &rightTip);
	ReadFSUIPC(0x1244, 4, &center2);
	ReadFSUIPC(0x124c, 4, &center3);
	ReadFSUIPC(0x1254, 4, &ext1);
	ReadFSUIPC(0x125c, 4, &ext2);

	char failed = 0;
	WriteFSUIPC(0x3bd6, 1, &failed);	// Fail ADF
	WriteFSUIPC(0x3be1, 1, &failed);	// Nav 1
	WriteFSUIPC(0x3be2, 1, &failed);	// Nav 2

	process();

	double factor = 128 * 65536.0;
	dcenter = center/factor;
	dleftMain = leftMain/factor;
	dleftAux = leftAux/factor;
	dleftTip = leftTip/factor;
	drightMain = rightMain/factor;
	drightAux = rightAux/factor;
	drightTip = rightTip/factor;
	dcenter2 = center2/factor;
	dcenter3 = center3/factor;
	dext1 = ext1/factor;
	dext2 = ext2/factor;

	removeAllContent();
	int night = 0;
	if (engineTime > 0 && ((nightTime/engineTime) > 0.5))
		night = 1;
	if (envTime > 0)
		envFactor/=(double)envTime;
	else
		envFactor = 1;
	if (envFactor < 1)
		envFactor = 1;
	if (envFactor > 2.5)
		envFactor = 2.5;

	char buffer[1024]="&";
	monitor->toString(buffer);
	if (!buffer[1])
		buffer[0]='\0';

	sprintf(logString, "lat=%f&lon=%f&time=%d&ticks=%d&c=%f&lm=%f&la=%f&let=%f&rm=%f&ra=%f&rt=%f&night=%d&env=%f&c2=%f&c3=%f&x1=%f&x2=%f%s", 
		FSposLat, FSposLon, (int)engineTime, (int)engineTicks, dcenter, dleftMain, dleftAux, dleftTip, drightMain, drightAux, drightTip,
		night, envFactor, dcenter2, dcenter3, dext1, dext2, buffer);

	DialogBox(hInst, (LPCTSTR)IDD_REPORTING, hWnd, (DLGPROC)Reporting);

	enableStart();	
	delete monitor;
	monitor = NULL;
}

void doCancelFlight(HWND hWnd)
{
	if (MessageBox(NULL, "This will cancel your current flight.\nPress Ok to cancel your flight or cancel to continue.", "Cancel flight", MB_OKCANCEL|MB_ICONINFORMATION )!= IDOK)
		return;
	doCancel(NULL);
}
int doStart(HWND hWnd)
{
	char *id = currentAircraft();
	char *nowFlying;
	char *expiry;
	char *sAccounting;
	char *sFuel;
	char *sEquipment;
	char *sPayloadWeight;
	char *sTotalWeight;
	unsigned char aircraftType;

	if (!id)
		return 0;
	if (Communicate("start", 0, "aircraft=%s&lat=%f&lon=%f", id, FSposLat, FSposLon)!=200)
		return 0;

	nowFlying = flightParams->find("reg");
	expiry = flightParams->find("expiry");
	sAccounting = flightParams->find("account");
	sFuel = flightParams->find("fuel");
	sEquipment = flightParams->find("equip");
	sPayloadWeight = flightParams->find("payload");
	sTotalWeight = flightParams->find("weight");

	if (!nowFlying || !expiry || !sAccounting || !sFuel || !sEquipment || !sPayloadWeight || !sTotalWeight)
		return 0;

	sscanf(expiry, "%ld", &checkinBy);
	sscanf(sAccounting, "%d", &accounting);
	sscanf(sEquipment, "%d", &equipment);
	sscanf(sPayloadWeight, "%d", &payloadWeight);
	sscanf(sTotalWeight, "%d", &totalWeight);

	DWORD fuel[11];
	double factor = 128 * 65536.0;
	int count = 0;
	SHORT engines;
	char *amount = strtok(sFuel, ":");
	while (amount && count < 11)
	{
		float value;
		sscanf(amount, "%f", &value);
		fuel[count]=(DWORD)(factor*value);
		amount = strtok(NULL, ":");
		count++;
	}
	WriteFSUIPC(0xb74, 4, &fuel[0]);
	WriteFSUIPC(0xb7c, 4, &fuel[1]);
	WriteFSUIPC(0xb84, 4, &fuel[2]);
	WriteFSUIPC(0xb8c, 4, &fuel[3]);
	WriteFSUIPC(0xb94, 4, &fuel[4]);
	WriteFSUIPC(0xb9c, 4, &fuel[5]);
	WriteFSUIPC(0xba4, 4, &fuel[6]);
	WriteFSUIPC(0x1244, 4, &fuel[7]);
	WriteFSUIPC(0x124c, 4, &fuel[8]);
	WriteFSUIPC(0x1254, 4, &fuel[9]);
	WriteFSUIPC(0x125c, 4, &fuel[10]);
	ReadFSUIPC(0x0609, 1, &aircraftType);
	ReadFSUIPC(0x0aec, 2, &engines);

	int find = 0;
	char *assignment;
	while ((assignment = flightParams->find("as", find, &find)) != NULL)
	{
		char *cargo = strtok(assignment, ":");
		char *amount = strtok(NULL, ":");
		char *destination = strtok(NULL, ":");
		setContentItem(cargo, amount, destination);
		find++;
	}
	process();
	flightFase = FF_GROUND;
	engineTime = engineTicks = nightTime = envFactor = envTime = 0;
	disableStart();
	SendMessage(statusBar, SB_SETTEXT, 2, (LPARAM) nowFlying);
	DialogBox(hInst, (LPCTSTR)IDD_WEIGHT, hWnd, (DLGPROC)Weight);
	monitor = new engineMonitor(aircraftType, engines);
	return 1;
}

int diffSeconds(char minutes, char seconds)
{
	static int prevTime = -1;
	int diff = 0, now = (int) minutes * 60 + seconds;
	if (prevTime != -1)
	{
		diff = now - prevTime;
		if (diff < 0)
			diff = now +  3600 - prevTime;
	}
	prevTime = now;
	return diff;
}

void CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if (commStatus == 0)
	{
		if (initFSUIPC() == 0)
			return;
		enableMenu();
	}

	LONGLONG lat, lon, newTicks;
	SHORT airborne;
	double rpm;
	char hour;
	SHORT season, visibility, winds;
	SHORT cloudBase, cloudCoverage;
	SHORT crashed;
	SHORT engineFiring;
	LONG radioAlt, groundAlt, groundSpeed;
	char zMinute, zSecond;
	int secondsThisRun;

	if (!ReadFSUIPC(0x560, 8, &lat)||!ReadFSUIPC(0x568, 8, &lon)) return;
	if (!ReadFSUIPC(0x366, 2, &airborne)) return;
	if (!ReadFSUIPC(0x894, 2, &engineFiring)) return;
	if (!ReadFSUIPC(0x2408, 8, &rpm)) return;
	if (!ReadFSUIPC(0x310, 8, &newTicks)) return;
	if (!ReadFSUIPC(0x238, 1, &hour)) return;
	if (!ReadFSUIPC(0x23c, 1, &zMinute)) return;
	if (!ReadFSUIPC(0x23a, 1, &zSecond)) return;
	if (!ReadFSUIPC(0x248, 2, &season)) return;
	if (!ReadFSUIPC(0xe8a, 2, &visibility)) return;
	if (!ReadFSUIPC(0xe90, 2, &winds)) return;
	if (!ReadFSUIPC(0x31e4, 4, &radioAlt)) return;
	if (!ReadFSUIPC(0x0ea4, 2, &cloudBase)) return;
	if (!ReadFSUIPC(0x0ea6, 2, &cloudCoverage)) return;
	if (!ReadFSUIPC(0x0020, 4, &groundAlt)) return;
	if (!ReadFSUIPC(0x30c0, 8, &currentWeight)) return;
	if (!ReadFSUIPC(0x2b4, 4, &groundSpeed)) return;
	if (!ReadFSUIPC(0x840, 2, &crashed)) return;
	if (!ReadFSUIPC(0x05dc, 2, &slewMode)) return;

	if (monitor)
		monitor->getVariables();

	if (flightFase != FF_OFFLINE)
	{
		if ((equipment & EQUIP_IFR) == 0)
		{
			char failed = 1;
			WriteFSUIPC(0x3bd6, 1, &failed);	// Fail ADF
			WriteFSUIPC(0x3be1, 1, &failed);	// Nav 1
			WriteFSUIPC(0x3be2, 1, &failed);	// Nav 2
		}

		if ((equipment & EQUIP_AP) == 0)
		{
			LONG failed = 0;
			WriteFSUIPC(0x7bc, 4, &failed);	// Fail AP
		}
	}

	if (!process())
		return;
	if (slewMode > 0)
	{
		SHORT off = 0;
		WriteFSUIPC(0x05DC, 2, &off);			// Turn off slew
		if (!process())
			return;
	}

	if (flightFase != FF_OFFLINE && crashed == 1)
	{
		doCancel("Aircraft crashed.");
		return;
	}

	secondsThisRun = diffSeconds(zMinute, zSecond);
	if (rpm < 0)
		rpm *= -1;

	groundSpeed >>= 16;

	double newLat = ((double) lat)*(90.0/(10001750.0 * 65536.0 * 65536.0));
	double newLon = ((double) lon)*(360.0/(65536.0 * 65536.0 * 65536.0 * 65536.0));

	if (monitor)
		monitor->run(secondsThisRun);
	if (flightFase != FF_OFFLINE && newTicks != ticks)
	{
		char buffer[50];
		int togo = (checkinBy - time(NULL))/60;
		sprintf(buffer, "Return by %02d:%02d", togo/60, togo%60);
		SendMessage(statusBar, SB_SETTEXT, 3, (LPARAM) buffer);
		boolean arrived = flightFase == FF_DEPARTED && engineFiring == 0 && airborne == 1 && groundSpeed < 5;
		
		if (arrived)
		{
			if (arrivedTicks++ > 5)
				doArrive(hwnd);
			return;
		} else
			arrivedTicks = 0;

		if (engineFiring > 0)
		{
			double factor = 1;
			if (accounting == 1)
				engineTicks+=secondsThisRun * rpm/36.0;
			engineTime+=secondsThisRun;

			int weather = 0;
			radioAlt/=65536;
			groundAlt/=256;

			if (visibility <= 500)
				weather++;
			if (visibility <= 300)
				weather++;
			if (radioAlt <= 1000)
			{
				factor = 5;
				weather += winds/5;
			} else if (radioAlt < 1500)
				factor = 3;
			double coverage = cloudCoverage/8192.0;
			if (coverage > 2)
				weather++;
			if (coverage > 4)
				weather++;
			if (coverage > 6)
				weather++;
			if (cloudBase - groundAlt < 1000)
				weather++;

			envTime += factor * secondsThisRun;
			envFactor += factor * secondsThisRun * weather/2.0;

//sprintf(buffer, "w: %d, %d %%", weather, (int)(envFactor*100/(double)engineTime) );
//SendMessage(statusBar, SB_SETTEXT, 3, (LPARAM) buffer);

			switch (season)
			{
			case 0:	// Winter
				if (hour < 8 || hour > 19)
					nightTime+=secondsThisRun;
				break;
			case 1:
			case 3: // Fall, Spring
				if (hour < 8 || hour > 20)
					nightTime+=secondsThisRun;
				break;
			case 2:	// Summer
				if (hour < 8 || hour > 21)
					nightTime+=secondsThisRun;
				break;
			}
		} 
//sprintf(buffer, "TI:%.2f TK:%.2f", engineTime, engineTicks*36.0);
//SendMessage(statusBar, SB_SETTEXT, 3, (LPARAM) buffer);
		if (flightFase == FF_GROUND && airborne == 0)
			flightFase = FF_DEPARTED;
	}

	ticks = newTicks;
	if (flightFase != FF_OFFLINE && (abs(FSposLat - newLat) > 1 || (((int)abs(FSposLon) != 179 )&&(abs(FSposLon - newLon) * cos(newLat*3.1415/180.0)) > 1)))
	{
		doCancel("Jump detected.");
	}
	FSposLat = newLat;
	FSposLon = newLon;
//	RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
}


int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR    lpCmdLine,
                     int       nCmdShow)
{
 	// TODO: Place code here.
	MSG msg;
	HACCEL hAccelTable;

	// Initialize global strings
	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDC_FSECONOMY, szWindowClass, MAX_LOADSTRING);
	MyRegisterClass(hInstance);

	// Perform application initialization:
	if (!InitInstance (hInstance, nCmdShow)) 
	{
		return FALSE;
	}

	hAccelTable = LoadAccelerators(hInstance, (LPCTSTR)IDC_FSECONOMY);

	// Main message loop:
	while (GetMessage(&msg, NULL, 0, 0)) 
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) 
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
//  COMMENTS:
//
//    This function and its usage are only necessary if you want this code
//    to be compatible with Win32 systems prior to the 'RegisterClassEx'
//    function that was added to Windows 95. It is important to call this function
//    so that the application will get 'well formed' small icons associated
//    with it.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX); 

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= (WNDPROC)WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, (LPCTSTR)IDI_FSECONOMY);
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= (LPCTSTR)IDC_FSECONOMY;
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= LoadIcon(wcex.hInstance, (LPCTSTR)IDI_SMALL);

	return RegisterClassEx(&wcex);
}

//
//   FUNCTION: InitInstance(HANDLE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//


BOOL setContentItem(char *cargo, char *amount, char *destination)
{
	LVITEM LvItem;
	LvItem.mask = LVIF_TEXT; 
	LvItem.cchTextMax = 256; // Max size of test
	LvItem.iItem=0;          // choose item  


	LvItem.iSubItem=0;       // Put in first coluom
	LvItem.pszText=cargo;
	SendMessage(content, LVM_INSERTITEM, 0, (LPARAM) &LvItem);
	LvItem.iSubItem=1;       // Put in first coluom
	LvItem.pszText=amount;
	SendMessage(content, LVM_SETITEM, 0, (LPARAM) &LvItem);
	LvItem.iSubItem=2;       // Put in first coluom
	LvItem.pszText=destination;
	SendMessage(content, LVM_SETITEM, 0, (LPARAM) &LvItem);
	return true;
}
BOOL removeAllContent()
{
	SendMessage(statusBar, SB_SETTEXT, 2, (LPARAM) "[No aircraft]");
	SendMessage(statusBar, SB_SETTEXT, 3, (LPARAM) "[No timeout]");
	return SendMessage(content, LVM_DELETEALLITEMS, 0, (LPARAM) 0) == TRUE;
}
BOOL setContent(HWND hWnd, HINSTANCE hInstance)
{
   INITCOMMONCONTROLSEX controls;
   controls.dwSize = sizeof(controls);
   controls.dwICC = ICC_BAR_CLASSES|ICC_LISTVIEW_CLASSES;
   if (InitCommonControlsEx(&controls) == FALSE)
	   return FALSE;

   statusBar=CreateStatusWindow(WS_CHILD|WS_VISIBLE, "", hWnd, ID_STATUSBAR);
   int parts[4]={70, 220, 290, 400};
   SendMessage(statusBar, SB_SETPARTS, 4, (LPARAM) parts);
   
   RECT rcParent;
   GetClientRect(hWnd, &rcParent);
   content = CreateWindowEx(0, WC_LISTVIEW, (LPCTSTR) NULL, WS_CHILD|WS_VISIBLE|LVS_REPORT,
		0, 0, rcParent.right, rcParent.bottom-20, hWnd, (HMENU)ID_HEADER, hInstance, (LPVOID) NULL);
	
   LVCOLUMN lvc;
   memset(&lvc, 0, sizeof(lvc));
   lvc.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
   lvc.cx = 100;
   lvc.fmt = LVCFMT_LEFT;
   lvc.pszText = "Cargo";
   ListView_InsertColumn(content, 0, &lvc);
   lvc.pszText = "Amount";
   ListView_InsertColumn(content, 1, &lvc);
   lvc.pszText = "Destination";
   ListView_InsertColumn(content, 2, &lvc);
   removeAllContent();
   return true;
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   HWND hWnd;

   hInst = hInstance; // Store instance handle in our global variable

   hWnd = CreateWindow(szWindowClass, szTitle, WS_MINIMIZEBOX|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN|WS_CAPTION|WS_BORDER|WS_SYSMENU,
      CW_USEDEFAULT, 0, 400, 200, NULL, NULL, hInstance, NULL);

   if (!hWnd)
      return FALSE;

   setContent(hWnd, hInstance);

   fillAccount();
   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);
   hwndMain = hWnd;
   SetTimer(hWnd, 1, 1000, TimerProc);

   return TRUE;
}

//
//  FUNCTION: WndProc(HWND, unsigned, WORD, LONG)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND	- process the application menu
//  WM_PAINT	- Paint the main window
//  WM_DESTROY	- post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;
	PAINTSTRUCT ps;
	HDC hdc;

	switch (message) 
	{
	case WM_COMMAND:
		wmId    = LOWORD(wParam); 
		wmEvent = HIWORD(wParam); 
		// Parse the menu selections:
		switch (wmId)
		{
		case ID_FILE_WEBSITE:
			ShellExecute(NULL, "open", WEBSITE , "", "c:\\", SW_SHOWNORMAL);
			break;
		case IDM_ABOUT:
			DialogBox(hInst, (LPCTSTR)IDD_ABOUTBOX, hWnd, (DLGPROC)About);
			break;
		case IDM_EXIT:
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			break;
		case ID_ACTION_START:
			doStart(hWnd);
			break;
		case ID_ACTION_CANCEL:
			doCancelFlight(hWnd);
			break;
		case IDM_ACCOUNT:
			DialogBox(hInst, (LPCTSTR)IDD_TEST, hWnd, (DLGPROC)Account);
			break;
		case ID_ACTION_TESTAIRCRAFT:
			testAircraft(hWnd);
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}
		break;
	case WM_PAINT:
		{
		hdc = BeginPaint(hWnd, &ps);

	//	hdcmem = CreateCompatibleDC(hdc);


	//	DeleteDC(hdcmem);

		// TODO: Add any drawing code here...
		EndPaint(hWnd, &ps);
		break;
		}
	case WM_CLOSE:
		{
			if (MessageBox(NULL, "Do you really want to exit the program?", "FSEconomy", MB_OKCANCEL|MB_ICONINFORMATION|MB_SETFOREGROUND) == IDOK)
				PostQuitMessage(0);
			break;
		}

	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// Message handler for StartCheck box.
LRESULT CALLBACK Weight(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		{
			char buffer[1024];
			int weightLb = (int)(totalWeight * 2.20462262);
			sprintf(buffer, "%d Kg (%d Lb)", totalWeight, weightLb);
			SetDlgItemText(hDlg, IDC_REQUIRED, buffer);
			weightLb = (int)(payloadWeight * 2.20462262);
			sprintf(buffer, "%d Kg (%d Lb)", payloadWeight, weightLb);
			SetDlgItemText(hDlg, IDC_PAYLOAD, buffer);
			EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);
			SetTimer(hDlg, 1, 1000, NULL);
		}
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) 
		{
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	case WM_TIMER:
		{
			RedrawWindow(hDlg, NULL, NULL, RDW_INVALIDATE|RDW_ERASE);
			break;
		}
	case WM_PAINT:
		{
			char buffer[1024];
			int curWeight = (int) currentWeight;
			int curWeightKg = curWeight / 2.20462262;
			int difference = curWeightKg - totalWeight;
			if (abs(difference) < 10)
			{
				strcpy(buffer, "Payload is correct");
				EnableWindow(GetDlgItem(hDlg, IDOK), TRUE);
			} else
			{
				int weightLb = (int)(difference * 2.20462262);
				char *action = difference < 0 ? "add" : "remove";
				sprintf(buffer, "Please %s %d Kg (%d Lb)", action, abs(difference), abs(weightLb));
				EnableWindow(GetDlgItem(hDlg, IDOK), FALSE);
			}
			SetDlgItemText(hDlg, IDC_ADVICE, buffer);
			sprintf(buffer, "%d Kg (%d Lb)", curWeightKg, curWeight);
			SetDlgItemText(hDlg, IDC_CURRENTWEIGHT, buffer);

		}
		break;
	}
	return FALSE;
}

// Message handler for about box.
LRESULT CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) 
		{
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	return FALSE;
}

// Message handler for account box.
LRESULT CALLBACK Account(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		SetDlgItemText(hDlg, IDC_USER, user);
		SetDlgItemText(hDlg, IDC_PASSWORD, password);
		SetFocus(GetDlgItem(hDlg, IDC_USER));
		return FALSE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
			case IDOK:
			{
				if (GetDlgItemText(hDlg, IDC_USER, user, 80) && GetDlgItemText(hDlg, IDC_PASSWORD, password, 80))
					setAccount();
			}

                    // Fall through.
             case IDCANCEL:
				EndDialog(hDlg, wParam);
				return TRUE;
            }
	}
	return FALSE;
}

// Message handler for Reporting box.
LRESULT CALLBACK Reporting(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		SetTimer(hDlg, 1, 1000, NULL);
		return FALSE;
	case WM_TIMER:
		{
			KillTimer(hDlg, 1);
			if (Communicate("arrive", 1, "%s", logString) == 200)
			{
				SendMessage(GetDlgItem(hDlg, IDC_PROGRESS1), PBM_SETPOS, 100, 0);
				SetDlgItemText(hDlg, IDC_STATIC, flightParams->find("mess"));
				EnableWindow(GetDlgItem(hDlg, IDCANCEL), TRUE);
				SetDlgItemText(hDlg, IDCANCEL, "Ok");
			} else
			{
				SetTimer(hDlg, 1, 5000, NULL);
				EnableWindow(GetDlgItem(hDlg, IDCANCEL), TRUE);
			}
		}
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
             case IDCANCEL:
				EndDialog(hDlg, wParam);
				return TRUE;
            }
	}
	return FALSE;
}
