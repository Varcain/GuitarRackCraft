/* Minimal stub for Apple Bonjour dnssd.dll so the iLok License Manager's
 * dynamic LoadLibrary("dnssd.dll")+GetProcAddress succeed (it crashes on a
 * NULL handle otherwise). No real mDNS — discovery just reports unsupported;
 * iLok Cloud / machine activation does not need Bonjour. x86_64 (runs under FEX). */
#include <stdint.h>
#define ERR_UNSUPPORTED (-65548)   /* kDNSServiceErr_Unsupported */
__declspec(dllexport) int32_t DNSServiceBrowse(void){ return ERR_UNSUPPORTED; }
__declspec(dllexport) int32_t DNSServiceGetAddrInfo(void){ return ERR_UNSUPPORTED; }
__declspec(dllexport) int32_t DNSServiceResolve(void){ return ERR_UNSUPPORTED; }
__declspec(dllexport) int32_t DNSServiceProcessResult(void){ return ERR_UNSUPPORTED; }
__declspec(dllexport) void    DNSServiceRefDeallocate(void){ }
__declspec(dllexport) int     DNSServiceRefSockFD(void){ return -1; }
