#pragma once

#include <Windows.h>

// Originals belong to a particular vtable AND hook, not to a process-wide
// method name. A ReShade proxy and the native device need different targets.
namespace d3d9chain {
struct Entry {
    void** table;
    unsigned int index;
    void* hook;
    void* next;
};

struct Registry {
    CRITICAL_SECTION lock;
    Entry entries[512];
    unsigned int count;
};

inline Registry& Hooks()
{
    // Constant initialization also works with /Zc:threadSafeInit-.
    // Use XP-era synchronization APIs, not Vista SRW-lock APIs/macros.
    static Registry registry = {};
    static volatile LONG initialized = 0;
    if (InterlockedCompareExchange(&initialized, 1, 0) == 0) {
        InitializeCriticalSection(&registry.lock);
        InterlockedExchange(&initialized, 2);
    } else {
        while (InterlockedCompareExchange(&initialized, 2, 2) != 2)
            Sleep(0);
    }
    return registry;
}

inline bool Install(void** table, unsigned int index, void* hook)
{
    if (!table || !hook) return false;
    Registry& registry = Hooks();
    EnterCriticalSection(&registry.lock);
    for (unsigned int i = 0; i < registry.count; ++i) {
        const Entry& entry = registry.entries[i];
        if (entry.table == table && entry.index == index && entry.hook == hook) {
            // Another hook may now sit above us. Reinstalling here would
            // create A -> B -> A recursion; keep our original chain link.
            LeaveCriticalSection(&registry.lock);
            return true;
        }
    }
    if (!table[index] || table[index] == hook || registry.count == 512) {
        LeaveCriticalSection(&registry.lock);
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(&table[index], sizeof(void*), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        LeaveCriticalSection(&registry.lock);
        return false;
    }
    registry.entries[registry.count++] = {table, index, hook, table[index]};
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&table[index]), hook);
    DWORD ignored = 0;
    VirtualProtect(&table[index], sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[index], sizeof(void*));
    LeaveCriticalSection(&registry.lock);
    return true;
}

template<class Function, class Object>
inline Function Next(Object* object, unsigned int index, void* hook)
{
    if (!object) return nullptr;
    void** table = *reinterpret_cast<void***>(object);
    Registry& registry = Hooks();
    void* next = nullptr;
    EnterCriticalSection(&registry.lock);
    for (unsigned int i = 0; i < registry.count; ++i) {
        const Entry& entry = registry.entries[i];
        if (entry.table == table && entry.index == index && entry.hook == hook) {
            next = entry.next;
            break;
        }
    }
    LeaveCriticalSection(&registry.lock);
    return reinterpret_cast<Function>(next);
}

// Shared by the scoreboard and AA translation units; native driver calls
// nested inside a wrapper's Present must not draw the overlays a second time.
inline unsigned int& PresentationDepth()
{
    static thread_local unsigned int depth = 0;
    return depth;
}

struct PresentScope {
    PresentScope() { ++PresentationDepth(); }
    ~PresentScope() { --PresentationDepth(); }
    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;
};

inline unsigned int& FeatureBypassDepth()
{
    static thread_local unsigned int depth = 0;
    return depth;
}

struct ProbeScope {
    ProbeScope() { ++FeatureBypassDepth(); }
    ~ProbeScope() { --FeatureBypassDepth(); }
    ProbeScope(const ProbeScope&) = delete;
    ProbeScope& operator=(const ProbeScope&) = delete;
};
} // namespace d3d9chain
