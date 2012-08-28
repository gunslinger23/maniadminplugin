// Sigscan code developed by Bailopan from www.sourcemm.net

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#if defined _WIN32 || defined WIN32
//   #define WIN32_LEAN_AND_MEAN
   #include <windows.h>
   #include <winnt.h>
#else
   #include <elf.h>
   #include <fcntl.h>
   #include <link.h>
   #include <sys/mman.h>
   #include <sys/stat.h>
   #include <demangle.h>
   #include "mani_linuxutils.h"
#endif
#include "interface.h"
#include "filesystem.h"
#include "engine/iserverplugin.h"
#include "iplayerinfo.h"
#include "eiface.h"
#include "igameevents.h"
#include "mrecipientfilter.h"
#include "bitbuf.h"
#include "engine/IEngineSound.h"
#include "inetchannelinfo.h"
#include "networkstringtabledefs.h"
#include "mani_main.h"
#include "mani_memory.h"
#include "mani_output.h"
#include "mani_gametype.h"
#include "mani_sigscan.h"
#include "mani_player.h"
#include "mani_vfuncs.h"
#include "cbaseentity.h"
#include "beam_flags.h"
#include "sourcehook.h"

extern	void *gamedll;
extern	int	max_players;
extern	CGlobalVars *gpGlobals;
extern	bool war_mode;
extern	int	con_command_index;
extern	bf_write *msg_buffer;
extern SourceHook::ISourceHook *g_SHPtr;

typedef void (*UTIL_Remove_)(CBaseEntity *);
typedef CCSWeaponInfo* (*CCSGetFileWeaponInfoHandle_)(unsigned short);

void *respawn_addr = NULL;
void *util_remove_addr = NULL;
void *ent_list_find_ent_by_classname = NULL;
void *console_echo_addr = NULL;
void *switch_team_addr = NULL;
void *set_model_from_class = NULL;
void *get_file_weapon_info_addr = NULL;
//void *get_weapon_price_addr = NULL;
//void *get_weapon_addr = NULL;
void *weapon_owns_this_type_addr = NULL;
//void *get_black_market_price_addr = NULL;
void *update_client_addr = NULL;
void *connect_client_addr = NULL;
void *netsendpacket_addr = NULL;

CBaseEntityList *g_pEList = NULL;
CGameRules *g_pGRules = NULL;
UTIL_Remove_ UTILRemoveFunc;
CCSGetFileWeaponInfoHandle_ CCSGetFileWeaponInfoHandleFunc;

static void ShowSigInfo(void *ptr, char *sig_name);

#ifdef WIN32
static void *FindSignature( unsigned char *pBaseAddress, size_t baseLength, unsigned char *pSignature, size_t sigLength);
static bool GetDllMemInfo(void *pAddr, unsigned char **base_addr, size_t *base_len);
#endif

class ManiEmptyClass {};

#ifdef WIN32

static 
unsigned char HexToBin(char hex_char)
{
	char upper_char = toupper(hex_char);
	return ((upper_char >= '0' && upper_char <= '9') ? upper_char - 48:upper_char - 55);
}

static bool ValidHexChar(char hex_char)
{
	char upper_char = toupper(hex_char);
	return ((upper_char >= '0' && upper_char <= '9') || (upper_char >= 'A' && upper_char <= 'F'));
}

