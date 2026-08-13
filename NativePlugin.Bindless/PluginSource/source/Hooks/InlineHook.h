#pragma once
// Stolen from https://github.com/saivs/com.saivs.plugin.mdi/blob/main/NativePlugin~/src/InlineHook.h

#ifdef _WIN32

#include <cstdint>
#include <cstring>
#include <windows.h>
#include "../UAL/UnityLog.h"

struct InlineHookData
{
    uint8_t* trampoline = nullptr;
    void* target = nullptr;
    uint8_t  savedBytes[32] = {};
    size_t   patchSize = 0;
};

class InlineHooks {
public:
    static bool InstallInlineHook(void* target, void* hook, InlineHookData* hd)
    {
        if (hd != nullptr) {
            *hd = {};
        }

        if (!target || !hook) return false;

        constexpr size_t kJmpSize = 12;

        auto* fn = static_cast<uint8_t*>(target);
        size_t copySize = 0;
        while (copySize < kJmpSize)
        {
            size_t len = X64InsnLength(fn + copySize);
            if (len == 0)
            {
                UnityLog::Log("[Bindless] InlineHook: unknown instruction at offset %zu (byte 0x%02X) in %p\n",
                    copySize, fn[copySize], target);
                return false;
            }
            copySize += len;
        }

        uint8_t tempSaveBytes[32];
        uint8_t* savedBytesPtr = &tempSaveBytes[0];
        //auto maxNumSaveBytes = sizeof(hd.savedBytes);
        auto maxNumSaveBytes = 32;

        if (copySize > maxNumSaveBytes)
        {
            UnityLog::Log("[Bindless] InlineHook: copySize %zu too large for %p\n", copySize, target);
            return false;
        }

        if (hd != nullptr) {
            savedBytesPtr = &hd->savedBytes[0];
        }

        memcpy(savedBytesPtr, target, copySize);

        UnityLog::Debug("[Bindless] InlineHook target %p copySize=%zu bytes: "
            "%02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X\n",
            target, copySize,
            fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7],
            fn[8], fn[9], fn[10], fn[11], fn[12], fn[13], fn[14], fn[15],
            fn[16], fn[17], fn[18], fn[19]);

        auto trampoline = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        if (trampoline == nullptr)
            return false;

        memcpy(trampoline, savedBytesPtr, copySize);

        trampoline[copySize + 0] = 0xFF;
        trampoline[copySize + 1] = 0x25;
        trampoline[copySize + 2] = 0x00;
        trampoline[copySize + 3] = 0x00;
        trampoline[copySize + 4] = 0x00;
        trampoline[copySize + 5] = 0x00;

        auto returnAddr = reinterpret_cast<uintptr_t>(fn + copySize);
        memcpy(trampoline + copySize + 6, &returnAddr, 8);

        DWORD oldProtect;
        VirtualProtect(target, kJmpSize, PAGE_EXECUTE_READWRITE, &oldProtect);
        fn[0] = 0x48; fn[1] = 0xB8;
        auto hookAddr = reinterpret_cast<uintptr_t>(hook);
        memcpy(fn + 2, &hookAddr, 8);
        fn[10] = 0xFF; fn[11] = 0xE0;
        VirtualProtect(target, kJmpSize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), target, kJmpSize);

        uint8_t verify[12];
        memcpy(verify, target, 12);
        bool patchOk = (verify[0] == 0x48 && verify[1] == 0xB8 && verify[10] == 0xFF && verify[11] == 0xE0);
        UnityLog::Log("[Bindless] InlineHook installed at %p -> %p, trampoline at %p, verified=%d\n",
            target, hook, trampoline, patchOk);

        if (hd != nullptr) {
            hd->target = target;
            hd->patchSize = copySize;
            hd->trampoline = trampoline;
        }

        return patchOk;
    }

    static void RemoveInlineHook(InlineHookData& hd)
    {
        if (hd.target && hd.patchSize > 0)
        {
            DWORD oldProtect;
            VirtualProtect(hd.target, hd.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(hd.target, hd.savedBytes, hd.patchSize);
            VirtualProtect(hd.target, hd.patchSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), hd.target, hd.patchSize);
        }
        if (hd.trampoline)
        {
            VirtualFree(hd.trampoline, 0, MEM_RELEASE);
            hd.trampoline = nullptr;
        }
        hd.target = nullptr;
        hd.patchSize = 0;
    }

protected:

    // Minimal x86_64 instruction length decoder for common D3D11/D3D12 function prologues.
    static size_t X64InsnLength(const uint8_t* code)
    {
        size_t off = 0;

        while (code[off] == 0x66 || code[off] == 0x67 ||
            (code[off] >= 0x40 && code[off] <= 0x4F))
            off++;

        uint8_t op = code[off++];

        if ((op >= 0x50 && op <= 0x5F) || op == 0xC3 || op == 0x90 || op == 0xCC)
            return off;

        if (op == 0x8B || op == 0x89 || op == 0x8D ||
            op == 0x85 || op == 0x3B || op == 0x39 || op == 0x33 || op == 0x31 ||
            op == 0x2B || op == 0x29 || op == 0x23 || op == 0x21 || op == 0x0B || op == 0x09 ||
            op == 0x03 || op == 0x01)
        {
            uint8_t modrm = code[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            if (mod == 3) return off;
            if (rm == 4) off++;
            if (mod == 0 && rm == 5) off += 4;
            else if (mod == 1) off += 1;
            else if (mod == 2) off += 4;
            return off;
        }

        if (op == 0x81)
        {
            uint8_t modrm = code[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            if (mod == 3) {} else { if (rm == 4) off++; if (mod == 0 && rm == 5) off += 4; else if (mod == 1) off += 1; else if (mod == 2) off += 4; }
            off += 4;
            return off;
        }

        if (op == 0x83)
        {
            uint8_t modrm = code[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            if (mod == 3) {} else { if (rm == 4) off++; if (mod == 0 && rm == 5) off += 4; else if (mod == 1) off += 1; else if (mod == 2) off += 4; }
            off += 1;
            return off;
        }

        if (op >= 0xB8 && op <= 0xBF)
        {
            bool rexW = (off >= 2 && (code[0] & 0x48) == 0x48);
            off += rexW ? 8 : 4;
            return off;
        }

        if (op >= 0xB0 && op <= 0xB7) { off += 1; return off; }

        if (op == 0xE8 || op == 0xE9) { off += 4; return off; }

        if (op >= 0x70 && op <= 0x7F) { off += 1; return off; }

        if (op == 0xC7)
        {
            uint8_t modrm = code[off++];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            if (mod == 3) {} else { if (rm == 4) off++; if (mod == 0 && rm == 5) off += 4; else if (mod == 1) off += 1; else if (mod == 2) off += 4; }
            off += 4;
            return off;
        }

        if (op == 0x0F)
        {
            uint8_t op2 = code[off++];
            if (op2 >= 0x80 && op2 <= 0x8F) { off += 4; return off; }
            {
                uint8_t modrm = code[off++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm = modrm & 7;
                if (mod == 3) return off;
                if (rm == 4) off++;
                if (mod == 0 && rm == 5) off += 4;
                else if (mod == 1) off += 1;
                else if (mod == 2) off += 4;
                return off;
            }
        }

        return 0;
    }
};

#endif // _WIN32