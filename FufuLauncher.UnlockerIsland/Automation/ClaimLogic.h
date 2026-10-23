// Copyright (c) FufuLauncher Dev Team. AGPL-3.0.
// Scheduling and request ABI adapted from QuickSubmitter/CommissionQuickClaim.
#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <limits>


namespace Shortcuts
{
    class HotkeyEdge
    {
        bool previous=false;
        int previousKey=0;
    public:
        bool Sample(int key,bool down,bool focused,bool enabled)
        {
            if(key!=previousKey) { previousKey=key; previous=down; return false; }
            const bool pressed=down && !previous;
            previous=down;
            return enabled && focused && key>=1 && key<=254 && pressed;
        }
    };
    enum class ClaimResult { Idle, FirstCalled, FirstFailed, SecondCalled, SecondFailed };
    class ClaimScheduler
    {
        bool pending=false;
        uint32_t started=0;
    public:
        bool Pending() const { return pending; }
        void Reset() { pending=false; started=0; }
        template<class Call> ClaimResult Tick(bool ready,uint32_t now,bool pressed,Call call)
        {
            if(!ready) { Reset(); return ClaimResult::Idle; }
            if(pending)
            {
                if(uint32_t(now-started)<600) return ClaimResult::Idle;
                pending=false;
                return call(1)?ClaimResult::SecondCalled:ClaimResult::SecondFailed;
            }
            if(!pressed) return ClaimResult::Idle;
            if(!call(0)) return ClaimResult::FirstFailed;
            started=now; pending=true; return ClaimResult::FirstCalled;
        }
    };
    inline bool ReadMemory(uintptr_t address,void* output,size_t size) noexcept
    {
        if(!output || !size || address<0x10000 || address>0x00007FFFFFFFFFFF || size-1>UINTPTR_MAX-address) return false;
        auto cursor=address; auto remaining=size;
        while(remaining)
        {
            MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
            const auto access=info.Protect&0xFF;
            if(access!=PAGE_READONLY && access!=PAGE_READWRITE && access!=PAGE_WRITECOPY && access!=PAGE_EXECUTE_READ && access!=PAGE_EXECUTE_READWRITE && access!=PAGE_EXECUTE_WRITECOPY) return false;
            const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
            if(end<=cursor) return false;
            const auto count=(end-cursor)<remaining?end-cursor:remaining;
            cursor+=count; remaining-=count;
        }
        __try { std::memcpy(output,reinterpret_cast<const void*>(address),size); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    template<class T> bool ReadField(uintptr_t base,uintptr_t offset,T& result) noexcept
    {
        return offset<=UINTPTR_MAX-base && ReadMemory(base+offset,&result,sizeof(result));
    }
    inline bool MatchGameHeader(uintptr_t base,uint32_t timestamp,uint32_t imageSize) noexcept
    {
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        return ReadField(base,0,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0 && dos.e_lfanew<0x100000 &&
            ReadField(base,uintptr_t(dos.e_lfanew),nt) && nt.Signature==IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 &&
            nt.FileHeader.TimeDateStamp==timestamp && nt.OptionalHeader.SizeOfImage==imageSize;
    }
    using AllocateRequest=uintptr_t(__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);
    using SendRequest=void(__fastcall*)(uintptr_t,uintptr_t,unsigned,uintptr_t);
    struct ClaimBackend
    {
        uintptr_t base=0,rootSlot=0,clientOffset=0,requiredPointer=0,readyByte=0,firstType=0,secondType=0;
        AllocateRequest allocate=nullptr;
        SendRequest send=nullptr;
        uintptr_t Client() const noexcept
        {
            uintptr_t root=0,client=0,required=0; uint8_t ready=0;
            return base && rootSlot && clientOffset && ReadField(base,rootSlot,root) && root &&
                ReadField(root,clientOffset,client) && client && ReadField(client,requiredPointer,required) && required &&
                ReadField(client,readyByte,ready) && ready?client:0;
        }
        bool Submit(uintptr_t client,unsigned stage) const noexcept
        {
            if(stage>1 || !allocate || !send || !client || client!=Client()) return false;
            const auto typeRva=stage==0?firstType:secondType;
            uintptr_t type=0;
            if(!typeRva || !ReadField(base,typeRva,type) || type<0x10000 || type>0x00007FFFFFFFFFFF) return false;
            __try
            {
                const auto request=allocate(type,0,typeRva,0);
                if(!request) return false;
                send(client,request,0,0); return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    };
}