static
void *FindSignature( unsigned char *pBaseAddress, size_t baseLength, unsigned char *pSignature)
{
	unsigned char *pBasePtr = pBaseAddress;
	unsigned char *pEndPtr = pBaseAddress + baseLength;	

	unsigned char	sigscan[128];
	bool			sigscan_wildcard[128];
	unsigned int	scan_length = 0;
	int				str_index = 0;

	while(1)
	{
		if (pSignature[str_index] == ' ')
		{
			str_index++;
			continue;
		}

		if (pSignature[str_index] == '\0')
		{
			break;
		}

		if (pSignature[str_index] == '?')
		{
			sigscan_wildcard[scan_length++] = true;
			str_index ++;
			continue;
		}

		if (pSignature[str_index + 1] == '\0' || 
			pSignature[str_index + 1] == '?' || 
			pSignature[str_index + 1] == ' ')
		{
			MMsg("Failed to decode [%s], single digit hex code\n", pSignature);
			return (void *) NULL;
		}

		// We are expecting a two digit hex code
		char upper_case1 = toupper(pSignature[str_index]);
		if (!ValidHexChar(upper_case1))
		{
			MMsg("Failed to decode [%s], bad hex code\n", pSignature);
			return NULL;
		}

		char upper_case2 = toupper(pSignature[str_index + 1]);
		if (!ValidHexChar(upper_case2))
		{
			MMsg("Failed to decode [%s], bad hex code\n", pSignature);
			return NULL;
		}

		// Generate our byte code
		unsigned char byte = (HexToBin(upper_case1) << 4) + HexToBin(upper_case2);
		str_index += 2;
		sigscan_wildcard[scan_length] = false;
		sigscan[scan_length] = byte;
		scan_length ++;
		if (scan_length == sizeof(sigscan))
		{
			MMsg("Sigscan too long!\n");
			return NULL;
		}
	}

	unsigned int i;
	while (pBasePtr < pEndPtr)
	{
		for (i=0; i < scan_length; i++)
		{
			if (sigscan_wildcard[i] != true && sigscan[i] != pBasePtr[i])
				break;
		}
		//iff i reached the end, we know we have a match!
		if (i == scan_length)
			return (void *)pBasePtr;
		pBasePtr += sizeof(unsigned char);  //search memory in an aligned manner
	}

	return NULL;
}
//Example usage:
//void *sigaddr = FindSignature(pBaseAddress, baseLength, MKSIG(TerminateRound));

static
bool GetDllMemInfo(void *pAddr, unsigned char **base_addr, size_t *base_len)
{
	MEMORY_BASIC_INFORMATION mem;

	if(!pAddr)
		return false; // GetDllMemInfo failed: !pAddr

	if(!VirtualQuery(pAddr, &mem, sizeof(mem)))
		return false;

	*base_addr = (unsigned char *)mem.AllocationBase;

	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)mem.AllocationBase;
	IMAGE_NT_HEADERS *pe = (IMAGE_NT_HEADERS*)((unsigned long)dos+(unsigned long)dos->e_lfanew);

	if(pe->Signature != IMAGE_NT_SIGNATURE) {
		*base_addr = 0;
		return false; // GetDllMemInfo failed: pe points to a bad location
	}

	*base_len = (size_t)pe->OptionalHeader.SizeOfImage;
	return true;
}

#endif

bool CCSRoundRespawn(CBaseEntity *pThisPtr)
{
	if (!respawn_addr) return false;

	void **this_ptr = *(void ***)&pThisPtr;
	void *func = respawn_addr;

	union {void (ManiEmptyClass::*mfpnew)();
#ifndef __linux__
        void *addr;	} u; 	u.addr = func;
#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
#endif

	(void) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)();
	return true;
}

bool	CCSUTILRemove(CBaseEntity *pCBE)
{
	if (!util_remove_addr) return false;

	if (pCBE)
	{
		UTILRemoveFunc(pCBE);
		return true;
	}

	return false;
}

CCSWeaponInfo	*CCSGetFileWeaponInfoFromHandle(unsigned short handle_id)
{
	if (!get_file_weapon_info_addr) return false;
	return CCSGetFileWeaponInfoHandleFunc(handle_id);
}

CBaseEntity * CGlobalEntityList_FindEntityByClassname(CBaseEntity *pCBE, const char *ent_class)
{
//	pWeapon = pThisPtr->Weapon_GetSlot(slot);

	if (!g_pEList) return NULL;
	if (!ent_list_find_ent_by_classname) return NULL;

	void **this_ptr = *(void ***)&g_pEList;
	void *func = ent_list_find_ent_by_classname;

	union {CBaseEntity * (ManiEmptyClass::*mfpnew)(CBaseEntity *pCBE, const char *ent_class);
#ifndef __linux__
        void *addr;	} u; 	u.addr = func;
#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
#endif

	return (CBaseEntity *) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(pCBE, ent_class);
}

bool CCSPlayer_SwitchTeam(CBaseEntity *pCBE, int team_index)
{
//	pWeapon = pThisPtr->Weapon_GetSlot(slot);

	if (!switch_team_addr) return false;

	CBasePlayer *pCBP = (CBasePlayer *) pCBE;

	void **this_ptr = *(void ***)&pCBP;
	void *func = switch_team_addr;

	union {void (ManiEmptyClass::*mfpnew)(int team_index);
#ifndef __linux__
        void *addr;	} u; 	u.addr = func;
#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
#endif

	(void) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(team_index);
	return true;
}

bool CCSPlayer_SetModelFromClass(CBaseEntity *pCBE)
{
	if (!set_model_from_class) return false;

	CBasePlayer *pCBP = (CBasePlayer *) pCBE;

	void **this_ptr = *(void ***)&pCBP;
	void *func = set_model_from_class;

	union {void (ManiEmptyClass::*mfpnew)();
#ifndef __linux__
        void *addr;	} u; 	u.addr = func;
#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
#endif

	(void) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)();
	return true;
}

//int CCSWeaponInfo_GetWeaponPriceFunc(CCSWeaponInfo *weapon_info)
//{
//	if (!get_weapon_price_addr) return -1;
//
//	void **this_ptr = *(void ***)&weapon_info;
//	void *func = get_weapon_price_addr;
//
//	union {int (ManiEmptyClass::*mfpnew)(CCSWeaponInfo *weapon_info);
//#ifndef __linux__
//        void *addr;	} u; 	u.addr = func;
//#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
//			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
//#endif
//
//	return (int) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(weapon_info);
//}

//int CCSGameRules_GetBlackMarketPriceForWeaponFunc(int weapon_id)
//{
//	if (!get_black_market_price_addr) return -1;
//	if (!g_pGRules) return -1;
//
//	void **this_ptr = *(void ***)g_pGRules;
//	void *func = get_black_market_price_addr;
//
//	union {int (ManiEmptyClass::*mfpnew)(int weapon_id);
//#ifndef __linux__
//        void *addr;	} u; 	u.addr = func;
//#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
//			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
//#endif
//
//	return (int) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(weapon_id);
//}

//CBaseCombatWeapon *CBaseCombatCharacter_GetWeapon(CBaseCombatCharacter *pCBCC, int weapon_number)
//{
//	if (!get_weapon_addr) return NULL;
//
//	void **this_ptr = *(void ***)&pCBCC;
//	void *func = get_weapon_addr;
//
//	union {CBaseCombatWeapon *(ManiEmptyClass::*mfpnew)(int weapon_number);
//#ifndef __linux__
//        void *addr;	} u; 	u.addr = func;
//#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
//			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
//#endif
//
//	return (CBaseCombatWeapon *) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(weapon_number);
//}

CBaseCombatWeapon *CBaseCombatCharacter_Weapon_OwnsThisType(CBaseCombatCharacter *pCBCC, const char *weapon_name, int sub_type)
{
	if (!weapon_owns_this_type_addr) return NULL;

	void **this_ptr = *(void ***)&pCBCC;
	void *func = weapon_owns_this_type_addr;

	union {CBaseCombatWeapon *(ManiEmptyClass::*mfpnew)(const char *weapon_name, int sub_type);
#ifndef __linux__
        void *addr;	} u; 	u.addr = func;
#else /* GCC's member function pointers all contain a this pointer adjustor. You'd probably set it to 0 */
			struct {void *addr; intptr_t adjustor;} s; } u; u.s.addr = func; u.s.adjustor = 0;
#endif

	return (CBaseCombatWeapon *) (reinterpret_cast<ManiEmptyClass*>(this_ptr)->*u.mfpnew)(weapon_name, sub_type);
}


void LoadSigScans(void)
{
	// Need to get engine hook here for clients connecting
#ifdef WIN32
	unsigned char *engine_base = 0;
	size_t engine_len = 0;

	bool engine_success = GetDllMemInfo(engine, &engine_base, &engine_len);

	if (engine_success) {
		MMsg("Found engine base %p and length %i [%p]\n", engine_base, engine_len, engine_base + engine_len);
		connect_client_addr = FindSignature (engine_base, engine_len, (unsigned char*) CBaseServer_ConnectClient_Sig);
		netsendpacket_addr =  FindSignature (engine_base, engine_len, (unsigned char*) NET_SendPacket_Sig);
	}
#else
	// Not using Gamegin for following using engine.so instead
	SymbolMap *engine_sym_ptr;
	engine_sym_ptr = new SymbolMap;
	if (!engine_sym_ptr->GetLib(gpManiGameType->GetLinuxEngine()))
	{
		MMsg("Failed to open [%s]\n", gpManiGameType->GetLinuxEngine());
	}

	connect_client_addr = engine_sym_ptr->FindAddress(CBaseServer_ConnectClient_Linux);
	netsendpacket_addr  = engine_sym_ptr->FindAddress(NET_SendPacket_Linux);

	/* Call deconstructor to cleanup */
	delete engine_sym_ptr;
#endif
	MMsg("Sigscan info\n");
	ShowSigInfo(connect_client_addr, "CBaseServer::ConnectClient");
	ShowSigInfo(netsendpacket_addr, "NET_SendPacket");
	if (!gpManiGameType->IsGameType(MANI_GAME_CSS)) return;
#ifdef WIN32
	// Windows
	unsigned char *base = 0;
	size_t len = 0;

	bool success = GetDllMemInfo(gamedll, &base, &len);

	if (success)
	{
		MMsg("Found base %p and length %i [%p]\n", base, len, base + len);

		respawn_addr = FindSignature(base, len, (unsigned char *) CCSPlayer_RoundRespawn_Sig);
		util_remove_addr = FindSignature(base, len, (unsigned char *) UTIL_Remove_Sig);
		void *is_there_a_bomb_addr = FindSignature(base, len, (unsigned char *) CEntList_Sig);
		if (is_there_a_bomb_addr)
		{
			g_pEList = *(CBaseEntityList **) ((unsigned long) is_there_a_bomb_addr + (unsigned long) CEntList_gEntList);
		}

		update_client_addr = FindSignature(base, len, (unsigned char *) CBasePlayer_UpdateClientData_Sig);
		if (update_client_addr)
		{
			g_pGRules = *(CGameRules **) ((unsigned long) update_client_addr + (unsigned long) CGameRules_gGameRules);
		}

		ent_list_find_ent_by_classname = FindSignature(base, len, (unsigned char *) CGlobalEntityList_FindEntityByClassname_Sig);
		switch_team_addr = FindSignature(base, len, (unsigned char *) CCSPlayer_SwitchTeam_Sig);
		set_model_from_class = FindSignature(base, len, (unsigned char *) CCSPlayer_SetModelFromClass_Sig);
		get_file_weapon_info_addr = FindSignature(base, len, (unsigned char *) GetFileWeaponInfoFromHandle_Sig);
		//get_weapon_price_addr = FindSignature(base, len, (unsigned char *) CCSWeaponInfo_GetWeaponPrice_Sig);
		//if (get_weapon_price_addr)
		//{
		//	unsigned long offset = *(unsigned long *) ((unsigned long) get_weapon_price_addr + (unsigned long) (CCSWeaponInfo_GetWeaponPrice_Offset));
		//	get_weapon_price_addr = (void *) ((unsigned long) get_weapon_price_addr + (unsigned long) (offset));
		//}
		//get_weapon_addr = FindSignature(base, len, (unsigned char *) CBaseCombatCharacter_GetWeapon_Sig);
		//get_black_market_price_addr = FindSignature(base, len, (unsigned char *) CCSGameRules_GetBlackMarketPriceForWeapon_Sig);
		weapon_owns_this_type_addr = FindSignature(base, len, (unsigned char *) CBaseCombatCharacter_Weapon_OwnsThisType_Sig);
	}
	else
	{
		MMsg("Did not find base and length for gamedll\n");
	}

#else
	void *ptr;

	// Linux
	if (!gpManiGameType->IsGameType(MANI_GAME_CSS)) return;

	SymbolMap *game_sym_ptr = new SymbolMap;
	if (!game_sym_ptr->GetLib(gpManiGameType->GetLinuxBin()))
	{
		MMsg("Failed to open [%s]\n", gpManiGameType->GetLinuxBin());
	}

	respawn_addr = game_sym_ptr->FindAddress(CCSPlayer_RoundRespawn_Linux);
	util_remove_addr = game_sym_ptr->FindAddress(UTIL_Remove_Linux);
	g_pEList = *(CBaseEntityList **) game_sym_ptr->FindAddress(CEntList_Linux);
	ent_list_find_ent_by_classname = game_sym_ptr->FindAddress(CGlobalEntityList_FindEntityByClassname_Linux);
	switch_team_addr = game_sym_ptr->FindAddress(CCSPlayer_SwitchTeam_Linux);
	set_model_from_class = game_sym_ptr->FindAddress(CCSPlayer_SetModelFromClass_Linux);

	ptr = game_sym_ptr->FindAddress(CBaseCombatCharacter_SwitchToNextBestWeapon_Linux);
	if (ptr)
	{
		g_pGRules = *(CGameRules **) ((unsigned long) ptr + (unsigned long) 6);
	}

	get_file_weapon_info_addr = game_sym_ptr->FindAddress(GetFileWeaponInfoFromHandle_Linux);
	//get_weapon_price_addr = game_sym_ptr->FindAddress(CCSWeaponInfo_GetWeaponPrice_Linux);
	//get_weapon_addr = game_sym_ptr->FindAddress(CBaseCombatCharacter_GetWeapon_Linux);
	//get_black_market_price_addr = game_sym_ptr->FindAddress(CCSGameRules_GetBlackMarketPriceForWeapon_Linux);
	weapon_owns_this_type_addr = game_sym_ptr->FindAddress(CBaseCombatCharacter_Weapon_OwnsThisType_Linux);

	/* Call deconstructor to cleanup */
	delete game_sym_ptr;


#endif
	MMsg("Sigscan info\n");
	ShowSigInfo(respawn_addr, "A");
	ShowSigInfo(util_remove_addr, "B");
	if (util_remove_addr != NULL)
	{
		UTILRemoveFunc = (UTIL_Remove_) util_remove_addr;
	}

	ShowSigInfo(g_pEList, "C");
#ifdef WIN32
	ShowSigInfo(update_client_addr, "D");
#endif
	ShowSigInfo(g_pGRules, "D1");
	ShowSigInfo(ent_list_find_ent_by_classname, "E");
	ShowSigInfo(switch_team_addr, "F");
	ShowSigInfo(set_model_from_class, "G");
	ShowSigInfo(get_file_weapon_info_addr, "H");
	if (get_file_weapon_info_addr != NULL)
	{
		CCSGetFileWeaponInfoHandleFunc = (CCSGetFileWeaponInfoHandle_) get_file_weapon_info_addr;
	}

//	ShowSigInfo(get_weapon_price_addr, "I");
//	ShowSigInfo(get_weapon_addr, "J");
//	ShowSigInfo(get_black_market_price_addr, "K");
	ShowSigInfo(weapon_owns_this_type_addr, "L");
}

static
void ShowSigInfo(void *ptr, char *sig_name)
{
	if (ptr == NULL)
	{
		MMsg("%s Failed\n", sig_name);
	}
	else
	{
		MMsg("%s [%p]\n", sig_name, ptr);
	}
}
