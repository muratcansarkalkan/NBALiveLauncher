#include "plugin-std.h"

#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <cstdlib>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>

using namespace plugin;

namespace custompaths
{
    enum GameVersion
    {
        GAME_UNKNOWN,
        GAME_LIVE_2005,
        GAME_LIVE_06,
        GAME_LIVE_07,
        GAME_LIVE_08
    };

    // NBA Live 06 bench pose overrides from assets\stadia\<id4>.json.
    // Missing benchPlayers or absent slots retain native game behavior.
    // The source EXE must be unmodified at the two validated hook sites.
    enum BenchPose { BENCH_NATIVE, BENCH_SEATED, BENCH_FLOOR };
    struct BenchSlotConfig
    {
        BenchPose pose = BENCH_NATIVE;
        bool hasOffset = false;
        float offset[3] = {};
        bool hasYaw = false;
        float yaw = 0.0f;
    };
    BenchSlotConfig gBenchSlots[2][7] = {};
    bool gBenchHasPositionRequests = false;
    // Set only by the 14 validated bench actor creation callsites.
    // Record addresses are used for identity, never as guessed position offsets.
    uintptr_t gBenchActorRecords[2][7] = {};


    // JSON is read only on stadium change. Parse strictly and leave the
    // entire feature at native defaults on invalid input.
    class BenchJson
    {
    public:
        explicit BenchJson(const std::string& contents) : text(contents) {}
        bool Parse(BenchSlotConfig (&slots)[2][7], bool& hasPosition)
        {
            hasPosition = false;
            if (!ObjectRoot(slots, hasPosition)) return false;
            Space(); return pos == text.size();
        }
    private:
        const std::string& text;
        size_t pos = 0;
        void Space() { while (pos < text.size() &&
            (text[pos]==' ' || text[pos]=='\n' || text[pos]=='\r' || text[pos]=='\t')) ++pos; }
        bool Take(char c) { Space(); if (pos >= text.size() || text[pos]!=c) return false; ++pos; return true; }
        bool Str(std::string& out)
        {
            if (!Take('"')) return false;
            out.clear();
            while (pos < text.size())
            {
                char c=text[pos++];
                if (c=='"') return true;
                if (c=='\\')
                {
                    if (pos==text.size()) return false;
                    c=text[pos++];
                    if (c=='"'||c=='\\'||c=='/') out.push_back(c);
                    else if (c=='n') out.push_back('\n');
                    else if (c=='t') out.push_back('\t');
                    else if (c=='r') out.push_back('\r');
                    else if (c=='b') out.push_back('\b');
                    else if (c=='f') out.push_back('\f');
                    else if (c=='u') {
                        // Consume a JSON Unicode escape. Bench schema field names
                        // are ASCII; escape sequences in other stadium data are skipped.
                        if (pos+4>text.size()) return false;
                        for (int i=0;i<4;++i)
                            if (!std::isxdigit(static_cast<unsigned char>(text[pos+i]))) return false;
                        pos+=4;
                        out.push_back('?');
                    }
                    else return false;
                }
                else { if (static_cast<unsigned char>(c)<32) return false; out.push_back(c); }
            }
            return false;
        }
        bool Number(float& v)
        {
            Space(); if (pos >= text.size()) return false;
            const char* ptr=text.c_str()+pos; char* end=nullptr;
            v=strtof(ptr,&end);
            if (ptr==end || !std::isfinite(v)) return false;
            pos+=static_cast<size_t>(end-ptr);
            return true;
        }
        bool Skip()
        {
            Space(); if (pos>=text.size()) return false;
            if (text[pos]=='{') {
                ++pos; if (Take('}')) return true;
                do { std::string k; if (!Str(k)||!Take(':')||!Skip()) return false;
                    if (Take('}')) return true; } while (Take(',')); return false;
            }
            if (text[pos]=='[') {
                ++pos; if (Take(']')) return true;
                do { if (!Skip()) return false; if (Take(']')) return true; }
                while (Take(',')); return false;
            }
            if (text[pos]=='"') {std::string x;return Str(x);}
            for (const char* keyword : {"true","false","null"}) {
                const size_t n=strlen(keyword);
                if (text.compare(pos,n,keyword)==0) {pos+=n;return true;}
            }
            float v=0; return Number(v);
        }
        bool Offset(BenchSlotConfig& slot)
        {
            if (!Take('[')) return false;
            for (int i=0;i<3;++i) {
                if (i && !Take(',')) return false;
                if (!Number(slot.offset[i])) return false;
            }
            if (!Take(']')) return false;
            slot.hasOffset=true; return true;
        }
        bool Slot(BenchSlotConfig& slot, bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string k; if (!Str(k)||!Take(':')) return false;
                if (k=="pose") {
                    std::string v; if (!Str(v)) return false;
                    if (v=="default") slot.pose=BENCH_NATIVE;
                    else if (v=="seated") slot.pose=BENCH_SEATED;
                    else if (v=="floor") slot.pose=BENCH_FLOOR;
                    else return false;
                } else if (k=="positionOffset") {
                    if (!Offset(slot)) return false; hasPosition=true;
                } else if (k=="yawOffset") {
                    if (!Number(slot.yaw)) return false;
                    slot.hasYaw=true;hasPosition=true;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(','));
            return false;
        }
        bool Side(BenchSlotConfig (&slots)[7], bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string k; if (!Str(k)||!Take(':')) return false;
                if (k=="slots") {
                    if (!Take('[')) return false;
                    int n=0;
                    if (!Take(']')) {
                        do {
                            if (n>=7 || !Slot(slots[n++],hasPosition)) return false;
                            if (Take(']')) break;
                        } while (Take(','));
                        if (n!=7) return false;
                    } else return false;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(',')); return false;
        }
        bool BenchPlayers(BenchSlotConfig (&slots)[2][7], bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string key; if (!Str(key)||!Take(':')) return false;
                if (key=="home" || key=="away") {
                    if (!Side(slots[key=="away"?1:0],hasPosition)) return false;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(','));
            return false;
        }
        bool ObjectRoot(BenchSlotConfig (&slots)[2][7], bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string key; if (!Str(key)||!Take(':')) return false;
                if (key=="benchPlayers") {
                    if (!BenchPlayers(slots,hasPosition)) return false;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(','));return false;
        }
    };
    void LoadBenchStadiumConfig(const char* id4)
    {
        BenchSlotConfig defaults[2][7] = {};
        std::memcpy(gBenchSlots,defaults,sizeof(gBenchSlots));
        gBenchHasPositionRequests=false;
        if (!id4 || !*id4) return;
        char filename[MAX_PATH]={};
        sprintf_s(filename,"assets\\stadia\\%s.json",id4);
        std::ifstream file(filename,std::ios::binary);
        if (!file.good()) return;
        std::ostringstream buffer; buffer<<file.rdbuf();
        const std::string json=buffer.str();
        if (json.size()>1048576) {OutputDebugStringA("[CustomPaths] stadium JSON too large for bench settings; ignored.\n");return;}
        BenchSlotConfig proposed[2][7] = {};
        bool positionRequests=false;
        BenchJson reader(json);
        if (!reader.Parse(proposed,positionRequests)) {
            OutputDebugStringA("[CustomPaths] Invalid stadium JSON or benchPlayers; original bench settings retained.\n"); return;
        }
        std::memcpy(gBenchSlots,proposed,sizeof(gBenchSlots));
        gBenchHasPositionRequests=positionRequests;
        OutputDebugStringA("[CustomPaths] Read benchPlayers from assets\\stadia\\<id4>.json.\n");
        if (positionRequests)
            OutputDebugStringA("[CustomPaths] Bench position offsets loaded; experimental pre-propagation root transform hook enabled.\n");
    }

#if defined(_M_IX86)
    static uintptr_t gBenchStartContinue=0x005ED0C9;
    static uintptr_t gBenchReplayContinue=0x005ED45F;
    // This is the ANIMATION controller address, not an actor transform.
    int __stdcall ResolveBenchPose(int original, uintptr_t controller, int slot)
    {
        if (slot<0||slot>=7) return original;
        __try {
            const uintptr_t root=*reinterpret_cast<const uintptr_t*>(0x00D20640);
            if (!root) return original;
            int side=-1;
            const uintptr_t home=*reinterpret_cast<const uintptr_t*>(root+0x164);
            const uintptr_t away=*reinterpret_cast<const uintptr_t*>(root+0x354);
            if (controller==home+static_cast<uintptr_t>(slot)*0x1100+0x60) side=0;
            else if (controller==away+static_cast<uintptr_t>(slot)*0x1100+0x60) side=1;
            if (side<0) return original;
            const BenchPose pose=gBenchSlots[side][slot].pose;
            if (pose==BENCH_NATIVE) return original;
            return pose==BENCH_SEATED ? 1 : 7;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return original;}
    }
    __declspec(naked) void BenchStartPoseHook()
    {
        __asm {
            pushfd
            pushad
            push edi             // zero-based slot
            push esi             // animation controller
            push eax             // native animation argument
            call ResolveBenchPose // __stdcall cleans 12 bytes
            mov [esp+28],eax    // replace PUSHAD's saved EAX
            popad
            popfd
            push eax            // stolen PUSH EAX
            mov ecx,esi         // stolen MOV ECX,ESI
            mov byte ptr [esi+8],0 // stolen instruction
            jmp dword ptr [gBenchStartContinue]
        }
    }
    __declspec(naked) void BenchReplayPoseHook()
    {
        __asm {
            pushfd
            pushad
            push ebp
            push edi
            push eax
            call ResolveBenchPose
            mov [esp+28],eax
            popad
            popfd
            mov edx,[edi]       // stolen MOV EDX,[EDI]
            push 40000000h      // stolen PUSH 2.0f
            jmp dword ptr [gBenchReplayContinue]
        }
    }
    bool InstallBenchPoseBranch(unsigned int address,const unsigned char* bytes,
                                size_t length,void* hook)
    {
        if (memcmp(reinterpret_cast<void*>(address),bytes,length)!=0) return false;
        DWORD old=0;
        if (!VirtualProtect(reinterpret_cast<void*>(address),length,PAGE_EXECUTE_READWRITE,&old)) return false;
        unsigned char patchBytes[8]={0xE9};
        const int relative=static_cast<int>(reinterpret_cast<uintptr_t>(hook)-address-5);
        memcpy(patchBytes+1,&relative,4);
        for (size_t i=5;i<length;++i) patchBytes[i]=0x90;
        memcpy(reinterpret_cast<void*>(address),patchBytes,length);
        DWORD unused=0; VirtualProtect(reinterpret_cast<void*>(address),length,old,&unused);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(address),length);
        return true;
    }
    // Experimental v4: shift the native player root BEFORE sub_603680
    // propagates it to render matrices and attached scene geodes.
    // The prior v3 hook only translated selected output matrices, leaving
    // independently-rendered headband/hair/geode attachments behind.
    using BenchFactoryFn = void (__cdecl *)(uintptr_t, uintptr_t, uintptr_t,
        uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    static const BenchFactoryFn kBenchFactory = reinterpret_cast<BenchFactoryFn>(0x00605A20);

    void __cdecl BenchFactoryCapture(uintptr_t record, uintptr_t name,
        uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5,
        uintptr_t a6, uintptr_t a7, uintptr_t a8)
    {
        kBenchFactory(record,name,a2,a3,a4,a5,a6,a7,a8);
        if (!record || !name) return;
        const char* path = reinterpret_cast<const char*>(name);
        const char* home = "scene/homeTeam/bench_";
        const char* away = "scene/awayTeam/bench_";
        int side=-1; const char* suffix=nullptr;
        if (strncmp(path,home,strlen(home))==0) {side=0;suffix=path+strlen(home);}
        else if (strncmp(path,away,strlen(away))==0) {side=1;suffix=path+strlen(away);}
        if (side>=0 && suffix && suffix[0]>='1' && suffix[0]<='7' && suffix[1]==0)
            gBenchActorRecords[side][suffix[0]-'1']=record;
    }

    // At sub_603680+0x28, the virtual transform getter has just filled
    // [record+0x4F0+0x30], the 4x4 root matrix. Here "matrix" points to
    // the getter's output destination (transform+0x30), NOT to the 0x4F0 object.
    void __stdcall BenchApplyRootOffset(uintptr_t matrix)
    {
        if (!matrix || !gBenchHasPositionRequests) return;
        for (int side=0;side<2;++side) for (int slot=0;slot<7;++slot) {
            const uintptr_t record=gBenchActorRecords[side][slot];
            if (!record || matrix!=record+0x4F0+0x30) continue;
            const BenchSlotConfig& config=gBenchSlots[side][slot];
            if (!config.hasOffset) return;
            __try {
                float* translation=reinterpret_cast<float*>(matrix+0x30);
                translation[0]+=config.offset[0];
                translation[1]+=config.offset[1];
                translation[2]+=config.offset[2];
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                OutputDebugStringA("[CustomPaths] Pre-propagation bench root offset failed.\n");
            }
            return;
        }
    }

    // Replaces exactly six original bytes at 0x006036A8:
    // FF 50 08   call dword ptr [eax+8]
    // 8A 45 0C   mov al, [ebp+arg_4]
    // The virtual method consumes its one stack argument (no caller ADD ESP).
    // Duplicate the original getter argument from the stack top, leave the
    // original copy available for our helper, then discard it and jump
    // to the first untouched native instruction (0x006036AE).
    // All registers except the native MOV AL result are preserved around helper.
    static uintptr_t gBenchRootContinue = 0x006036AE;
    __declspec(naked) void BenchRootTransformHook()
    {
        __asm {
            push dword ptr [esp]     // duplicate original getter output pointer
            call dword ptr [eax+8]  // native getter fills root matrix; pops duplicate
            pushfd
            pushad
            mov eax,[esp+36]        // PUSHFD (4) + PUSHAD (32)
            push eax
            call BenchApplyRootOffset // stdcall pops its pointer
            popad
            popfd
            add esp,4                // discard the original getter argument
            mov al,[ebp+0Ch]        // stolen MOV AL,[EBP+arg_4]
            jmp dword ptr [gBenchRootContinue] // patched site entered via JMP, not CALL
        }
    }

    static bool BenchValidateRelativeCall(uintptr_t at,uintptr_t expected)
    {
        if (*reinterpret_cast<const uint8_t*>(at)!=0xE8) return false;
        const int32_t relative=*reinterpret_cast<const int32_t*>(at+1);
        return static_cast<uintptr_t>(at+5+relative)==expected;
    }

    void InstallLive06BenchPositionHooks()
    {
        static const uintptr_t factoryCalls[]={
            0x0061E60F,0x0061E658,0x0061E6A1,0x0061E6EA,
            0x0061E733,0x0061E77C,0x0061E7C5,
            0x0061EB3E,0x0061EB87,0x0061EBD0,0x0061EC19,
            0x0061EC62,0x0061ECAB,0x0061ECF4
        };
        for (uintptr_t call:factoryCalls) if (!BenchValidateRelativeCall(call,0x00605A20)) {
            OutputDebugStringA("[CustomPaths] Bench factory validation failed; offsets disabled.\n");return;
        }
        const unsigned char originalRootCall[6]={0xFF,0x50,0x08,0x8A,0x45,0x0C};
        if (memcmp(reinterpret_cast<const void*>(0x006036A8),originalRootCall,6)!=0) {
            OutputDebugStringA("[CustomPaths] Native root transform instructions differ; offsets disabled.\n");return;
        }
        // Install identity-capture hooks before enabling the shared transform hook.
        for (uintptr_t call:factoryCalls) patch::RedirectCall(call,BenchFactoryCapture);
        if (!InstallBenchPoseBranch(0x006036A8,originalRootCall,6,
                                   reinterpret_cast<void*>(BenchRootTransformHook))) {
            OutputDebugStringA("[CustomPaths] Failed to install pre-propagation root hook.\n");return;
        }
        OutputDebugStringA("[CustomPaths] Experimental Live06 bench root transform v4 installed.\n");
    }

    void InstallLive06BenchPoseHooks()
    {
        // Two validated, whole-instruction branch sites. Hooking the shared
        // virtual method would also affect unrelated actors, so do not.
        const unsigned char start[7]={0x50,0x8B,0xCE,0xC6,0x46,0x08,0x00};
        const unsigned char replay[7]={0x8B,0x17,0x68,0x00,0x00,0x00,0x40};
        if (memcmp(reinterpret_cast<const void*>(0x005ED0C2),start,7)!=0 ||
            memcmp(reinterpret_cast<const void*>(0x005ED458),replay,7)!=0) {
            OutputDebugStringA("[CustomPaths] Bench hook bytes differ; no hooks installed.\n");
            return;
        }
        const bool a=InstallBenchPoseBranch(0x005ED0C2,start,7,reinterpret_cast<void*>(BenchStartPoseHook));
        const bool b=InstallBenchPoseBranch(0x005ED458,replay,7,reinterpret_cast<void*>(BenchReplayPoseHook));
        if (!a||!b) OutputDebugStringA("[CustomPaths] Bench hook validation failed. Use original unmodified NBA Live 06 EXE.\n");
    }
#endif

    enum OverrideSlot
    {
        SLOT_STAGE_COURT,
        SLOT_STAGE_BACKBOARD,
        SLOT_STAGE_DORNA,
        SLOT_PLAYOFF_COURT,
        SLOT_PLAYOFF_BACKBOARD,
        SLOT_PLAYOFF_DORNA,
        SLOT_REGULAR_COURT,
        SLOT_REGULAR_BACKBOARD,
        SLOT_REGULAR_DORNA,
        SLOT_COUNT
    };

    enum PostseasonStage
    {
        STAGE_REGULAR,
        STAGE_PLAYOFF,
        STAGE_ECF,
        STAGE_WCF,
        STAGE_FINALS,
        STAGE_ECSF,
        STAGE_WCSF
    };

    enum ForcedNativeGameMode
    {
        NATIVE_MODE_OFF,
        NATIVE_MODE_ALLSTAR,
        NATIVE_MODE_ROOKIE
    };

    const size_t kOverridePathCapacity = 96;
    const size_t kOverrideCacheCapacity = 256;
    const size_t kTournamentChildCandidateCount = 2;
    const size_t kTournamentChildSnapshotCapacity = 0x854;

    // These paths are never registered with the game's global filesystem.
    // They are only consulted by the direct OpenResolved override hook.
    char gOverridePaths[SLOT_COUNT][kOverridePathCapacity] = {};

    struct OverrideCacheEntry
    {
        char logicalName[128];
        char resolvedPath[MAX_PATH];
        bool found;
    };

    struct TournamentChildSnapshot
    {
        void* pointer;
        unsigned char bytes[kTournamentChildSnapshotCapacity];
        bool valid;
    };

    OverrideCacheEntry gOverrideCache[kOverrideCacheCapacity] = {};
    unsigned int gOverrideCacheCount = 0;
    unsigned int gOverrideCacheCursor = 0;
    bool gGameContextValid = false;
    char gCurrentStadiumId4[5] = {};
    PostseasonStage gCurrentStage = STAGE_REGULAR;
    bool gForcedStageEnabled = false;
    PostseasonStage gForcedStage = STAGE_REGULAR;
    bool gForceGlobalGameMode = false;
    bool gForcedSeriesEnabled = false;
    int gForcedSeriesGame = 0;
    int gForcedSeriesLength = 7;
    int gForcedHomeWins = 0;
    int gForcedAwayWins = 0;
    ForcedNativeGameMode gForcedNativeGameMode = NATIVE_MODE_OFF;
    bool gPlayoffStateDebug = false;
    bool gLoggedMissingGlobalOptions = false;
    bool gLoggedMissingNativeModeOptions = false;
    bool gLoggedUnsupportedNativeMode = false;
    void* gLastPlayoffSnapshotOptions = nullptr;
    unsigned int gLastPlayoffSnapshotHash = 0;
    bool gRetryLive06PlayoffSeries = false;
    bool gLoggedForcedScheduleGetter = false;
    bool gLoggedForcedScheduleSelection = false;
    unsigned char gPreviousOptionsSnapshot[0x200] = {};
    bool gPreviousOptionsSnapshotValid = false;
    void* gLastRejectedOptions = nullptr;
    int gLastRejectedMode = 0;
    int gLastRejectedRound = 0;
    FILE* gPlayoffStateLogFile = nullptr;
    bool gLastTournamentSnapshotValid = false;
    unsigned int gLastTournamentSnapshotHash = 0;
    int gLastTournamentGameMode = -1;
    unsigned int gPlayoffSnapshotSequence = 0;
    unsigned int gLastTournamentPlayoffSequence = 0;
    unsigned int gTournamentSnapshotSequence = 0;
    TournamentChildSnapshot
        gTournamentChildSnapshots[kTournamentChildCandidateCount] = {};
    const size_t kLive06PlayoffBracketSlotCount = 30;
    unsigned char
        gLive06PlayoffBracketTeams[kLive06PlayoffBracketSlotCount] = {};
    short gLive06PlayoffBracketWins[kLive06PlayoffBracketSlotCount] = {};
    bool gLive06PlayoffBracketCacheValid = false;
    void* gLive06CapturedPlayoffBracket = nullptr;

    struct PlayoffAccessorTrace
    {
        void* settings;
        uintptr_t caller;
        unsigned int id;
        unsigned int source;
        int value;
        bool isBoolean;
    };

    const size_t kPlayoffAccessorTraceCapacity = 256;
    PlayoffAccessorTrace
        gPlayoffAccessorTraces[kPlayoffAccessorTraceCapacity] = {};
    unsigned int gPlayoffAccessorTraceCount = 0;
    unsigned int gPlayoffAccessorTraceCursor = 0;

    struct CommentaryParameterTrace
    {
        uintptr_t caller;
        unsigned int index;
        int value;
        bool write;
    };

    const size_t kCommentaryParameterTraceCapacity = 256;
    CommentaryParameterTrace
        gCommentaryParameterTraces[kCommentaryParameterTraceCapacity] = {};
    unsigned int gCommentaryParameterTraceCount = 0;
    unsigned int gCommentaryParameterTraceCursor = 0;

    unsigned int gLive06IntroArguments[36] = {};
    bool gLive06IntroArgumentsValid = false;
    unsigned int gLive06IntroTraceCount = 0;

    GameVersion gGameVersion = GAME_UNKNOWN;

#if defined(_M_IX86)
    // Resolve custom stadium files directly at OpenResolved entry. The nine
    // directories are no longer registered as global search paths, so normal
    // assets pay no custom-path enumeration cost.
    uintptr_t gOpenResolvedContinuation = 0;
    bool gOpenResolvedGateInstalled = false;
#endif

    const unsigned int kLive08GameCourtNameCalls[] = {
        0x0066F677, 0x00670EC2, 0x00671532, 0x00671F12,
        0x0067671B, 0x0067743D, 0x006780D3, 0x00678613
    };
    const unsigned int kLive08GameCourtSetStadiumName = 0x0069EFC0;

    const unsigned int kLive06ScheduleSelectionCalls[] = {
        0x00515DFB, 0x00515F6B, 0x00530BDF,
        0x00530DC3, 0x0055E61F, 0x005A6FEA,
        0x005AE3F6, 0x005AE6B4, 0x005BD225
    };

    template <unsigned int GetterAddress, unsigned int AddPathAddress>
    DWORD* AddCustomPathsForBuild()
    {
        DWORD* fsm = CallAndReturn<DWORD*, GetterAddress>();

        // Faces remains a normal global search path. Stadium overrides are
        // handled directly and are deliberately absent from this list.
        CallMethod<AddPathAddress>(fsm, "faces\\");
        return fsm;
    }

    static DWORD* AddCustomPaths05()
    {
        return AddCustomPathsForBuild<0x429E10, 0x429E90>();
    }

    static DWORD* AddCustomPaths06()
    {
        return AddCustomPathsForBuild<0x429F50, 0x429FD0>();
    }

    static DWORD* AddCustomPaths07()
    {
        return AddCustomPathsForBuild<0x44DCA0, 0x44DD20>();
    }

    static DWORD* AddCustomPaths08()
    {
        return AddCustomPathsForBuild<0x450500, 0x450580>();
    }

    bool IsValidId4(const char* value)
    {
        if (!value)
            return false;

        for (int i = 0; i < 4; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == '\0' || !std::isalnum(c))
                return false;
        }
        return value[4] == '\0';
    }

    void NormalizeId4(const char* source, char* destination)
    {
        for (int i = 0; i < 4; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(source[i]);
            destination[i] = static_cast<char>(std::tolower(c));
        }
        destination[4] = '\0';
    }

    void* GetCurrentOptions(void* suppliedOptions)
    {
        if (suppliedOptions)
            return suppliedOptions;

        __try
        {
            if (gGameVersion == GAME_LIVE_2005)
            {
                unsigned char* audio = *reinterpret_cast<unsigned char**>(0x00C44A30);
                return audio ? *reinterpret_cast<void**>(audio + 0x28) : nullptr;
            }
            if (gGameVersion == GAME_LIVE_07)
            {
                void* game = *reinterpret_cast<void**>(0x00CEBBB0);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0AC0)
                    : nullptr;
            }

            if (gGameVersion == GAME_LIVE_08)
            {
                void* game = *reinterpret_cast<void**>(0x00DA7A20);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0B70)
                    : nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        return nullptr;
    }

    const char* StageName(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_PLAYOFF: return "playoff";
        case STAGE_ECSF:    return "ECSF";
        case STAGE_WCSF:    return "WCSF";
        case STAGE_ECF:     return "ECF";
        case STAGE_WCF:     return "WCF";
        case STAGE_FINALS:  return "finals";
        default:            return "regular";
        }
    }

    const char* ForcedNativeGameModeName(ForcedNativeGameMode mode)
    {
        switch (mode)
        {
        case NATIVE_MODE_ALLSTAR: return "ALLSTAR";
        case NATIVE_MODE_ROOKIE:  return "ROOKIE";
        default:                  return "OFF";
        }
    }

    int StageRoundValue(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_ECSF:   return 2;
        case STAGE_WCSF:   return 3;
        case STAGE_ECF:    return 4;
        case STAGE_WCF:    return 5;
        case STAGE_FINALS: return 6;
        default:           return 0;
        }
    }

    const char* GameVersionName()
    {
        switch (gGameVersion)
        {
        case GAME_LIVE_2005: return "2005";
        case GAME_LIVE_06:   return "06";
        case GAME_LIVE_07:   return "07";
        case GAME_LIVE_08:   return "08";
        default:             return "unknown";
        }
    }

    void InitializePlayoffStateLog()
    {
        if (!gPlayoffStateDebug || gPlayoffStateLogFile)
            return;

        if (!CreateDirectoryA("logs", nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS)
            {
                OutputDebugStringA(
                    "[CustomPaths] Could not create the logs directory.\n");
                return;
            }
        }

        if (fopen_s(
                &gPlayoffStateLogFile,
                "logs\\playoff_state.log",
                "a") != 0 ||
            !gPlayoffStateLogFile)
        {
            gPlayoffStateLogFile = nullptr;
            OutputDebugStringA(
                "[CustomPaths] Could not open logs\\playoff_state.log.\n");
            return;
        }

        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf_s(
            gPlayoffStateLogFile,
            "\n============================================================\n"
            "Playoff-state session: %04u-%02u-%02u %02u:%02u:%02u "
            "NBA Live %s\n"
            "============================================================\n",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            GameVersionName());
        fflush(gPlayoffStateLogFile);

        OutputDebugStringA(
            "[CustomPaths] Playoff state log: "
            "logs\\playoff_state.log.\n");
    }

    void WritePlayoffStateLog(const char* message)
    {
        if (!message)
            return;

        OutputDebugStringA(message);
        if (!gPlayoffStateLogFile)
            return;

        fputs(message, gPlayoffStateLogFile);
        fflush(gPlayoffStateLogFile);
    }

    struct Live06ScheduleGame
    {
        unsigned char bytes[44];
    };

    short ReadScheduleShort(
        const Live06ScheduleGame& game,
        size_t offset)
    {
        short value = 0;
        std::memcpy(&value, game.bytes + offset, sizeof(value));
        return value;
    }

    bool ReadLive06ScheduleGame(
        unsigned int gameId,
        Live06ScheduleGame& game)
    {
        if (gGameVersion != GAME_LIVE_06 ||
            gameId == 0 ||
            gameId > 0x7FFF)
        {
            return false;
        }

        // Native helper used by ScheduleInfo.GetGameInfoArray. It queries the
        // already-open schedule table by ID and returns the game's normalized
        // 44-byte record, so no DBF path discovery or extra disk I/O is needed.
        typedef void* (__stdcall* ReadScheduleGameFn)(
            void* output,
            short period,
            int id);
        ReadScheduleGameFn readScheduleGame =
            reinterpret_cast<ReadScheduleGameFn>(0x00552F20);

        std::memset(&game, 0, sizeof(game));
        __try
        {
            // PERIOD=2 is the playoff schedule, as confirmed by
            // scheduleplayoff.dbf.
            readScheduleGame(
                &game,
                2,
                static_cast<int>(gameId));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_QUERY id=%u period=2 "
                "exception=yes.\n",
                gameId);
            WritePlayoffStateLog(message);
            return false;
        }

        const short returnedId = ReadScheduleShort(game, 0);
        if (returnedId == static_cast<short>(gameId))
            return true;

        const unsigned int* raw =
            reinterpret_cast<const unsigned int*>(game.bytes);
        char message[320] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_QUERY id=%u period=2 returnedId=%d "
            "played=%u home=%d away=%d score=%d-%d "
            "raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
            gameId,
            returnedId,
            game.bytes[18],
            ReadScheduleShort(game, 20),
            ReadScheduleShort(game, 26),
            ReadScheduleShort(game, 22),
            ReadScheduleShort(game, 28),
            raw[0],
            raw[1],
            raw[2],
            raw[3],
            raw[4],
            raw[5]);
        WritePlayoffStateLog(message);
        return false;
    }

    bool IsReadableMemoryRange(const void* address, size_t size)
    {
        if (!address || size == 0)
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = start + size;
        if (end < start)
            return false;

        uintptr_t cursor = start;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION information = {};
            if (!VirtualQuery(
                    reinterpret_cast<const void*>(cursor),
                    &information,
                    sizeof(information)))
            {
                return false;
            }

            if (information.State != MEM_COMMIT ||
                (information.Protect & PAGE_GUARD) != 0 ||
                (information.Protect & PAGE_NOACCESS) != 0)
            {
                return false;
            }

            const uintptr_t regionStart =
                reinterpret_cast<uintptr_t>(information.BaseAddress);
            const uintptr_t regionEnd = regionStart + information.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = regionEnd;
        }

        return true;
    }

    bool AddMemoryToHash(
        const void* address,
        size_t size,
        unsigned int& hash)
    {
        if (!IsReadableMemoryRange(address, size))
            return false;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(address);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 16777619u;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void LogRawMemoryBlock(
        const char* label,
        const void* address,
        size_t size)
    {
        if (!label || !IsReadableMemoryRange(address, size))
            return;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(address);
            char message[256] = {};
            for (size_t offset = 0; offset < size; offset += 16)
            {
                const unsigned int* values =
                    reinterpret_cast<const unsigned int*>(bytes + offset);
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] %s +%04X: %08X %08X %08X %08X\n",
                    label,
                    static_cast<unsigned int>(offset),
                    values[0],
                    values[1],
                    values[2],
                    values[3]);
                WritePlayoffStateLog(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Tournament raw-memory read failed.\n");
        }
    }

    void LogLive06PlayoffSettings(const void* settings)
    {
        // NBA Live 06 SettingsObject is 0x428 bytes. The executable's
        // SettingsObject accessors use the bool array at +0x7E and integer
        // array at +0xBC. The IDs below come directly from the game's
        // registered SETTINGS_* enum names. These are setup/configuration
        // values; the live series results are owned by the bracket object.
        const size_t settingsSize = 0x428;
        if (!IsReadableMemoryRange(settings, settingsSize))
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETTINGS readable=no.\n");
            return;
        }

        const unsigned int awayLostId = 0x0B;
        const unsigned int awayWonId = 0x0C;
        const unsigned int homeLostId = 0x23;
        const unsigned int homeWonId = 0x24;
        const unsigned int awayCouldWinId = 0x2A;
        const unsigned int homeCouldWinId = 0x2B;
        const unsigned int seriesLengthId = 0x3B;
        const unsigned int roundId = 0x3C;
        const unsigned int seriesTypeId = 0x3D;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(settings);
            char message[512] = {};

            const unsigned int boolBase = 0x7E;
            const unsigned int intBase = 0xBC;
            const int awayLost = *reinterpret_cast<const int*>(
                bytes + intBase + awayLostId * sizeof(int));
            const int awayWon = *reinterpret_cast<const int*>(
                bytes + intBase + awayWonId * sizeof(int));
            const int homeLost = *reinterpret_cast<const int*>(
                bytes + intBase + homeLostId * sizeof(int));
            const int homeWon = *reinterpret_cast<const int*>(
                bytes + intBase + homeWonId * sizeof(int));
            const int seriesLength = *reinterpret_cast<const int*>(
                bytes + intBase + seriesLengthId * sizeof(int));
            const int round = *reinterpret_cast<const int*>(
                bytes + intBase + roundId * sizeof(int));
            const int seriesType = *reinterpret_cast<const int*>(
                bytes + intBase + seriesTypeId * sizeof(int));
            const unsigned int awayCouldWin =
                bytes[boolBase + awayCouldWinId] ? 1u : 0u;
            const unsigned int homeCouldWin =
                bytes[boolBase + homeCouldWinId] ? 1u : 0u;

            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_CONFIG "
                "awayW=%d awayL=%d homeW=%d homeL=%d "
                "awayCouldWin=%u homeCouldWin=%u "
                "length=%d round=%d seriesType=%d\n",
                awayWon,
                awayLost,
                homeWon,
                homeLost,
                awayCouldWin,
                homeCouldWin,
                seriesLength,
                round,
                seriesType);
            WritePlayoffStateLog(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETTINGS read failed.\n");
        }
    }

    void LogLive06PlayoffBracket(const void* bracket)
    {
        // The persistent playoff tree contains 30 nodes: 16 first-round
        // teams, 8 conference-semifinal teams, 4 conference-final teams and
        // 2 Finals teams. Native function 0x004F8820 reads the consecutive
        // team bytes at +0x04 and 0x004F8730 writes the corresponding signed
        // 16-bit win counts at +0x22. Unlike the temporary records at +0x3BC,
        // these arrays remain valid after the game-launch transition.
        const size_t bracketSize = 0x4BC;
        const bool bracketReadable =
            IsReadableMemoryRange(bracket, bracketSize);
        if (!bracketReadable && !gLive06PlayoffBracketCacheValid)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET readable=no.\n");
            return;
        }

        __try
        {
            const unsigned char* teams = gLive06PlayoffBracketTeams;
            const short* wins = gLive06PlayoffBracketWins;
            if (bracketReadable)
            {
                const unsigned char* bytes =
                    reinterpret_cast<const unsigned char*>(bracket);
                teams = bytes + 0x04;
                wins = reinterpret_cast<const short*>(bytes + 0x22);
            }
            else
            {
                WritePlayoffStateLog(
                    "[CustomPaths] PLAYOFF_BRACKET source=cached-before-"
                    "destruction\n");
            }
            char message[192] = {};

            for (size_t i = 0;
                 i < kLive06PlayoffBracketSlotCount;
                 ++i)
            {
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_BRACKET_SLOT slot=%02u "
                    "team=%u wins=%d\n",
                    static_cast<unsigned int>(i),
                    static_cast<unsigned int>(teams[i]),
                    static_cast<int>(wins[i]));
                WritePlayoffStateLog(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET persistent read failed.\n");
        }
    }

    void CaptureLive06PlayoffBracket(const void* bracket)
    {
        if (!gPlayoffStateDebug ||
            !IsReadableMemoryRange(bracket, 0x4BC))
        {
            return;
        }

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(bracket);
            std::memcpy(
                gLive06PlayoffBracketTeams,
                bytes + 0x04,
                sizeof(gLive06PlayoffBracketTeams));
            std::memcpy(
                gLive06PlayoffBracketWins,
                bytes + 0x22,
                sizeof(gLive06PlayoffBracketWins));
            gLive06CapturedPlayoffBracket = const_cast<void*>(bracket);
            gLive06PlayoffBracketCacheValid = true;

            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_BRACKET_CAPTURE bracket=%08X "
                "slots=%u\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(bracket)),
                static_cast<unsigned int>(
                    kLive06PlayoffBracketSlotCount));
            WritePlayoffStateLog(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET capture failed.\n");
        }
    }

    void __fastcall Live06PlayoffBracketDestructorHook(
        void* bracket,
        void*)
    {
        CaptureLive06PlayoffBracket(bracket);
        CallMethodDynGlobal(0x0051E700, bracket);
    }

    bool IsLive06PlayoffIntegerSetting(unsigned int id)
    {
        return id == 0x0B || id == 0x0C ||
            id == 0x23 || id == 0x24 ||
            id == 0x3B || id == 0x3C || id == 0x3D;
    }

    bool IsLive06PlayoffBooleanSetting(unsigned int id)
    {
        return id == 0x2A || id == 0x2B;
    }

    const char* Live06PlayoffSettingName(unsigned int id)
    {
        switch (id)
        {
        case 0x0B: return "AWAY_LOST";
        case 0x0C: return "AWAY_WON";
        case 0x23: return "HOME_LOST";
        case 0x24: return "HOME_WON";
        case 0x2A: return "AWAY_COULD_WIN";
        case 0x2B: return "HOME_COULD_WIN";
        case 0x3B: return "LENGTH";
        case 0x3C: return "ROUND";
        case 0x3D: return "SERIES_TYPE";
        default:   return "UNKNOWN";
        }
    }

    void LogLive06PlayoffAccessor(
        void* settings,
        uintptr_t caller,
        unsigned int id,
        unsigned int source,
        int value,
        bool isBoolean)
    {
        if (!gPlayoffStateDebug)
            return;

        for (unsigned int i = 0;
             i < gPlayoffAccessorTraceCount;
             ++i)
        {
            const PlayoffAccessorTrace& trace =
                gPlayoffAccessorTraces[i];
            if (trace.settings == settings &&
                trace.caller == caller &&
                trace.id == id &&
                trace.source == source &&
                trace.value == value &&
                trace.isBoolean == isBoolean)
            {
                return;
            }
        }

        PlayoffAccessorTrace& trace =
            gPlayoffAccessorTraces[gPlayoffAccessorTraceCursor];
        trace.settings = settings;
        trace.caller = caller;
        trace.id = id;
        trace.source = source;
        trace.value = value;
        trace.isBoolean = isBoolean;

        if (gPlayoffAccessorTraceCount < kPlayoffAccessorTraceCapacity)
            ++gPlayoffAccessorTraceCount;
        gPlayoffAccessorTraceCursor =
            (gPlayoffAccessorTraceCursor + 1) %
            kPlayoffAccessorTraceCapacity;

        char message[256] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_ACCESS type=%s name=%s id=%02X "
            "value=%d source=%u settings=%08X caller=%08X\n",
            isBoolean ? "bool" : "int",
            Live06PlayoffSettingName(id),
            id,
            value,
            source,
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(settings)),
            static_cast<unsigned int>(caller));
        WritePlayoffStateLog(message);
    }

    int __fastcall Live06IntegerSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source)
    {
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        int value = 0;

        switch (source)
        {
        case 1:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C4460)(id);
            break;
        case 2:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C4610)(id);
            break;
        case 3:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C46C0)(id);
            break;
        default:
            __try
            {
                value = *reinterpret_cast<const int*>(
                    reinterpret_cast<const unsigned char*>(settings) +
                    0xBC + id * sizeof(int));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                value = 0;
            }
            break;
        }

        if (gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
            // These are the presentation-facing SettingsObject values.  In
            // an exhibition promoted to Playoff mode there is no live
            // schedule/bracket object, so callers otherwise keep receiving
            // the default "round one, game one" state.
            switch (id)
            {
            case 0x0B: value = gForcedHomeWins; break; // away losses
            case 0x0C: value = gForcedAwayWins; break; // away wins
            case 0x23: value = gForcedAwayWins; break; // home losses
            case 0x24: value = gForcedHomeWins; break; // home wins
            case 0x3B: value = (gForcedSeriesLength - 1) / 2; break;
            case 0x3C: value = StageRoundValue(gForcedStage); break;
            default: break;
            }
        }

        if (IsLive06PlayoffIntegerSetting(id))
        {
            LogLive06PlayoffAccessor(
                settings,
                caller,
                id,
                source,
                value,
                false);
        }
        return value;
    }

    bool __fastcall Live06BooleanSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source)
    {
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        bool value = false;

        switch (source)
        {
        case 1:
            value = reinterpret_cast<bool (__stdcall*)(unsigned int)>(
                0x004C45A0)(id);
            break;
        case 2:
            value = false;
            break;
        case 3:
            value = true;
            break;
        default:
            __try
            {
                value = *reinterpret_cast<const unsigned char*>(
                    reinterpret_cast<const unsigned char*>(settings) +
                    0x7E + id) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                value = false;
            }
            break;
        }

        if (gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
            if (id == 0x2A)
                value = gForcedAwayWins == (gForcedSeriesLength - 1) / 2;
            else if (id == 0x2B)
                value = gForcedHomeWins == (gForcedSeriesLength - 1) / 2;
        }

        if (IsLive06PlayoffBooleanSetting(id))
        {
            LogLive06PlayoffAccessor(
                settings,
                caller,
                id,
                source,
                value ? 1 : 0,
                true);
        }
        return value;
    }

    void LogLive06CommentaryParameter(
        uintptr_t caller,
        unsigned int index,
        int value,
        bool write)
    {
        if (!gPlayoffStateDebug)
            return;

        for (unsigned int i = 0;
             i < gCommentaryParameterTraceCount;
             ++i)
        {
            const CommentaryParameterTrace& trace =
                gCommentaryParameterTraces[i];
            if (trace.caller == caller &&
                trace.index == index &&
                trace.value == value &&
                trace.write == write)
            {
                return;
            }
        }

        CommentaryParameterTrace& trace =
            gCommentaryParameterTraces[gCommentaryParameterTraceCursor];
        trace.caller = caller;
        trace.index = index;
        trace.value = value;
        trace.write = write;
        if (gCommentaryParameterTraceCount <
            kCommentaryParameterTraceCapacity)
        {
            ++gCommentaryParameterTraceCount;
        }
        gCommentaryParameterTraceCursor =
            (gCommentaryParameterTraceCursor + 1) %
            kCommentaryParameterTraceCapacity;

        char message[224] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_COMMENTARY_PARAM op=%s index=%u "
            "value=%d caller=%08X\n",
            write ? "write" : "read",
            index,
            value,
            static_cast<unsigned int>(caller));
        WritePlayoffStateLog(message);
    }

    int __stdcall Live06CommentaryParameterGetterHook(unsigned int index)
    {
        // Exact replacement for native 0x00449550. The original lazily fills
        // the 128-entry commentary parameter table with -1 before reading.
        unsigned char* initialized =
            reinterpret_cast<unsigned char*>(0x00CAF5B4);
        int* values = reinterpret_cast<int*>(0x00CAFA18);
        if (*initialized == 0)
        {
            for (unsigned int i = 0; i < 128; ++i)
                values[i] = -1;
            *initialized = 1;
        }

        const int value = index < 128 ? values[index] : -1;
        LogLive06CommentaryParameter(
            reinterpret_cast<uintptr_t>(_ReturnAddress()),
            index,
            value,
            false);
        return value;
    }

    void __stdcall Live06CommentaryParameterSetterHook(
        unsigned int index,
        int value)
    {
        // Exact replacement for native 0x00449590.
        unsigned char* initialized =
            reinterpret_cast<unsigned char*>(0x00CAF5B4);
        int* values = reinterpret_cast<int*>(0x00CAFA18);
        if (*initialized == 0)
        {
            for (unsigned int i = 0; i < 128; ++i)
                values[i] = -1;
            *initialized = 1;
        }

        if (index < 128)
            values[index] = value;
        LogLive06CommentaryParameter(
            reinterpret_cast<uintptr_t>(_ReturnAddress()),
            index,
            value,
            true);
    }

#if defined(_M_IX86)
    uintptr_t gLive06IntroEventTarget = 0x00434610;
    uintptr_t gLive06SpeechDispatchContinuation = 0;
    uintptr_t gLive06PregameContinuation = 0;
    uintptr_t gLive06PlayoffCacheInitializer = 0x0043C410;

    void ApplyForcedSeriesRulesForBuild(void* options)
    {
        if (!options || !gForcedSeriesEnabled || !gForcedStageEnabled ||
            !gForceGlobalGameMode)
            return;
        __try
        {
            unsigned char* rules = static_cast<unsigned char*>(options);
            const bool live08 = gGameVersion == GAME_LIVE_08;
            const size_t modeOffset = live08 ? 0x64 : 0x5C;
            const size_t roundOffset = live08 ? 0x168 : 0x158;
            const size_t lengthOffset = live08 ? 0x170 : 0x160;
            const size_t awayOffset = live08 ? 0x11D : 0x10D;
            const size_t homeOffset = live08 ? 0x11E : 0x10E;
            if (!IsReadableMemoryRange(rules, lengthOffset + sizeof(int)))
                return;
            int& mode = *reinterpret_cast<int*>(rules + modeOffset);
            if (mode < 0 || mode > 24)
                return;
            mode = mode == 4 ? 4 : 2;
            *reinterpret_cast<int*>(rules + roundOffset) = StageRoundValue(gForcedStage);
            *reinterpret_cast<int*>(rules + lengthOffset) = (gForcedSeriesLength - 1) / 2;
            rules[awayOffset] = gForcedAwayWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            rules[homeOffset] = gForcedHomeWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            if (gGameVersion == GAME_LIVE_2005)
            {
                // Native 2005 rules copies: 892033/89203F and 5DCF15/5DCF1E.
                *reinterpret_cast<unsigned char*>(0x00CBE934) = rules[homeOffset];
                *reinterpret_cast<unsigned char*>(0x00CBE935) = rules[awayOffset];
                unsigned char* info = *reinterpret_cast<unsigned char**>(0x00C49F04);
                if (IsReadableMemoryRange(info, 0x26))
                {
                    info[0x24] = rules[awayOffset];
                    info[0x25] = rules[homeOffset];
                }
                return;
            }
            if (live08)
            {
                // Native Live 08 rules copy at 0x009E421A..0x009E422C.
                *reinterpret_cast<unsigned char*>(0x00DF9C54) = rules[homeOffset];
                *reinterpret_cast<unsigned char*>(0x00DF9C55) = rules[awayOffset];
                return;
            }
            // Live 07 copies home/away clinch flags to bits 1/2 in the
            // gameplay rules object (native copy at 0x0092A327..0x0092A362).
            unsigned char* gameplayRules =
                *reinterpret_cast<unsigned char**>(0x00D3B188);
            if (IsReadableMemoryRange(gameplayRules, 0x58A))
                gameplayRules[0x589] = static_cast<unsigned char>(
                    (gameplayRules[0x589] & ~6u) |
                    (rules[0x10E] ? 2u : 0u) | (rules[0x10D] ? 4u : 0u));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog("[CustomPaths] Forced rules unreadable.\n");
        }
    }

    void __stdcall ApplyLive06ForcedSpeechContext()
    {
        if (!gForcedSeriesEnabled || !gForcedStageEnabled ||
            !gForceGlobalGameMode)
            return;
        int round = 1;
        int slot = 5; // Native default: Eastern first-round bracket slot.
        switch (gForcedStage)
        {
        case STAGE_FINALS: round = 4; slot = 1; break;
        case STAGE_ECSF: round = 2; slot = 3; break;
        case STAGE_WCSF: round = 2; slot = 1; break;
        case STAGE_ECF: round = 3; slot = 2; break;
        case STAGE_WCF: round = 3; slot = 1; break;
        case STAGE_PLAYOFF: break;
        default: return;
        }
        if (gGameVersion == GAME_LIVE_2005)
        {
            // 2005 builds its speech context from live query results below;
            // it has no 06/07/08 cached-round address to update.
            ApplyForcedSeriesRulesForBuild(GetCurrentOptions(nullptr));
            return;
        }
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        int* cachedRound = reinterpret_cast<int*>(live08 ? 0x00D137C4 : (live07 ? 0x00C567F4 : 0x00C3D234));
        int* cachedSlot = reinterpret_cast<int*>(live08 ? 0x00D137C8 : (live07 ? 0x00C567F8 : 0x00C3D238));
        int* cachedLength = reinterpret_cast<int*>(live08 ? 0x00D137CC : (live07 ? 0x00C567FC : 0x00C3D23C));
        int* cachedGame = reinterpret_cast<int*>(live08 ? 0x00D137D0 : (live07 ? 0x00C56800 : 0x00C3D240));
        if (live07 || live08)
        {
            ApplyForcedSeriesRulesForBuild(GetCurrentOptions(nullptr));
            __try
            {
                unsigned char* root = *reinterpret_cast<unsigned char**>(live08 ? 0x00DA3D08 : 0x00CE8378);
                if (IsReadableMemoryRange(root, 0x2C))
                    ApplyForcedSeriesRulesForBuild(*reinterpret_cast<void**>(root + 0x28));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        const bool changed = *cachedRound != round || *cachedSlot != slot ||
            *cachedLength != gForcedSeriesLength || *cachedGame != gForcedSeriesGame;
        *cachedRound = round;
        *cachedSlot = slot;
        *cachedLength = gForcedSeriesLength;
        *cachedGame = gForcedSeriesGame;
        // The two cached win counts are indexed by bracket participants,
        // rather than a verified home/away ordering. Equal forced counts
        // are independent of that ordering (including the Finals G7 test).
        if (gForcedHomeWins == gForcedAwayWins)
        {
            *reinterpret_cast<int*>(live08 ? 0x00D137D4 : (live07 ? 0x00C56804 : 0x00CAF5C8)) = gForcedHomeWins;
            *reinterpret_cast<int*>(live08 ? 0x00D137D8 : (live07 ? 0x00C56808 : 0x00CAF5CC)) = gForcedAwayWins;
        }
        if (changed && gPlayoffStateDebug)
        {
            char message[256] = {};
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SPEECH_CONTEXT game=%s stage=%s round=%d "
                "slot=%d length=%d game=%d winsApplied=%s\n",
                GameVersionName(), StageName(gForcedStage), round, slot, gForcedSeriesLength, gForcedSeriesGame,
                gForcedHomeWins == gForcedAwayWins ? "equal-counts" : "native-order-unverified");
            WritePlayoffStateLog(message);
        }
    }

    bool Live05ForcedSpeechEnabled()
    {
        return gGameVersion == GAME_LIVE_2005 && gForcedSeriesEnabled &&
            gForcedStageEnabled && gForceGlobalGameMode;
    }

    short* __fastcall Live05SpeechPlayoffQueryHook(
        void* object, void*, short* output)
    {
        typedef short* (__thiscall* QueryFn)(void*, short*);
        short* result = reinterpret_cast<QueryFn>(0x00539C40)(object, output);
        if (!Live05ForcedSpeechEnabled() || !output)
            return result;
        // The native 10-byte result is period, round, conference-local
        // bracket slot, series game, schedule ID. Preserve period and ID.
        int round = 1;
        int slot = 5;
        switch (gForcedStage)
        {
        case STAGE_ECSF: round = 2; slot = 3; break;
        case STAGE_WCSF: round = 2; slot = 1; break;
        case STAGE_ECF: round = 3; slot = 2; break;
        case STAGE_WCF: round = 3; slot = 1; break;
        case STAGE_FINALS: round = 4; slot = 1; break;
        case STAGE_PLAYOFF: break;
        default: return result;
        }
        output[1] = static_cast<short>(round);
        output[2] = static_cast<short>(slot);
        output[3] = static_cast<short>(gForcedSeriesGame);
        if (gPlayoffStateDebug)
        {
            static int previousRound = -1, previousSlot = -1, previousGame = -1;
            if (previousRound != round || previousSlot != slot ||
                previousGame != gForcedSeriesGame)
            {
                char message[256] = {};
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SPEECH_CONTEXT game=2005 stage=%s "
                    "round=%d slot=%d seriesGame=%d length=%d winsApplied=%s\n",
                    StageName(gForcedStage), round, slot, gForcedSeriesGame,
                    gForcedSeriesLength, gForcedHomeWins == gForcedAwayWins
                        ? "equal-counts" : "native-order-unverified");
                WritePlayoffStateLog(message);
                previousRound = round;
                previousSlot = slot;
                previousGame = gForcedSeriesGame;
            }
        }
        return result;
    }

    unsigned char* __fastcall Live05SpeechSeriesQueryHook(
        void* object, void*, unsigned char* output,
        unsigned int period, unsigned int roundAndSlot,
        unsigned int slotAndGame)
    {
        // Preserve the native hidden-result ABI: result pointer followed by
        // three packed arguments, with RET 10h at 0x004ECB25.
        typedef unsigned char* (__thiscall* QueryFn)(void*, unsigned char*,
            unsigned int, unsigned int, unsigned int);
        unsigned char* result = reinterpret_cast<QueryFn>(0x004EC880)(
            object, output, period, roundAndSlot, slotAndGame);
        if (!Live05ForcedSpeechEnabled() || !output)
            return result;
        // Native 54-byte series record: length +6, wins needed +8,
        // participant wins +0E/+10 (consumed by 44CE15..44CE2B).
        *reinterpret_cast<short*>(output + 6) = static_cast<short>(gForcedSeriesLength);
        *reinterpret_cast<short*>(output + 8) = static_cast<short>((gForcedSeriesLength + 1) / 2);
        if (gForcedHomeWins == gForcedAwayWins)
        {
            *reinterpret_cast<short*>(output + 0x0E) = static_cast<short>(gForcedHomeWins);
            *reinterpret_cast<short*>(output + 0x10) = static_cast<short>(gForcedAwayWins);
        }
        return result;
    }

    void __declspec(naked) Live06PlayoffCacheInitializerHook()
    {
        __asm
        {
            call dword ptr [gLive06PlayoffCacheInitializer]
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            call ApplyLive06ForcedSpeechContext
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            ret
        }
    }

    void __declspec(naked) Live06PregameContextHook()
    {
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            call ApplyLive06ForcedSpeechContext
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06PregameContinuation]
        }
    }

    bool InstallLive06PregameContextHook()
    {
        // 0x0045D240 builds event 164; its only direct caller is
        // 0x00461B69. 0x0045CD00 is a separate request-management routine,
        // ending at 0x0045D224, and cannot cover the first pregame event.
        // MOV AL,[00CAF5D0h] is one complete five-byte instruction.
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        const bool live05 = gGameVersion == GAME_LIVE_2005;
        const size_t patchLength = live08 ? 7 : (live07 || live05 ? 6 : 5);
        unsigned char* target = reinterpret_cast<unsigned char*>(live05 ? 0x0045B5F0 : (live08 ? 0x0048ECE0 : (live07 ? 0x0047C430 : 0x0045D240)));
        const unsigned char expected[5] = { 0xA0, 0xD0, 0xF5, 0xCA, 0x00 };
        const unsigned char expected07[6] = { 0x81, 0xEC, 0xA4, 0x02, 0x00, 0x00 };
        // Two complete PUSH instructions, before the native SEH frame.
        const unsigned char expected08[7] = { 0x6A, 0xFF, 0x68, 0x71, 0x1C, 0xBF, 0x00 };
        const unsigned char expected05[6] = { 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00 };
        if (std::memcmp(target, live05 ? expected05 : (live08 ? expected08 : (live07 ? expected07 : expected)), patchLength) != 0)
            return false;
        unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline)
            return false;
        std::memcpy(trampoline, target, patchLength);
        trampoline[patchLength] = 0xE9;
        const int back = static_cast<int>(
            reinterpret_cast<uintptr_t>(target + patchLength) -
            reinterpret_cast<uintptr_t>(trampoline + patchLength + 5));
        std::memcpy(trampoline + patchLength + 1, &back, sizeof(back));
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        gLive06PregameContinuation = reinterpret_cast<uintptr_t>(trampoline);
        unsigned char replacement[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        const int forward = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06PregameContextHook) -
            reinterpret_cast<uintptr_t>(target + 5));
        std::memcpy(replacement + 1, &forward, sizeof(forward));
        std::memcpy(target, replacement, patchLength);
        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), trampoline, patchLength + 5);
        FlushInstructionCache(GetCurrentProcess(), target, patchLength);
        return true;
    }

    void LogLive06AudioSettings(const char* phase);

    void __stdcall LogLive06SpeechDispatch(const unsigned int* entryStack)
    {
        static unsigned int snapshots = 0;
        static unsigned int previousHash = 0;
        static bool previousValid = false;
        if (!gPlayoffStateDebug || snapshots >= 512)
            return;
        __try
        {
            const unsigned int caller = entryStack[0];
            // Native speech emitters. Exclude frontend and middleware traffic.
            const bool live07 = gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08;
            if (caller < (live07 ? 0x00460000u : 0x00430000u) ||
                caller >= (live07 ? 0x004A0000u : 0x00470000u))
                return;
            const unsigned int event = entryStack[1];
            const unsigned int count = entryStack[2];
            if (count > 64 ||
                !IsReadableMemoryRange(entryStack, (count + 3) * sizeof(unsigned int)))
                return;
            unsigned int hash = 2166136261u;
            for (unsigned int i = 0; i < count + 3; ++i)
                hash = (hash ^ entryStack[i]) * 16777619u;
            if (previousValid && hash == previousHash)
                return;
            previousHash = hash;
            previousValid = true;
            ++snapshots;
            char message[320] = {};
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SPEECH_EVENT snapshot=%u "
                "event=%u route=%u group=%u packed=%08X count=%u caller=%08X\n",
                snapshots, event & 0xFFFFu, event >> 24,
                (event >> 16) & 0xFFu, event, count, caller);
            WritePlayoffStateLog(message);
            for (unsigned int i = 0; i < count; ++i)
            {
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SPEECH_ARG snapshot=%u "
                    "index=%02u hex=%08X signed=%d\n", snapshots, i,
                    entryStack[i + 3], static_cast<int>(entryStack[i + 3]));
                WritePlayoffStateLog(message);
            }
            // Capture the settings at the actual speech dispatch boundary.
            // This also works when the event-8 emitter never runs.
            LogLive06AudioSettings("speech-dispatch");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SPEECH_EVENT unreadable.\n");
        }
    }

    void __declspec(naked) Live06SpeechDispatchTraceHook()
    {
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            lea eax, [ebx + 36]
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push eax
            call LogLive06SpeechDispatch
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06SpeechDispatchContinuation]
        }
    }

    bool InstallLive06SpeechDispatchTrace()
    {
        // Whole instructions: PUSH EBX; PUSH ESI; PUSH EDI;
        // MOV EDI,[ESP+14h]. No relative instructions in this prologue.
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        const size_t patchLength = live07 || live08 ? 6 : 7;
        unsigned char* target = reinterpret_cast<unsigned char*>(gGameVersion == GAME_LIVE_2005 ? 0x0070DDB0 : (live08 ? 0x00851580 : (live07 ? 0x00822540 : 0x00722C0D)));
        const unsigned char expected[7] = {
            0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14
        };
        const unsigned char expected07[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
        if (std::memcmp(target, live07 || live08 ? expected07 : expected, patchLength) != 0)
            return false;
        unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline)
            return false;
        std::memcpy(trampoline, target, patchLength);
        trampoline[patchLength] = 0xE9;
        const int back = static_cast<int>(
            reinterpret_cast<uintptr_t>(target + patchLength) -
            reinterpret_cast<uintptr_t>(trampoline + patchLength + 5));
        std::memcpy(trampoline + patchLength + 1, &back, sizeof(back));
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        gLive06SpeechDispatchContinuation =
            reinterpret_cast<uintptr_t>(trampoline);
        unsigned char replacement[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        const int forward = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06SpeechDispatchTraceHook) -
            reinterpret_cast<uintptr_t>(target + 5));
        std::memcpy(replacement + 1, &forward, sizeof(forward));
        std::memcpy(target, replacement, patchLength);
        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), trampoline, patchLength + 5);
        FlushInstructionCache(GetCurrentProcess(), target, patchLength);
        return true;
    }

    void LogLive06AudioSettings(const char* phase)
    {
        char message[320] = {};
        __try
        {
            const unsigned char* root =
                *reinterpret_cast<unsigned char**>(gGameVersion == GAME_LIVE_2005
                    ? 0x00C44A30 : (gGameVersion == GAME_LIVE_08
                    ? 0x00DA3D08 : (gGameVersion == GAME_LIVE_07 ? 0x00CE8378 : 0x00CAF5D8)));
            if (!IsReadableMemoryRange(root, 0x3C))
                return;
            const unsigned char* rules =
                *reinterpret_cast<const unsigned char* const*>(root + 0x28);
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_AUDIO_SETTINGS phase=%s root=%08X "
                "rules=%08X lastOptions=%08X ready=%u active=%u\n", phase,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(root)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(rules)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(gLastPlayoffSnapshotOptions)),
                static_cast<unsigned int>(root[8]),
                static_cast<unsigned int>(root[9]));
            WritePlayoffStateLog(message);
            if (!IsReadableMemoryRange(rules, 0x174))
                return;
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_AUDIO_RULES phase=%s mode=%d "
                "round=%d adjacent=%d length=%d\n", phase,
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x16C : 0x15C)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x170 : 0x160)));
            WritePlayoffStateLog(message);
            LogRawMemoryBlock("AUDIO_RULES_100", rules + 0x100, 0x70);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_AUDIO_SETTINGS unreadable.\n");
        }
    }

    void __stdcall LogLive06SetupHandoff(
        const unsigned char* setup, const unsigned int* entryStack,
        const unsigned int* vtable, void* interfaceObject)
    {
        static unsigned int count = 0;
        if (!gPlayoffStateDebug || count >= 32)
            return;
        ++count;
        char message[320] = {};
        __try
        {
            const unsigned char* rules =
                reinterpret_cast<const unsigned char*>(entryStack[2]);
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SETUP_HANDOFF snapshot=%u "
                "setup=%08X interface=%08X target=%08X "
                "settings=%08X rules=%08X\n", count,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(setup)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(interfaceObject)),
                vtable[0x15C / 4], entryStack[1], entryStack[2]);
            WritePlayoffStateLog(message);
            if (IsReadableMemoryRange(rules, 0x174))
            {
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SETUP_RULES mode=%d "
                    "field158=%d field15C=%d length160=%d\n",
                    *reinterpret_cast<const int*>(rules + 0x5C),
                    *reinterpret_cast<const int*>(rules + 0x158),
                    *reinterpret_cast<const int*>(rules + 0x15C),
                    *reinterpret_cast<const int*>(rules + 0x160));
                WritePlayoffStateLog(message);
                LogRawMemoryBlock("SETUP_RULES_100", rules + 0x100, 0x70);
            }
            LogLive06AudioSettings("setup-handoff-before");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETUP_HANDOFF unreadable.\n");
        }
    }

    void __declspec(naked) Live06SetupHandoffTraceHook()
    {
        // Replace only CALL [eax+15Ch]. EAX is the native vtable and ECX
        // the native interface. Restore all state before dispatching it.
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push dword ptr [ebx + 24]
            push dword ptr [ebx + 28]
            lea edx, [ebx + 36]
            push edx
            push dword ptr [ebx + 4]
            call LogLive06SetupHandoff
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [eax + 15Ch]
        }
    }

    bool InstallLive06SetupHandoffTrace()
    {
        unsigned char* site = reinterpret_cast<unsigned char*>(0x0062F7DF);
        const unsigned char expected[6] = { 0xFF, 0x90, 0x5C, 0x01, 0, 0 };
        if (std::memcmp(site, expected, sizeof(expected)) != 0)
            return false;
        DWORD oldProtect = 0;
        if (!VirtualProtect(site, sizeof(expected), PAGE_EXECUTE_READWRITE,
                            &oldProtect))
            return false;
        unsigned char replacement[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        const int displacement = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06SetupHandoffTraceHook) -
            reinterpret_cast<uintptr_t>(site + 5));
        std::memcpy(replacement + 1, &displacement, sizeof(displacement));
        std::memcpy(site, replacement, sizeof(replacement));
        DWORD ignored = 0;
        VirtualProtect(site, sizeof(expected), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(expected));
        return true;
    }

    void __stdcall LogLive06IntroArguments(const unsigned int* entryStack)
    {
        if (!gPlayoffStateDebug || gLive06IntroTraceCount >= 64)
            return;

        unsigned int arguments[36] = {};
        __try
        {
            std::memcpy(arguments, entryStack + 1, sizeof(arguments));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_INTRO arguments unreadable.\n");
            return;
        }

        if (gLive06IntroArgumentsValid &&
            std::memcmp(arguments, gLive06IntroArguments,
                        sizeof(arguments)) == 0)
        {
            return;
        }
        std::memcpy(gLive06IntroArguments, arguments, sizeof(arguments));
        gLive06IntroArgumentsValid = true;
        ++gLive06IntroTraceCount;

        char message[320] = {};
        sprintf_s(message, sizeof(message),
            "[CustomPaths] PLAYOFF_INTRO snapshot=%u event=8 count=36 "
            "caller=%08X forced=%u\n",
            gLive06IntroTraceCount, entryStack[0],
            gForcedSeriesEnabled && gForcedStageEnabled &&
                gForceGlobalGameMode ? 1u : 0u);
        WritePlayoffStateLog(message);

        LogLive06AudioSettings("intro-event");
        for (unsigned int i = 0; i < 36; i += 4)
        {
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_INTRO_ARGS snapshot=%u "
                "index=%02u..%02u hex=%08X,%08X,%08X,%08X "
                "signed=%d,%d,%d,%d\n",
                gLive06IntroTraceCount, i, i + 3,
                arguments[i], arguments[i + 1],
                arguments[i + 2], arguments[i + 3],
                static_cast<int>(arguments[i]),
                static_cast<int>(arguments[i + 1]),
                static_cast<int>(arguments[i + 2]),
                static_cast<int>(arguments[i + 3]));
            WritePlayoffStateLog(message);
        }
    }

    void __declspec(naked) Live06IntroEventTraceHook()
    {
        // The native cdecl emitter receives 36 dwords. Preserve its exact
        // stack, registers, flags, x87 and SSE state, then tail-call it.
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            lea eax, [ebx + 36]
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push eax
            call LogLive06IntroArguments
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06IntroEventTarget]
        }
    }
#endif

    void LogLive06TournamentState()
    {
        if (!gPlayoffStateDebug || gGameVersion != GAME_LIVE_06)
            return;

        // Log exactly once for each PLAYOFF_STATE notification, even when
        // the decoded SettingsObject and bracket bytes happen to be equal.
        if (gPlayoffSnapshotSequence == 0 ||
            gLastTournamentPlayoffSequence == gPlayoffSnapshotSequence)
        {
            return;
        }

        const size_t uberSize = 0x90;
        // Executable research identified +0x58 as SettingsObject and +0x74
        // as the tournament/bracket object. Do not scan unrelated FE objects.
        const unsigned int childOffsets[kTournamentChildCandidateCount] = {
            0x58, 0x74
        };
        const size_t childSizes[kTournamentChildCandidateCount] = {
            0x428, 0x854
        };
        void* uber = nullptr;

        __try
        {
            uber = *reinterpret_cast<void**>(0x00CB2A2C);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        if (!IsReadableMemoryRange(uber, uberSize))
            return;

        void* children[kTournamentChildCandidateCount] = {};
        void* tournamentRoot = nullptr;
        void* bracket = nullptr;
        int gameMode = -1;
        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(uber);
            gameMode = *reinterpret_cast<const int*>(bytes + 0x08);
            for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
            {
                children[i] = *reinterpret_cast<void* const*>(
                    bytes + childOffsets[i]);
            }

            // The global game-mode owner stores the active playoff bracket at
            // +0x8E4. Native playoff code at 0x004F82D2 follows this exact
            // chain, and the bracket destructor at 0x0051E710 clears it.
            tournamentRoot = *reinterpret_cast<void**>(0x00CB2B08);
            const unsigned char* tournamentRootBytes =
                reinterpret_cast<const unsigned char*>(tournamentRoot);
            if (IsReadableMemoryRange(tournamentRootBytes, 0x8E8))
            {
                bracket = *reinterpret_cast<void* const*>(
                    tournamentRootBytes + 0x8E4);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        unsigned int hash = 2166136261u;
        if (!AddMemoryToHash(uber, uberSize, hash))
            return;

        for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
        {
            const size_t childSize = childSizes[i];
            const uintptr_t pointerValue =
                reinterpret_cast<uintptr_t>(children[i]);
            hash ^= static_cast<unsigned int>(pointerValue);
            hash *= 16777619u;
            AddMemoryToHash(children[i], childSize, hash);
        }

        const uintptr_t bracketPointerValue =
            reinterpret_cast<uintptr_t>(bracket);
        hash ^= static_cast<unsigned int>(bracketPointerValue);
        hash *= 16777619u;
        AddMemoryToHash(bracket, 0x4BC, hash);

        const bool gameModeChanged = gameMode != gLastTournamentGameMode;

        gLastTournamentPlayoffSequence = gPlayoffSnapshotSequence;
        gLastTournamentSnapshotValid = true;
        gLastTournamentSnapshotHash = hash;
        ++gTournamentSnapshotSequence;

        if (gameModeChanged)
        {
            gLastTournamentGameMode = gameMode;
            std::memset(
                gTournamentChildSnapshots,
                0,
                sizeof(gTournamentChildSnapshots));
        }

        char message[256] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] TOURNAMENT_STATE snapshot=%u uber=%08X "
            "gameMode=%d hash=%08X\n",
            gTournamentSnapshotSequence,
            static_cast<unsigned int>(reinterpret_cast<uintptr_t>(uber)),
            gameMode,
            hash);
        WritePlayoffStateLog(message);

        LogLive06PlayoffSettings(children[0]);
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_BRACKET root=%08X bracket=%08X\n",
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(tournamentRoot)),
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(bracket)));
        WritePlayoffStateLog(message);
        LogLive06PlayoffBracket(bracket);

        if (gameModeChanged)
        {
            WritePlayoffStateLog(
                "[CustomPaths] TOURNAMENT_CONTEXT changed; child "
                "baselines reset.\n");
        }

        LogRawMemoryBlock("FE_UBER", uber, uberSize);

        for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
        {
            const size_t childSize = childSizes[i];
            const bool readable =
                IsReadableMemoryRange(children[i], childSize);
            TournamentChildSnapshot& previous =
                gTournamentChildSnapshots[i];

            if (!readable)
            {
                if (previous.valid || previous.pointer != children[i])
                {
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X pointer=%08X "
                        "readable=no\n",
                        childOffsets[i],
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(children[i])));
                    WritePlayoffStateLog(message);
                }
                previous.pointer = children[i];
                previous.valid = false;
                continue;
            }

            __try
            {
                const unsigned char* current =
                    reinterpret_cast<const unsigned char*>(children[i]);

                if (!previous.valid || previous.pointer != children[i])
                {
                    previous.pointer = children[i];
                    std::memcpy(previous.bytes, current, childSize);
                    previous.valid = true;
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X baseline pointer=%08X "
                        "size=%04X\n",
                        childOffsets[i],
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(children[i])),
                        static_cast<unsigned int>(childSize));
                    WritePlayoffStateLog(message);
                    continue;
                }

                unsigned int changed = 0;
                const unsigned int maximumLoggedChanges = 256;
                for (size_t offset = 0;
                     offset + sizeof(unsigned int) <= childSize;
                     offset += sizeof(unsigned int))
                {
                    const unsigned int oldValue =
                        *reinterpret_cast<const unsigned int*>(
                            previous.bytes + offset);
                    const unsigned int newValue =
                        *reinterpret_cast<const unsigned int*>(
                            current + offset);
                    if (oldValue == newValue)
                        continue;

                    if (changed < maximumLoggedChanges)
                    {
                        sprintf_s(
                            message,
                            sizeof(message),
                            "[CustomPaths] FE_CHILD_%02X_CHANGE +%04X: "
                            "%08X -> %08X\n",
                            childOffsets[i],
                            static_cast<unsigned int>(offset),
                            oldValue,
                            newValue);
                        WritePlayoffStateLog(message);
                    }
                    ++changed;
                }

                if (changed > maximumLoggedChanges)
                {
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X_CHANGE truncated: "
                        "%u total changed dwords.\n",
                        childOffsets[i],
                        changed);
                    WritePlayoffStateLog(message);
                }

                if (changed != 0)
                {
                    std::memcpy(previous.bytes, current, childSize);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] FE_CHILD_%02X comparison failed.\n",
                    childOffsets[i]);
                WritePlayoffStateLog(message);
                previous.valid = false;
            }
        }
    }

    void InitializeForcedStageSettings()
    {
        char value[32] = {};
        GetPrivateProfileStringA(
            "PLAYOFF",
            "FORCE_STAGE",
            "OFF",
            value,
            sizeof(value),
            ".\\main.ini");

        if (_stricmp(value, "PLAYOFF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_PLAYOFF;
        }
        else if (_stricmp(value, "ECSF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_ECSF;
        }
        else if (_stricmp(value, "WCSF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_WCSF;
        }
        else if (_stricmp(value, "ECF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_ECF;
        }
        else if (_stricmp(value, "WCF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_WCF;
        }
        else if (_stricmp(value, "FINALS") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_FINALS;
        }
        else
        {
            gForcedStageEnabled = false;
            gForcedStage = STAGE_REGULAR;
        }

        gForceGlobalGameMode =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "GLOBAL_GAME_MODE",
                0,
                ".\\main.ini") != 0;

        const int requestedSeriesGame =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_SERIES_GAME",
                0,
                ".\\main.ini");
        const int requestedSeriesLength = GetPrivateProfileIntA(
            "PLAYOFF", "FORCE_SERIES_LENGTH", 7, ".\\main.ini");
        const bool validSeriesLength = requestedSeriesLength == 1 ||
            requestedSeriesLength == 3 || requestedSeriesLength == 5 ||
            requestedSeriesLength == 7;
        const int maximumPreGameWins = validSeriesLength
            ? (requestedSeriesLength - 1) / 2 : -1;
        const int requestedHomeWins =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_HOME_WINS",
                -1,
                ".\\main.ini");
        const int requestedAwayWins =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_AWAY_WINS",
                -1,
                ".\\main.ini");

        gForcedSeriesEnabled =
            validSeriesLength &&
            requestedSeriesGame >= 1 && requestedSeriesGame <= requestedSeriesLength &&
            requestedHomeWins >= 0 && requestedHomeWins <= maximumPreGameWins &&
            requestedAwayWins >= 0 && requestedAwayWins <= maximumPreGameWins &&
            requestedHomeWins + requestedAwayWins ==
                requestedSeriesGame - 1;

        if (gForcedSeriesEnabled)
        {
            gForcedSeriesGame = requestedSeriesGame;
            gForcedSeriesLength = requestedSeriesLength;
            gForcedHomeWins = requestedHomeWins;
            gForcedAwayWins = requestedAwayWins;
        }
        else
        {
            gForcedSeriesGame = 0;
            gForcedSeriesLength = 7;
            gForcedHomeWins = 0;
            gForcedAwayWins = 0;

            if (requestedSeriesGame != 0 ||
                requestedHomeWins != -1 ||
                requestedAwayWins != -1)
            {
                OutputDebugStringA(
                    "[CustomPaths] Ignored invalid forced-series values; "
                    "length must be 1/3/5/7, game must be within length, "
                    "neither team may have won the series, and total wins "
                    "must equal game-1.\n");
            }
        }

        gPlayoffStateDebug =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "LOG_STATE",
                0,
                ".\\main.ini") != 0;

        char nativeMode[32] = {};
        GetPrivateProfileStringA(
            "GAME_MODE",
            "FORCE_MODE",
            "OFF",
            nativeMode,
            sizeof(nativeMode),
            ".\\main.ini");

        if (_stricmp(nativeMode, "ALLSTAR") == 0 ||
            _stricmp(nativeMode, "ALL_STAR") == 0)
        {
            gForcedNativeGameMode = NATIVE_MODE_ALLSTAR;
        }
        else if (_stricmp(nativeMode, "ROOKIE") == 0 ||
                 _stricmp(nativeMode, "ROOKIE_CHALLENGE") == 0)
        {
            gForcedNativeGameMode = NATIVE_MODE_ROOKIE;
        }
        else
        {
            gForcedNativeGameMode = NATIVE_MODE_OFF;
        }

        // A native All-Star event and a forced playoff state are mutually
        // exclusive. Give the explicit event selection priority so playoff
        // arena assets and tournament state are not mixed into the test.
        if (gForcedNativeGameMode != NATIVE_MODE_OFF &&
            (gForcedStageEnabled || gForceGlobalGameMode))
        {
            gForcedStageEnabled = false;
            gForcedStage = STAGE_REGULAR;
            gForceGlobalGameMode = false;
            gForcedSeriesEnabled = false;
            OutputDebugStringA(
                "[CustomPaths] FORCE_MODE overrides [PLAYOFF] forcing.\n");
        }

        // A series score is meaningful only when the complete native playoff
        // presentation state is being forced. Do not leave a partial request
        // active, because that could combine a regular exhibition with
        // playoff clinching flags.
        if (gForcedSeriesEnabled &&
            (!gForcedStageEnabled || !gForceGlobalGameMode))
        {
            gForcedSeriesEnabled = false;
            gForcedSeriesGame = 0;
            gForcedHomeWins = 0;
            gForcedAwayWins = 0;
            OutputDebugStringA(
                "[CustomPaths] FORCE_SERIES_GAME requires FORCE_STAGE and "
                "GLOBAL_GAME_MODE=1; forced series ignored.\n");
        }

        if (gForcedStageEnabled)
        {
            char message[192] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Forced stage=%s scope=%s.\n",
                StageName(gForcedStage),
                gForceGlobalGameMode ? "global" : "stadium-only");
            OutputDebugStringA(message);
        }

        if (gForcedSeriesEnabled)
        {
            char message[224] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Forced series game=%d homeWins=%d "
                "awayWins=%d.\n",
                gForcedSeriesGame,
                gForcedHomeWins,
                gForcedAwayWins);
            OutputDebugStringA(message);
        }

        if (gPlayoffStateDebug)
        {
            OutputDebugStringA(
                "[CustomPaths] Read-only playoff-state logging enabled.\n");
        }


        if (gForcedNativeGameMode != NATIVE_MODE_OFF)
        {
            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Requested native game mode=%s.\n",
                ForcedNativeGameModeName(gForcedNativeGameMode));
            OutputDebugStringA(message);
        }
    }

    void LogPlayoffStateSnapshot(void* options)
    {
        if (!gPlayoffStateDebug || !options)
            return;

        const unsigned int firstOffset = 0x40;
        const unsigned int endOffset =
            gGameVersion == GAME_LIVE_08 ? 0x190 : 0x180;
        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(options);
            unsigned int hash = 2166136261u;
            for (unsigned int i = firstOffset; i < endOffset; ++i)
            {
                hash ^= bytes[i];
                hash *= 16777619u;
            }

            // Repeated GameCourt notifications normally carry the same state.
            // If the Live 06 schedule database was not ready on the first
            // notification, keep retrying until the exact series score has
            // been resolved.
            if (options == gLastPlayoffSnapshotOptions &&
                hash == gLastPlayoffSnapshotHash &&
                !gRetryLive06PlayoffSeries)
            {
                return;
            }
            gLastPlayoffSnapshotOptions = options;
            gLastPlayoffSnapshotHash = hash;
            gRetryLive06PlayoffSeries = false;
            ++gPlayoffSnapshotSequence;

            const int mode = *reinterpret_cast<const int*>(bytes + modeOffset);
            const int round = *reinterpret_cast<const int*>(bytes + roundOffset);
            char message[256] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_FORCE stage=%s enabled=%u global=%u "
                "series=%u game=%d homeWins=%d awayWins=%d\n",
                StageName(gForcedStage),
                gForcedStageEnabled ? 1u : 0u,
                gForceGlobalGameMode ? 1u : 0u,
                gForcedSeriesEnabled ? 1u : 0u,
                gForcedSeriesGame,
                gForcedHomeWins,
                gForcedAwayWins);
            WritePlayoffStateLog(message);

            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_STATE game=%s options=%08X "
                "mode[+%04X]=%d round[+%04X]=%d hash=%08X\n",
                GameVersionName(),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(options)),
                modeOffset,
                mode,
                roundOffset,
                round,
                hash);
            WritePlayoffStateLog(message);

            if (gGameVersion == GAME_LIVE_06)
            {
                // RulesSettings carries the two native playoff-clinching
                // flags directly. Gameplay copies +0x10D/+0x10E into
                // 0x00D212D5/0x00D212D4 in sub_8CF820, and GdCentral copies
                // the same bytes into its presentation state at +0x24/+0x25.
                const bool awayCanWin = bytes[0x10D] != 0;
                const bool homeCanWin = bytes[0x10E] != 0;
                const char* clinchState = "none";
                if (awayCanWin && homeCanWin)
                    clinchState = "decisive";
                else if (awayCanWin)
                    clinchState = "away-can-clinch";
                else if (homeCanWin)
                    clinchState = "home-can-clinch";

                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_RULES awayCanWin=%u "
                    "homeCanWin=%u clinchState=%s\n",
                    awayCanWin ? 1u : 0u,
                    homeCanWin ? 1u : 0u,
                    clinchState);
                WritePlayoffStateLog(message);

                // ScheduleInfo.GetGame/GetLastGame read these two native
                // identifiers from the Live 06 schedule service. Keep this
                // probe narrow: it avoids reopening scheduleplayoff.dbf or
                // tracing the generic database layer during every file load.
                const unsigned char* scheduleInfo =
                    *reinterpret_cast<unsigned char* const*>(0x00CB2AE0);
                if (scheduleInfo)
                {
                    const unsigned int currentGame =
                        *reinterpret_cast<const unsigned short*>(
                            scheduleInfo + 0x64);
                    const unsigned int lastGame =
                        *reinterpret_cast<const unsigned short*>(
                            scheduleInfo + 0x66);

                    // scheduleplayoff.dbf interleaves the 15 bracket series:
                    // one potential game for every series per block. Thus a
                    // series advances by 15 GAMENUM values each game.
                    const bool validPlayoffGame =
                        currentGame > 0u && currentGame <= 15u * 7u;
                    const unsigned int seriesGame =
                        validPlayoffGame
                            ? ((currentGame - 1u) / 15u) + 1u
                            : 0u;
                    const unsigned int bracketSlot =
                        validPlayoffGame
                            ? ((currentGame - 1u) % 15u) + 1u
                            : 0u;

                    char seriesState[64] = "unknown";
                    if (seriesGame == 1u)
                    {
                        strcpy_s(seriesState, "tied-0-0");
                    }
                    else if (awayCanWin && homeCanWin)
                    {
                        strcpy_s(seriesState, "tied-3-3");
                    }
                    else if (awayCanWin && seriesGame >= 4u)
                    {
                        sprintf_s(
                            seriesState,
                            sizeof(seriesState),
                            "away-leads-3-%u",
                            seriesGame - 4u);
                    }
                    else if (homeCanWin && seriesGame >= 4u)
                    {
                        sprintf_s(
                            seriesState,
                            sizeof(seriesState),
                            "home-leads-3-%u",
                            seriesGame - 4u);
                    }
                    else if (seriesGame == 5u)
                    {
                        // Four completed games and neither team has three
                        // wins can only be a 2-2 series.
                        strcpy_s(seriesState, "tied-2-2");
                    }

                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_SCHEDULE currentGame=%u "
                        "lastGame=%u seriesGame=%u bracketSlot=%u "
                        "seriesState=%s service=%08X\n",
                        currentGame,
                        lastGame,
                        seriesGame,
                        bracketSlot,
                        seriesState,
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(scheduleInfo)));
                    WritePlayoffStateLog(message);

                    if (gForcedSeriesEnabled &&
                        gForcedStageEnabled &&
                        gForceGlobalGameMode)
                    {
                        gRetryLive06PlayoffSeries = false;
                        sprintf_s(
                            message,
                            sizeof(message),
                            "[CustomPaths] PLAYOFF_SERIES forced=yes "
                            "game=%d homeWins=%d awayWins=%d exact=yes\n",
                            gForcedSeriesGame,
                            gForcedHomeWins,
                            gForcedAwayWins);
                        WritePlayoffStateLog(message);
                    }
                    else
                    {
                        // Read the live schedule records owned by the game and
                        // count the completed games in this exact bracket slot.
                        // The DBF layout advances a series by 15 IDs per game.
                        Live06ScheduleGame currentScheduleGame = {};
                        if (validPlayoffGame &&
                            ReadLive06ScheduleGame(
                                currentGame,
                                currentScheduleGame))
                        {
                            // Native normalized layout: +20 home team, +22
                            // home score, +26 away team, +28 away score.
                            const short homeTeam = ReadScheduleShort(
                                currentScheduleGame,
                                20);
                            const short awayTeam = ReadScheduleShort(
                                currentScheduleGame,
                                26);
                            unsigned int homeWins = 0;
                            unsigned int awayWins = 0;
                            unsigned int historyGames = 0;
                            bool exactSeriesScore =
                                homeTeam >= 0 && awayTeam >= 0 &&
                                homeTeam != awayTeam;

                            unsigned int previousGame = currentGame;
                            while (exactSeriesScore && previousGame > 15u)
                            {
                                previousGame -= 15u;
                                Live06ScheduleGame prior = {};
                                if (!ReadLive06ScheduleGame(previousGame, prior))
                                {
                                    exactSeriesScore = false;
                                    break;
                                }

                                const short priorHome =
                                    ReadScheduleShort(prior, 20);
                                const short priorAway =
                                    ReadScheduleShort(prior, 26);
                                const short priorHomeScore =
                                    ReadScheduleShort(prior, 22);
                                const short priorAwayScore =
                                    ReadScheduleShort(prior, 28);
                                const bool sameTeams =
                                    (priorHome == homeTeam &&
                                     priorAway == awayTeam) ||
                                    (priorHome == awayTeam &&
                                     priorAway == homeTeam);

                                // +18 is PLAYED in the normalized native record.
                                if (prior.bytes[18] == 0 ||
                                    !sameTeams ||
                                    priorHomeScore == priorAwayScore)
                                {
                                    exactSeriesScore = false;
                                    break;
                                }

                                const short winner =
                                    priorHomeScore > priorAwayScore
                                        ? priorHome
                                        : priorAway;
                                if (winner == homeTeam)
                                    ++homeWins;
                                else if (winner == awayTeam)
                                    ++awayWins;
                                else
                                {
                                    exactSeriesScore = false;
                                    break;
                                }
                                ++historyGames;
                            }

                            // All preceding games must have been accounted for.
                            if (historyGames + 1u != seriesGame)
                                exactSeriesScore = false;

                            gRetryLive06PlayoffSeries = !exactSeriesScore;

                            sprintf_s(
                                message,
                                sizeof(message),
                                "[CustomPaths] PLAYOFF_SERIES game=%u "
                                "homeTeam=%d awayTeam=%d homeWins=%u "
                                "awayWins=%u historyGames=%u exact=%s\n",
                                seriesGame,
                                homeTeam,
                                awayTeam,
                                homeWins,
                                awayWins,
                                historyGames,
                                exactSeriesScore ? "yes" : "no");
                            WritePlayoffStateLog(message);
                        }
                        else
                        {
                            gRetryLive06PlayoffSeries = true;
                            WritePlayoffStateLog(
                                "[CustomPaths] PLAYOFF_SERIES available=no.\n");
                        }
                    }
                }
                else
                {
                    gRetryLive06PlayoffSeries = true;
                    WritePlayoffStateLog(
                        "[CustomPaths] PLAYOFF_SCHEDULE available=no.\n");
                }
            }

            if (!gPreviousOptionsSnapshotValid)
            {
                std::memcpy(
                    gPreviousOptionsSnapshot,
                    bytes,
                    endOffset);
                gPreviousOptionsSnapshotValid = true;
                WritePlayoffStateLog(
                    "[CustomPaths] OPTIONS_BASELINE captured; subsequent "
                    "snapshots will list changed dwords.\n");
            }
            else
            {
                unsigned int changed = 0;
                for (unsigned int offset = firstOffset;
                     offset + sizeof(unsigned int) <= endOffset;
                     offset += sizeof(unsigned int))
                {
                    const unsigned int previous =
                        *reinterpret_cast<const unsigned int*>(
                            gPreviousOptionsSnapshot + offset);
                    const unsigned int current =
                        *reinterpret_cast<const unsigned int*>(bytes + offset);
                    if (previous == current)
                        continue;

                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] OPTIONS_CHANGE +%04X: "
                        "%08X -> %08X\n",
                        offset,
                        previous,
                        current);
                    WritePlayoffStateLog(message);
                    ++changed;
                }

                if (changed == 0)
                {
                    WritePlayoffStateLog(
                        "[CustomPaths] OPTIONS_CHANGE: no aligned dword "
                        "changes found.\n");
                }

                std::memcpy(
                    gPreviousOptionsSnapshot,
                    bytes,
                    endOffset);
            }

        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Could not read playoff-state snapshot.\n");
        }
    }

    void ApplyForcedNativeGameMode(void* options)
    {
        if (gForcedNativeGameMode == NATIVE_MODE_OFF)
            return;

        // The confirmed OptionsSettings values currently apply only to 06.
        if (gGameVersion != GAME_LIVE_06)
        {
            if (!gLoggedUnsupportedNativeMode)
            {
                OutputDebugStringA(
                    "[CustomPaths] FORCE_MODE is currently mapped only for "
                    "NBA Live 06; request ignored.\n");
                gLoggedUnsupportedNativeMode = true;
            }
            return;
        }

        if (!options)
        {
            if (!gLoggedMissingNativeModeOptions)
            {
                OutputDebugStringA(
                    "[CustomPaths] Native game mode requested, but the "
                    "OptionsSettings pointer is not available yet.\n");
                gLoggedMissingNativeModeOptions = true;
            }
            return;
        }

        gLoggedMissingNativeModeOptions = false;

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(options);
            int* mode = reinterpret_cast<int*>(bytes + 0x5C);
            int* round = reinterpret_cast<int*>(bytes + 0x158);
            const int originalMode = *mode;
            const int originalRound = *round;

            if (originalMode < 0 || originalMode > 32 ||
                originalRound < 0 || originalRound > 32)
            {
                OutputDebugStringA(
                    "[CustomPaths] Rejected native game-mode write because "
                    "OptionsSettings is not initialized.\n");
                return;
            }

            const int targetMode =
                gForcedNativeGameMode == NATIVE_MODE_ALLSTAR ? 16 : 17;
            const int targetRound = 7;
            if (originalMode == targetMode && originalRound == targetRound)
                return;

            *mode = targetMode;
            *round = targetRound;

            char message[256] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Applied NBA Live 06 native mode test: "
                "options=%08X mode %d->%d round %d->%d event=%s.\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(options)),
                originalMode,
                targetMode,
                originalRound,
                targetRound,
                ForcedNativeGameModeName(gForcedNativeGameMode));
            OutputDebugStringA(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply native game mode.\n");
        }
    }

    void ApplyGlobalForcedStage(void* options)
    {
        if (!gForcedStageEnabled || !gForceGlobalGameMode)
            return;

        if (!options)
        {
            if (!gLoggedMissingGlobalOptions)
            {
                OutputDebugStringA(
                    "[CustomPaths] Global forced stage requested, but the "
                    "game-options pointer is not available yet.\n");
                gLoggedMissingGlobalOptions = true;
            }
            return;
        }

        gLoggedMissingGlobalOptions = false;

        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            int* mode = reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + modeOffset);
            int* round = reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + roundOffset);

            const int originalMode = *mode;
            const int originalRound = *round;

            // Do not write into a pointer that has not reached a plausible
            // initialized OptionsSettings state yet.
            if (originalMode < 0 || originalMode > 16 ||
                originalRound < 0 || originalRound > 32)
            {
                if (options != gLastRejectedOptions ||
                    originalMode != gLastRejectedMode ||
                    originalRound != gLastRejectedRound)
                {
                    gLastRejectedOptions = options;
                    gLastRejectedMode = originalMode;
                    gLastRejectedRound = originalRound;

                    char message[224] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] Rejected global stage write: "
                        "options=%08X mode=%d round=%d.\n",
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(options)),
                        originalMode,
                        originalRound);
                    OutputDebugStringA(message);
                }
                return;
            }

            gLastRejectedOptions = nullptr;

            // Preserve an existing Franchise Playoff mode. Exhibition and
            // other modes are promoted to the standard Playoff mode.
            const int targetMode = originalMode == 4 ? 4 : 2;
            const int targetRound = StageRoundValue(gForcedStage);

            // The stadium setter can be called many times for one game. Avoid
            // touching stable state after the requested values are present.
            if (originalMode == targetMode && originalRound == targetRound)
                return;

            *mode = targetMode;
            *round = targetRound;

            char message[256] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Applied global stage once: options=%08X "
                "mode %d->%d round %d->%d stage=%s.\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(options)),
                originalMode,
                targetMode,
                originalRound,
                targetRound,
                StageName(gForcedStage));
            OutputDebugStringA(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply global forced stage.\n");
        }
    }

    unsigned int ForcedStageBracketSlot(
        PostseasonStage stage,
        unsigned int currentGame)
    {
        switch (stage)
        {
        case STAGE_WCSF:
        case STAGE_ECSF:
        {
            // Schedule slots 9/10 are West, 11/12 East. Preserve a
            // matching native matchup; use its conference's first slot
            // when launching without a matching schedule selection.
            const unsigned int firstSlot = stage == STAGE_WCSF ? 9u : 11u;
            if (currentGame > 0u && currentGame <= 15u * 7u)
            {
                const unsigned int currentSlot = ((currentGame - 1u) % 15u) + 1u;
                if (currentSlot == firstSlot || currentSlot == firstSlot + 1u)
                    return currentSlot;
            }
            return firstSlot;
        }
        case STAGE_WCF:
            return 13u;
        case STAGE_ECF:
            return 14u;
        case STAGE_FINALS:
            return 15u;
        case STAGE_PLAYOFF:
        {
            if (currentGame > 0u && currentGame <= 15u * 7u)
            {
                const unsigned int currentSlot =
                    ((currentGame - 1u) % 15u) + 1u;
                if (currentSlot <= 12u)
                    return currentSlot;
            }
            return 1u;
        }
        default:
            return 0u;
        }
    }

    unsigned int ForcedScheduleGameId(unsigned int currentGame)
    {
        const unsigned int bracketSlot =
            ForcedStageBracketSlot(gForcedStage, currentGame);
        if (bracketSlot == 0u ||
            gForcedSeriesGame < 1 ||
            gForcedSeriesGame > 7)
        {
            return 0u;
        }

        return bracketSlot +
            15u * static_cast<unsigned int>(gForcedSeriesGame - 1);
    }

    short __cdecl Live06ForcedCurrentGameGetterHook()
    {
        unsigned int currentGame = 0u;
        __try
        {
            unsigned char* scheduleInfo =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (scheduleInfo)
            {
                currentGame =
                    *reinterpret_cast<unsigned short*>(scheduleInfo + 0x64);
            }

            const unsigned int forcedGame =
                ForcedScheduleGameId(currentGame);
            if (forcedGame != 0u)
            {
                // Keep the backing object synchronized once it exists, while
                // still allowing native consumers to receive the forced ID
                // before ScheduleInfo has been constructed.
                if (scheduleInfo)
                {
                    *reinterpret_cast<unsigned short*>(scheduleInfo + 0x64) =
                        static_cast<unsigned short>(forcedGame);
                }

                if (!gLoggedForcedScheduleGetter)
                {
                    char message[224] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_SCHEDULE_GET forced=%u "
                        "original=%u stage=%s game=%d service=%s\n",
                        forcedGame,
                        currentGame,
                        StageName(gForcedStage),
                        gForcedSeriesGame,
                        scheduleInfo ? "ready" : "unavailable");
                    WritePlayoffStateLog(message);
                    gLoggedForcedScheduleGetter = true;
                }

                return static_cast<short>(forcedGame);
            }

            return static_cast<short>(currentGame);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void __fastcall Live06ScheduleSelectionHook(
        void* scheduleInfo,
        void*,
        int selection,
        void* selectionContext)
    {
        typedef void (__thiscall* ScheduleSelectionFn)(
            void*,
            int,
            void*);
        ScheduleSelectionFn selectScheduleGame =
            reinterpret_cast<ScheduleSelectionFn>(0x004F0250);

        // Preserve the complete native schedule-selection process first.
        selectScheduleGame(
            scheduleInfo,
            selection,
            selectionContext);

        if (!scheduleInfo ||
            !gForcedSeriesEnabled ||
            !gForcedStageEnabled ||
            !gForceGlobalGameMode)
        {
            return;
        }

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(scheduleInfo);
            const unsigned int originalGame =
                *reinterpret_cast<unsigned short*>(bytes + 0x64);
            const unsigned int forcedGame =
                ForcedScheduleGameId(originalGame);
            if (forcedGame == 0u)
                return;

            *reinterpret_cast<unsigned short*>(bytes + 0x64) =
                static_cast<unsigned short>(forcedGame);

            // This object becomes the global ScheduleInfo singleton during
            // initialization. Keep the global object synchronized whenever it
            // is already published.
            unsigned char* globalSchedule =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (globalSchedule)
            {
                *reinterpret_cast<unsigned short*>(globalSchedule + 0x64) =
                    static_cast<unsigned short>(forcedGame);
            }

            if (!gLoggedForcedScheduleSelection)
            {
                char message[240] = {};
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_SCHEDULE_SELECT forced=%u "
                    "original=%u stage=%s game=%d object=%08X global=%s\n",
                    forcedGame,
                    originalGame,
                    StageName(gForcedStage),
                    gForcedSeriesGame,
                    static_cast<unsigned int>(
                        reinterpret_cast<uintptr_t>(scheduleInfo)),
                    globalSchedule ? "ready" : "unavailable");
                WritePlayoffStateLog(message);
                gLoggedForcedScheduleSelection = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SCHEDULE_SELECT exception=yes.\n");
        }
    }

    void ApplyForcedSeriesState(void* options)
    {
        if (!gForcedSeriesEnabled ||
            !gForcedStageEnabled ||
            !gForceGlobalGameMode)
        {
            return;
        }

#if defined(_M_IX86)
        if (gGameVersion == GAME_LIVE_2005 ||
            gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08)
        {
            ApplyForcedSeriesRulesForBuild(options);
            return; // Schedule service layout is not shared with Live 06.
        }
#endif
        // Remaining schedule/global addresses below are specific to Live 06.
        if (gGameVersion != GAME_LIVE_06 || !options)
            return;

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(options);
            const unsigned char awayCanWin =
                gForcedAwayWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            const unsigned char homeCanWin =
                gForcedHomeWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;

            // RulesSettings fields consumed by gameplay setup.
            *reinterpret_cast<int*>(bytes + 0x160) =
                (gForcedSeriesLength - 1) / 2;
            bytes[0x10D] = awayCanWin;
            bytes[0x10E] = homeCanWin;

            // Mirror the same values into the live rules globals in case the
            // initial RulesSettings copy has already occurred.
            *reinterpret_cast<unsigned char*>(0x00D212D5) = awayCanWin;
            *reinterpret_cast<unsigned char*>(0x00D212D4) = homeCanWin;

            unsigned char* scheduleInfo =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (!scheduleInfo)
            {
                OutputDebugStringA(
                    "[CustomPaths] Forced series requested, but the Live 06 "
                    "schedule service is not available yet.\n");
                return;
            }

            unsigned short* currentGamePointer =
                reinterpret_cast<unsigned short*>(scheduleInfo + 0x64);
            const unsigned int originalGame = *currentGamePointer;
            const unsigned int bracketSlot =
                ForcedStageBracketSlot(gForcedStage, originalGame);
            if (bracketSlot == 0u)
                return;

            const unsigned int targetGame =
                ForcedScheduleGameId(originalGame);
            if (targetGame == 0u)
                return;
            *currentGamePointer = static_cast<unsigned short>(targetGame);

            if (originalGame != targetGame)
            {
                char message[256] = {};
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] Applied forced series: stage=%s "
                    "game=%d score(home-away)=%d-%d schedule %u->%u "
                    "slot=%u homeCanWin=%u awayCanWin=%u.\n",
                    StageName(gForcedStage),
                    gForcedSeriesGame,
                    gForcedHomeWins,
                    gForcedAwayWins,
                    originalGame,
                    targetGame,
                    bracketSlot,
                    homeCanWin,
                    awayCanWin);
                OutputDebugStringA(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply forced playoff-series state.\n");
        }
    }

    PostseasonStage GetPostseasonStage(void* suppliedOptions)
    {
        if (gForcedNativeGameMode != NATIVE_MODE_OFF)
            return STAGE_REGULAR;

        if (gForcedStageEnabled)
            return gForcedStage;

        void* options = GetCurrentOptions(suppliedOptions);
        if (!options)
            return STAGE_REGULAR;

        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            const int mode = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + modeOffset);
            const int round = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + roundOffset);

            // These are the same fields used by the games' native playoff-logo
            // selection. The bracket enumerates East then West, with the NBA
            // Finals last. A normal exhibition uses the round sentinel 16.
            if (mode != 2 && mode != 4)
                return STAGE_REGULAR;

            if (round == 6)
                return STAGE_FINALS;
            if (round == 4)
                return STAGE_ECF;
            if (round == 5)
                return STAGE_WCF;
            if (round == 2)
                return STAGE_ECSF;
            if (round == 3)
                return STAGE_WCSF;
            if (round >= 0 && round <= 1)
                return STAGE_PLAYOFF;

            return STAGE_REGULAR;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return STAGE_REGULAR;
        }
    }

    const char* GetStageFolder(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_ECF:
            return "ECF";
        case STAGE_WCF:
            return "WCF";
        case STAGE_FINALS:
            return "finals";
        default:
            return nullptr;
        }
    }

    void SetSlot(OverrideSlot slot, const char* value)
    {
        strcpy_s(
            gOverridePaths[slot],
            kOverridePathCapacity,
            value ? value : "");
    }

    void DisableAllSlots()
    {
        LoadBenchStadiumConfig(nullptr);
        for (unsigned int i = 0; i < SLOT_COUNT; ++i)
            gOverridePaths[i][0] = '\0';

        gGameContextValid = false;
        gCurrentStadiumId4[0] = '\0';
        gCurrentStage = STAGE_REGULAR;
        gOverrideCacheCount = 0;
        gOverrideCacheCursor = 0;
        std::memset(gOverrideCache, 0, sizeof(gOverrideCache));
    }

    // Entered at the native 3D game handoff, not during launcher startup.
    // Explicitly expire the previous game's context even if the next game uses
    // exactly the same ID4 and playoff round. UpdateGameContext is still cached
    // within that game, including the post-Setup options normalization pass.
    void BeginNewGameStadiumLoad()
    {
        DisableAllSlots();
        OutputDebugStringA(
            "[CustomPaths] New game load: expired stadium configuration/cache.\n");
    }

    void UpdateGameContext(const char* stadiumId4, void* suppliedOptions)
    {
        if (!IsValidId4(stadiumId4))
        {
            DisableAllSlots();
            return;
        }

        char id4[5] = {};
        NormalizeId4(stadiumId4, id4);
        void* options = GetCurrentOptions(suppliedOptions);
        // Earlier FEUber/bracket probes were useful for eliminating the
        // frontend objects, but they are not part of the gameplay handoff and
        // produced large logs. RulesSettings is now decoded directly above.
        ApplyForcedNativeGameMode(options);
        ApplyGlobalForcedStage(options);
        ApplyForcedSeriesState(options);
        LogPlayoffStateSnapshot(options);
        const PostseasonStage stage = GetPostseasonStage(options);
        const bool playoffs = stage != STAGE_REGULAR;
        const char* stageFolder = GetStageFolder(stage);

        // Within the same loading session, repeated notifications for the same
        // arena/stage do not invalidate the newly loaded filename cache.
        // BeginNewGameStadiumLoad expires it at the next gameplay handoff.
        if (gGameContextValid &&
            _stricmp(gCurrentStadiumId4, id4) == 0 &&
            gCurrentStage == stage)
        {
            return;
        }

        DisableAllSlots();
        gGameContextValid = true;
        strcpy_s(gCurrentStadiumId4, sizeof(gCurrentStadiumId4), id4);
        gCurrentStage = stage;
        if (gGameVersion == GAME_LIVE_06) LoadBenchStadiumConfig(id4);

        char path[kOverridePathCapacity] = {};

        if (stageFolder)
        {
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\court\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_COURT, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\backboard\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_BACKBOARD, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\dorna\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_DORNA, path);
        }
        else
        {
            SetSlot(SLOT_STAGE_COURT, "");
            SetSlot(SLOT_STAGE_BACKBOARD, "");
            SetSlot(SLOT_STAGE_DORNA, "");
        }

        if (playoffs)
        {
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\court\\",
                id4);
            SetSlot(SLOT_PLAYOFF_COURT, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\backboard\\",
                id4);
            SetSlot(SLOT_PLAYOFF_BACKBOARD, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\dorna\\",
                id4);
            SetSlot(SLOT_PLAYOFF_DORNA, path);
        }
        else
        {
            SetSlot(SLOT_PLAYOFF_COURT, "");
            SetSlot(SLOT_PLAYOFF_BACKBOARD, "");
            SetSlot(SLOT_PLAYOFF_DORNA, "");
        }

        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\court\\",
            id4);
        SetSlot(SLOT_REGULAR_COURT, path);
        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\backboard\\",
            id4);
        SetSlot(SLOT_REGULAR_BACKBOARD, path);
        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\dorna\\",
            id4);
        SetSlot(SLOT_REGULAR_DORNA, path);

        char message[192] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] Team '%s': stage=%s asset paths selected.\n",
            id4,
            StageName(stage));
        OutputDebugStringA(message);
    }

    bool ValidateCallSites(
        const unsigned int* sites,
        size_t count,
        unsigned int expectedTarget)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (patch::GetUChar(sites[i]) != 0xE8)
                return false;

            const int displacement = patch::GetInt(sites[i] + 1);
            if (sites[i] + 5 + displacement != expectedTarget)
                return false;
        }
        return true;
    }

    bool ContainsCaseInsensitive(const char* text, const char* token)
    {
        if (!text || !token || token[0] == '\0')
            return false;

        for (const char* start = text; *start; ++start)
        {
            const char* left = start;
            const char* right = token;
            while (*left && *right)
            {
                const unsigned char a = static_cast<unsigned char>(*left);
                const unsigned char b = static_cast<unsigned char>(*right);
                if (std::tolower(a) != std::tolower(b))
                    break;
                ++left;
                ++right;
            }

            if (*right == '\0')
                return true;
        }
        return false;
    }

    bool ShouldSearchStadiumPaths(const char* requestedFilename)
    {
        static const char* const tokens[] = {
            "bbd",
            "crt",
            "dorna",
            "std",
            "playoff",
            "finals",
            "crowd3d",
            "crowdbase",
            "conffinals"
        };

        for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i)
        {
            if (ContainsCaseInsensitive(requestedFilename, tokens[i]))
                return true;
        }
        return false;
    }

    const char* BaseNameOf(const char* path)
    {
        if (!path)
            return "";

        const char* base = path;
        for (const char* p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
        return base;
    }

    const char* ResolveCachedCustomPath(const char* requestedPath)
    {
        if (!gGameContextValid || !requestedPath)
            return nullptr;

        const char* logicalName = BaseNameOf(requestedPath);
        if (!*logicalName || !ShouldSearchStadiumPaths(logicalName))
            return nullptr;

        for (unsigned int i = 0; i < gOverrideCacheCount; ++i)
        {
            const OverrideCacheEntry& entry = gOverrideCache[i];
            if (_stricmp(entry.logicalName, logicalName) == 0)
                return entry.found ? entry.resolvedPath : nullptr;
        }

        unsigned int index = 0;
        if (gOverrideCacheCount < kOverrideCacheCapacity)
        {
            index = gOverrideCacheCount++;
        }
        else
        {
            index = gOverrideCacheCursor++ % kOverrideCacheCapacity;
        }

        OverrideCacheEntry& entry = gOverrideCache[index];
        std::memset(&entry, 0, sizeof(entry));
        strncpy_s(
            entry.logicalName,
            sizeof(entry.logicalName),
            logicalName,
            _TRUNCATE);

        for (unsigned int i = 0; i < SLOT_COUNT; ++i)
        {
            if (!gOverridePaths[i][0])
                continue;

            char candidate[MAX_PATH] = {};
            const int written = sprintf_s(
                candidate,
                sizeof(candidate),
                "%s%s",
                gOverridePaths[i],
                logicalName);
            if (written <= 0)
                continue;

            const DWORD attributes = GetFileAttributesA(candidate);
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                strcpy_s(
                    entry.resolvedPath,
                    sizeof(entry.resolvedPath),
                    candidate);
                entry.found = true;
                return entry.resolvedPath;
            }
        }

        // Negative entries are cached too. A missing custom file immediately
        // falls through to the game's untouched sgsm/archive resolver on all
        // subsequent requests for this stadium and stage.
        entry.found = false;
        return nullptr;
    }

#if defined(_M_IX86)
    void __cdecl ResolveCustomPathAtOpen(uintptr_t* entryStack)
    {
        if (!entryStack)
            return;

        const char* requestedPath = nullptr;
        unsigned int filenameIndex = 0;
        if (gGameVersion == GAME_LIVE_2005)
        {
            // Return address, filename, flags.
            filenameIndex = 1;
        }
        else
        {
            // Return address, internal type, filename, flags, context.
            filenameIndex = 2;
        }
        requestedPath = reinterpret_cast<const char*>(entryStack[filenameIndex]);

        __try
        {
            const char* resolved = ResolveCachedCustomPath(requestedPath);
            if (resolved)
                entryStack[filenameIndex] =
                    reinterpret_cast<uintptr_t>(resolved);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
    }

    __declspec(naked) void FilteredOpenResolved()
    {
        __asm
        {
            pushfd
            pushad

            // Original entry ESP is 36 bytes above the saved state.
            lea     eax, [esp + 36]
            push    eax
            call    ResolveCustomPathAtOpen
            add     esp, 4

            popad
            popfd
            jmp     dword ptr [gOpenResolvedContinuation]
        }
    }

    bool InstallOpenResolvedGate(unsigned int targetAddress)
    {
        if (gOpenResolvedGateInstalled)
            return true;

        const size_t patchLength = 9;
        unsigned char* target =
            reinterpret_cast<unsigned char*>(targetAddress);

        __try
        {
            // DebugConsole may already own the entry hook.  Chain to its JMP
            // destination so permitted requests retain all diagnostics.
            if (target[0] == 0xE9)
            {
                const int32_t displacement =
                    *reinterpret_cast<const int32_t*>(target + 1);
                gOpenResolvedContinuation =
                    targetAddress + 5 + displacement;
            }
            else if (target[0] == 0x55 &&
                     target[1] == 0x8B && target[2] == 0xEC &&
                     target[3] == 0x81 && target[4] == 0xEC)
            {
                unsigned char* trampoline =
                    reinterpret_cast<unsigned char*>(VirtualAlloc(
                        nullptr,
                        32,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE));
                if (!trampoline)
                    return false;

                std::memcpy(trampoline, target, patchLength);
                trampoline[patchLength] = 0xE9;
                *reinterpret_cast<int32_t*>(trampoline + patchLength + 1) =
                    static_cast<int32_t>(
                        (targetAddress + patchLength) -
                        reinterpret_cast<uintptr_t>(
                            trampoline + patchLength + 5));
                gOpenResolvedContinuation =
                    reinterpret_cast<uintptr_t>(trampoline);
            }
            else
            {
                OutputDebugStringA(
                    "[CustomPaths] OpenResolved gate validation failed.\n");
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                target,
                patchLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;
        *reinterpret_cast<int32_t*>(target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(&FilteredOpenResolved) -
                (targetAddress + 5));
        for (size_t i = 5; i < patchLength; ++i)
            target[i] = 0x90;

        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            patchLength);

        gOpenResolvedGateInstalled = true;
        OutputDebugStringA(
            "[CustomPaths] Cached direct stadium override hook installed.\n");
        return true;
    }
#else
    bool InstallOpenResolvedGate(unsigned int)
    {
        return false;
    }
#endif

    void __fastcall GameCourtSetStadiumNameHook08(
        void* gameCourt,
        void*,
        const char* stadiumId4)
    {
        BeginNewGameStadiumLoad();
        UpdateGameContext(stadiumId4, nullptr);
        CallMethodDynGlobal(
            kLive08GameCourtSetStadiumName,
            gameCourt,
            stadiumId4);
    }

    void InstallLive06PlayoffBracketCaptureHook()
    {
        const unsigned int destructorCallSite = 0x005491B3;
        if (!ValidateCallSites(
                &destructorCallSite,
                1,
                0x0051E700))
        {
            OutputDebugStringA(
                "[CustomPaths] NBA Live 06 playoff-bracket capture hook "
                "validation failed.\n");
            return;
        }

        patch::RedirectCall(
            destructorCallSite,
            Live06PlayoffBracketDestructorHook);
        OutputDebugStringA(
            "[CustomPaths] NBA Live 06 playoff-bracket capture hook "
            "installed.\n");
    }

    bool InstallLive06AccessorEntryHook(
        uintptr_t address,
        const unsigned char* expected,
        void* hook)
    {
        const size_t patchLength = 5;
        unsigned char* target =
            reinterpret_cast<unsigned char*>(address);

        __try
        {
            if (std::memcmp(target, expected, patchLength) != 0)
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                target,
                patchLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;
        *reinterpret_cast<int32_t*>(target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(hook) - (address + 5));

        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            patchLength);
        return true;
    }

    void InstallLive06PlayoffAccessorHooks()
    {
        const unsigned char expectedInteger[5] = {
            0x8B, 0x44, 0x24, 0x08, 0x48
        };
        const unsigned char expectedBoolean[5] = {
            0x8B, 0x54, 0x24, 0x08, 0x32
        };

        const bool integerInstalled = InstallLive06AccessorEntryHook(
            0x004EA3D0,
            expectedInteger,
            reinterpret_cast<void*>(&Live06IntegerSettingGetterHook));
        const bool booleanInstalled = InstallLive06AccessorEntryHook(
            0x004EA420,
            expectedBoolean,
            reinterpret_cast<void*>(&Live06BooleanSettingGetterHook));

        if (integerInstalled && booleanInstalled)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Forced playoff SettingsObject accessors "
                "installed.\n");
        }
        else
        {
            WritePlayoffStateLog(
                "[CustomPaths] Forced playoff SettingsObject accessor "
                "validation failed.\n");
        }
    }

    void InstallLive08ContextHook()
    {
        const size_t count = sizeof(kLive08GameCourtNameCalls) /
            sizeof(kLive08GameCourtNameCalls[0]);
        if (!ValidateCallSites(
                kLive08GameCourtNameCalls,
                count,
                kLive08GameCourtSetStadiumName))
        {
            OutputDebugStringA(
                "[CustomPaths] NBA Live 08 context-hook validation failed.\n");
            return;
        }

        for (size_t i = 0; i < count; ++i)
            patch::RedirectCall(
                kLive08GameCourtNameCalls[i],
                GameCourtSetStadiumNameHook08);
    }

    void Initialize()
    {
        InitializeForcedStageSettings();

        switch (FM::GetEntryPoint())
        {
        case 0xCD8005: // NBA Live 2005 1.0 NOCD
            gGameVersion = GAME_LIVE_2005;
            patch::RedirectCall(0x005D8AF1, AddCustomPaths05);
            InstallOpenResolvedGate(0x006C5E6D);
            break;

        case 0x40109F:
            if (patch::GetFloat(0xBD832C) == 1.3333334f)
            {
                // NBA Live 06 1.0 NOCD
                gGameVersion = GAME_LIVE_06;
                patch::RedirectCall(0x005E756C, AddCustomPaths06);
                InstallOpenResolvedGate(0x006DC8F9);
#if defined(_M_IX86)
                InstallLive06BenchPoseHooks();
                InstallLive06BenchPositionHooks();
#endif
            }
            else if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
            {
                // NBA Live 07 1.1 NOCD
                gGameVersion = GAME_LIVE_07;
                patch::RedirectCall(0x0063A19E, AddCustomPaths07);
                InstallOpenResolvedGate(0x007FF4CE);
            }
            else if (patch::GetFloat(0xC3DF84) == 1.3333334f)
            {
                // NBA Live 08 1.0 NOCD
                gGameVersion = GAME_LIVE_08;
                patch::RedirectCall(0x00659E0E, AddCustomPaths08);
                InstallOpenResolvedGate(0x0082E4ED);
                InstallLive08ContextHook();
            }
            break;
        }

        InitializePlayoffStateLog();

#if defined(_M_IX86)
        if (gGameVersion == GAME_LIVE_2005)
        {
            if (gPlayoffStateDebug)
                WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                    ? "[CustomPaths] Live 2005 speech-event trace installed (70DDB0).\n"
                    : "[CustomPaths] Live 2005 speech-event trace validation failed.\n");
            if (Live05ForcedSpeechEnabled())
            {
                // Redirect only queries from the native speech routines;
                // frontend/tournament queries retain their original results.
                const unsigned int queryCalls[] = {
                    0x0043BF88, 0x0044CDE5, 0x0045BB15
                };
                const unsigned int seriesCalls[] = { 0x0044CE02, 0x0045BB35 };
                const bool valid = ValidateCallSites(queryCalls, 3, 0x00539C40) &&
                    ValidateCallSites(seriesCalls, 2, 0x004EC880);
                const bool installed = valid && InstallLive06PregameContextHook();
                if (installed)
                {
                    for (size_t i = 0; i < 3; ++i)
                        patch::RedirectCall(queryCalls[i], Live05SpeechPlayoffQueryHook);
                    for (size_t i = 0; i < 2; ++i)
                        patch::RedirectCall(seriesCalls[i], Live05SpeechSeriesQueryHook);
                }
                WritePlayoffStateLog(installed
                    ? "[CustomPaths] Live 2005 forced speech-context hooks installed "
                      "(pregame=45B5F0 query=539C40 series=4EC880).\n"
                    : "[CustomPaths] Live 2005 forced speech-context validation failed.\n");
            }
        }
        if (gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08)
        {
            const bool live08 = gGameVersion == GAME_LIVE_08;
            if (gPlayoffStateDebug)
                WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                    ? "[CustomPaths] Live 07/08 speech-event trace installed.\n"
                    : "[CustomPaths] Live 07/08 speech-event trace validation failed.\n");
            if (gForcedSeriesEnabled && gForcedStageEnabled && gForceGlobalGameMode)
            {
                // 08's init thunk jumps to relocated native code at E2F38C.
                const unsigned int calls[] = { live08 ? 0x006632FDu : 0x00643BC6u };
                const unsigned int initializer = live08 ? 0x004642B0u : 0x004617F0u;
                const bool initValid = ValidateCallSites(calls, 1, initializer);
                const bool pregameInstalled = initValid && InstallLive06PregameContextHook();
                if (pregameInstalled)
                {
                    gLive06PlayoffCacheInitializer = initializer;
                    patch::RedirectCall(calls[0], Live06PlayoffCacheInitializerHook);
                }
                WritePlayoffStateLog(pregameInstalled
                    ? (live08
                        ? "[CustomPaths] Live 08 forced speech-context hooks installed (init=4642B0 pregame=48ECE0).\n"
                        : "[CustomPaths] Live 07 forced speech-context hooks installed (init=4617F0 pregame=47C430).\n")
                    : "[CustomPaths] Live 07/08 forced speech-context validation failed.\n");
            }
        }
        // Install independently of force settings, allowing an unmodified
        // real playoff launch to supply the comparison baseline.
        if (gGameVersion == GAME_LIVE_06 && gPlayoffStateDebug)
        {
            WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                ? "[CustomPaths] Playoff speech-event dispatch trace installed.\n"
                : "[CustomPaths] Playoff speech-event dispatch trace validation "
                  "or installation failed.\n");
            WritePlayoffStateLog(InstallLive06SetupHandoffTrace()
                ? "[CustomPaths] Playoff GameSetup handoff trace installed.\n"
                : "[CustomPaths] Playoff GameSetup handoff trace validation "
                  "or installation failed.\n");
            const unsigned int introCall[] = { 0x0046018F };
            const bool valid = ValidateCallSites(introCall, 1, 0x00434610);
            if (valid)
                patch::RedirectCall(introCall[0], Live06IntroEventTraceHook);
            WritePlayoffStateLog(valid
                ? "[CustomPaths] Playoff live-intro argument trace installed.\n"
                : "[CustomPaths] Playoff live-intro argument trace validation "
                  "failed.\n");
        }
#endif

        if (gGameVersion == GAME_LIVE_06 &&
            gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
#if defined(_M_IX86)
            const unsigned int cacheInitCalls[] = { 0x005EFE82 };
            const bool cacheInitValid = ValidateCallSites(
                cacheInitCalls, 1, 0x0043C410);
            const bool pregameInstalled = InstallLive06PregameContextHook();
            if (cacheInitValid)
                patch::RedirectCall(cacheInitCalls[0], Live06PlayoffCacheInitializerHook);
            WritePlayoffStateLog(cacheInitValid && pregameInstalled
                ? "[CustomPaths] Forced playoff speech-context hooks installed "
                  "(pregame builder=0045D240).\n"
                : "[CustomPaths] Forced playoff speech-context hooks incomplete: "
                  "initialization or pregame validation failed.\n");
#endif
            // Unlike a real playoff launch, a promoted exhibition has no
            // ScheduleInfo object.  The game's SettingsObject accessors are
            // still used by presentation/announcer code, so force the series
            // values at that boundary and record exactly which consumers ask
            // for them.
            InstallLive06PlayoffAccessorHooks();

            const unsigned char expectedCommentaryParameter[5] = {
                0xA0, 0xB4, 0xF5, 0xCA, 0x00
            };
            const bool commentaryGetterInstalled =
                InstallLive06AccessorEntryHook(
                    0x00449550,
                    expectedCommentaryParameter,
                    reinterpret_cast<void*>(
                        &Live06CommentaryParameterGetterHook));
            const bool commentarySetterInstalled =
                InstallLive06AccessorEntryHook(
                    0x00449590,
                    expectedCommentaryParameter,
                    reinterpret_cast<void*>(
                        &Live06CommentaryParameterSetterHook));
            WritePlayoffStateLog(
                commentaryGetterInstalled && commentarySetterInstalled
                    ? "[CustomPaths] Playoff commentary-parameter trace "
                      "installed.\n"
                    : "[CustomPaths] Playoff commentary-parameter trace "
                      "validation failed.\n");

            // ScheduleInfo::GetGame is the presentation-facing accessor used
            // after the frontend/gameplay handoff. Hooking it lets announcer
            // and tournament presentation receive the requested schedule ID
            // even before the ScheduleInfo singleton has been constructed.
            const unsigned char expectedCurrentGameGetter[5] = {
                0xA1, 0xE0, 0x2A, 0xCB, 0x00
            };
            const bool installed = InstallLive06AccessorEntryHook(
                0x004E4380,
                expectedCurrentGameGetter,
                reinterpret_cast<void*>(
                    &Live06ForcedCurrentGameGetterHook));
            WritePlayoffStateLog(
                installed
                    ? "[CustomPaths] Forced playoff schedule getter "
                      "installed.\n"
                    : "[CustomPaths] Forced playoff schedule getter "
                      "validation failed.\n");

            const size_t selectionCallCount =
                sizeof(kLive06ScheduleSelectionCalls) /
                sizeof(kLive06ScheduleSelectionCalls[0]);
            const bool selectionHooksValid = ValidateCallSites(
                kLive06ScheduleSelectionCalls,
                selectionCallCount,
                0x004F0250);
            if (selectionHooksValid)
            {
                for (size_t i = 0; i < selectionCallCount; ++i)
                {
                    patch::RedirectCall(
                        kLive06ScheduleSelectionCalls[i],
                        Live06ScheduleSelectionHook);
                }
            }
            WritePlayoffStateLog(
                selectionHooksValid
                    ? "[CustomPaths] Forced playoff schedule-selection "
                      "hooks installed.\n"
                    : "[CustomPaths] Forced playoff schedule-selection "
                      "hook validation failed.\n");
        }

        // The temporary generic SettingsObject accessor trace is no longer
        // installed. It established that only PLAYOFF_LENGTH is consumed by
        // the normal settings-copy path; the live clinching flags reside in
        // RulesSettings and are logged without executable entry hooks.

    }
}

// Called by CustomCamera's existing 2005/06/07 GameCourt hooks. Keeping one
// owner for those call sites prevents the two launcher features from replacing
// each other's hooks.
void UpdateCustomPathsGameContext(const char* stadiumId4, void* options)
{
    custompaths::UpdateGameContext(stadiumId4, options);
}

// Called once at the pre-Setup stadium handoff for each game session.
// The post-Setup context update preserves the newly loaded JSON/cache.
void BeginCustomPathsGameLoad()
{
    custompaths::BeginNewGameStadiumLoad();
}

void InitializeCustomCamera(); // Legacy entrypoint retained for compatibility.

void InitializeCustomPaths()
{
    custompaths::Initialize();
    InitializeCustomCamera();
}


// -----------------------------------------------------------------------------
// Stadium camera script selection and GameCourt context capture.
// Merged from CustomCamera.cpp.
// Keep the legacy InitializeCustomCamera symbol until all launcher call sites
// have been removed; InitializeCustomPaths initializes it directly.
// -----------------------------------------------------------------------------

bool ResolveStadiumCameraPath(
    const char* stadiumId,
    char* outPath,
    size_t outPathSize);

void UpdateCustomPathsGameContext(
    const char* stadiumId4,
    void* options);

namespace {

constexpr unsigned int kLive2005EntryPoint = 0x00CD8005;
constexpr unsigned int kLive060708EntryPoint = 0x0040109F;
constexpr unsigned int kLive06AspectRatio = 0x00BD832C;
constexpr unsigned int kLive07AspectRatio = 0x00BBBC3C;
constexpr float kStandardAspectRatio = 1.3333334f;

const char kDefaultCameraScript[] = "merlin\\nbacam.mgd";
char gStadiumId4[5] = {};
char gCustomCameraScript[MAX_PATH] = {};

const unsigned int kLive2005GameCourtSetupCalls[] = {
    0x0060A0A0, 0x0060A0B3, 0x0060A15C, 0x0060A2D1,
    0x0060A379, 0x0060A746, 0x0060ADDE, 0x0060AFE1,
    0x0060B06F, 0x0060B38D, 0x0060C40F, 0x0060D536,
    0x0060D56B, 0x0060E357, 0x0060E39A
};

const unsigned int kLive2005CameraLoadCalls[] = {
    0x00620F1D, 0x00621632, 0x00621912, 0x00621C02,
    0x00622206, 0x00622BA6, 0x006233F2, 0x00623C22
};

const unsigned int kLive06GameCourtSetupCalls[] = {
    0x00617F10, 0x00617F23, 0x00617FCC, 0x00618461,
    0x00618508, 0x00619391, 0x0061941E, 0x0061B146,
    0x0061B17B, 0x0061CD2E, 0x0061D2E2, 0x0061DC80,
    0x0061E059, 0x0061F0BB
};

const unsigned int kLive06CameraLoadCalls[] = {
    0x0063071D, 0x00630E32, 0x00631112, 0x00631438,
    0x00631A48, 0x00632418, 0x00632C38, 0x006333F2
};

// NBA Live 07 stores the stadium abbreviation through a small GameCourt
// setter instead of passing it through the NBA Live 05/06 Setup signature.
const unsigned int kLive07GameCourtNameCalls[] = {
    0x0064EDE7, 0x006504D2, 0x00650B42, 0x006515A2,
    0x00655CF1, 0x006567E1, 0x00656D83, 0x00657053
};

const unsigned int kLive07CameraLoadCalls[] = {
    0x0066721D, 0x00667932, 0x00667C12, 0x00667F28,
    0x00668548, 0x00668F18, 0x00669738, 0x00669F02
};

enum StadiumCaptureKind {
    CAPTURE_GAMECOURT_SETUP_0506,
    CAPTURE_GAMECOURT_NAME_07
};

struct CustomCameraBuild {
    const char* name;
    StadiumCaptureKind stadiumCaptureKind;
    unsigned int gameCourtSetup;
    unsigned int cameraManagerLoadScripts;
    unsigned int nbaCameraScriptSlot;
    const unsigned int* gameCourtSetupCalls;
    size_t gameCourtSetupCallCount;
    const unsigned int* cameraLoadCalls;
    size_t cameraLoadCallCount;
};

const CustomCameraBuild kLive2005Build = {
    "NBA Live 2005",
    CAPTURE_GAMECOURT_SETUP_0506,
    0x00608A00,
    0x006251A0,
    0x00B640CC,
    kLive2005GameCourtSetupCalls,
    sizeof(kLive2005GameCourtSetupCalls) /
        sizeof(kLive2005GameCourtSetupCalls[0]),
    kLive2005CameraLoadCalls,
    sizeof(kLive2005CameraLoadCalls) / sizeof(kLive2005CameraLoadCalls[0])
};

const CustomCameraBuild kLive06Build = {
    "NBA Live 06",
    CAPTURE_GAMECOURT_SETUP_0506,
    0x00616D60,
    0x00634980,
    0x00BCA4E4,
    kLive06GameCourtSetupCalls,
    sizeof(kLive06GameCourtSetupCalls) /
        sizeof(kLive06GameCourtSetupCalls[0]),
    kLive06CameraLoadCalls,
    sizeof(kLive06CameraLoadCalls) / sizeof(kLive06CameraLoadCalls[0])
};

const CustomCameraBuild kLive07Build = {
    "NBA Live 07",
    CAPTURE_GAMECOURT_NAME_07,
    0x0067B710,
    0x0066B4C0,
    0x00BEC128,
    kLive07GameCourtNameCalls,
    sizeof(kLive07GameCourtNameCalls) /
        sizeof(kLive07GameCourtNameCalls[0]),
    kLive07CameraLoadCalls,
    sizeof(kLive07CameraLoadCalls) /
        sizeof(kLive07CameraLoadCalls[0])
};

const CustomCameraBuild* gBuild = nullptr;

const CustomCameraBuild* DetectBuild() {
    const unsigned int entryPoint = FM::GetEntryPoint();

    if (entryPoint == kLive2005EntryPoint)
        return &kLive2005Build;

    if (entryPoint == kLive060708EntryPoint &&
        patch::GetFloat(kLive06AspectRatio) == kStandardAspectRatio) {
        return &kLive06Build;
    }

    if (entryPoint == kLive060708EntryPoint &&
        patch::GetFloat(kLive07AspectRatio) == kStandardAspectRatio) {
        return &kLive07Build;
    }

    return nullptr;
}

bool IsValidStadiumId4(const char* value) {
    if (!value)
        return false;

    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '\0' || !std::isalnum(c))
            return false;
    }

    return value[4] == '\0';
}

void CacheStadiumId4(const char* value) {
    if (!IsValidStadiumId4(value)) {
        gStadiumId4[0] = '\0';
        return;
    }

    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        gStadiumId4[i] = static_cast<char>(std::tolower(c));
    }
    gStadiumId4[4] = '\0';
}

bool FileExistsBesideExecutable(const char* relativePath) {
    char absolutePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, absolutePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char* separator = std::strrchr(absolutePath, '\\');
    if (!separator)
        return false;

    separator[1] = '\0';
    const size_t used = std::strlen(absolutePath);
    const size_t available = MAX_PATH - used;
    if (std::strlen(relativePath) + 1 > available)
        return false;

    std::strcat(absolutePath, relativePath);
    const DWORD attributes = GetFileAttributesA(absolutePath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void SelectCameraScript() {
    const char* selectedPath = kDefaultCameraScript;

    if (gStadiumId4[0] != '\0') {
        const bool hasConfiguredCamera =
            ResolveStadiumCameraPath(
                gStadiumId4,
                gCustomCameraScript,
                sizeof(gCustomCameraScript));

        if (!hasConfiguredCamera) {
            sprintf_s(
                gCustomCameraScript,
                sizeof(gCustomCameraScript),
                "assets\\camera\\%s.mgd",
                gStadiumId4);
            gCustomCameraScript[sizeof(gCustomCameraScript) - 1] = '\0';
        }

        if (FileExistsBesideExecutable(gCustomCameraScript))
            selectedPath = gCustomCameraScript;
    }

    patch::SetPointer(gBuild->nbaCameraScriptSlot,
                      const_cast<char*>(selectedPath));
}

// Shared NBA Live 2005/06 signature:
// GameCourt::Setup(const char*, const char*, CameraGlobal*, OptionsSettings*,
//                  bool, bool, const char*)
void __fastcall GameCourtSetupHook(
    void* gameCourt,
    void*,
    const char* ubiPath,
    const char* stadiumId4,
    void* camera,
    void* options,
    bool arg5,
    bool arg6,
    const char* arg7) {
    BeginCustomPathsGameLoad();
    CacheStadiumId4(stadiumId4);
    UpdateCustomPathsGameContext(stadiumId4, options);

    CallMethodDynGlobal(
        gBuild->gameCourtSetup,
        gameCourt,
        ubiPath,
        stadiumId4,
        camera,
        options,
        arg5,
        arg6,
        arg7);

    // GameCourt::Setup can populate the playoff schedule service and can
    // rewrite OptionsSettings after the pre-call context pass. Apply the
    // context again so forced mode, round, series game, and clinching flags
    // are committed to the final native state. The CustomPaths cache keeps
    // the repeated asset-routing work inexpensive.
    UpdateCustomPathsGameContext(stadiumId4, options);
}

// NBA Live 07: GameCourt::SetStadiumName(const char* stadiumId4).
// The original copies the zero-terminated ID into GameCourt + 0x3BC.
void __fastcall GameCourtSetStadiumNameHook07(
    void* gameCourt,
    void*,
    const char* stadiumId4) {
    BeginCustomPathsGameLoad();
    CacheStadiumId4(stadiumId4);
    UpdateCustomPathsGameContext(stadiumId4, nullptr);
    CallMethodDynGlobal(
        gBuild->gameCourtSetup,
        gameCourt,
        stadiumId4);
}

// CamManager::LoadCameraScripts(int mode)
void __fastcall CameraLoadHook(void* cameraManager, void*, int mode) {
    SelectCameraScript();
    CallMethodDynGlobal(
        gBuild->cameraManagerLoadScripts, cameraManager, mode);
}

bool ValidateCallSites(
    const unsigned int* sites,
    size_t count,
    unsigned int expectedTarget) {
    for (size_t i = 0; i < count; ++i) {
        const unsigned int callAddress = sites[i];
        if (patch::GetUChar(callAddress) != 0xE8)
            return false;

        const int displacement = patch::GetInt(callAddress + 1);
        const unsigned int target = callAddress + 5 + displacement;
        if (target != expectedTarget)
            return false;
    }
    return true;
}

void RedirectCalls(const unsigned int* sites, size_t count, void* hook) {
    for (size_t i = 0; i < count; ++i)
        patch::RedirectCall(sites[i], hook);
}

} // namespace

void InitializeCustomCamera() {
    // May be called both by InitializeCustomPaths and a legacy launcher entry.
    // Never rewire the already redirected native callsites twice.
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    gBuild = DetectBuild();
    if (!gBuild)
        return;

    // Refuse the whole patch if this is not the exact executable revision.
    // This prevents a partial installation when an address belongs to a
    // different build.
    if (!ValidateCallSites(
            gBuild->gameCourtSetupCalls,
            gBuild->gameCourtSetupCallCount,
            gBuild->gameCourtSetup) ||
        !ValidateCallSites(
            gBuild->cameraLoadCalls,
            gBuild->cameraLoadCallCount,
            gBuild->cameraManagerLoadScripts)) {
        OutputDebugStringA(
            "[CustomCamera] Call-site validation failed.\n");
        gBuild = nullptr;
        return;
    }

    gStadiumId4[0] = '\0';
    patch::SetPointer(gBuild->nbaCameraScriptSlot,
                      const_cast<char*>(kDefaultCameraScript));
    void* stadiumCaptureHook =
        gBuild->stadiumCaptureKind == CAPTURE_GAMECOURT_NAME_07
            ? reinterpret_cast<void*>(&GameCourtSetStadiumNameHook07)
            : reinterpret_cast<void*>(&GameCourtSetupHook);

    RedirectCalls(
                  gBuild->gameCourtSetupCalls,
                  gBuild->gameCourtSetupCallCount,
                  stadiumCaptureHook);
    RedirectCalls(
                  gBuild->cameraLoadCalls,
                  gBuild->cameraLoadCallCount,
                  reinterpret_cast<void*>(&CameraLoadHook));

    char message[96] = {};
    sprintf_s(
        message,
        sizeof(message),
        "[CustomCamera] Initialized for %s.\n",
        gBuild->name);
    OutputDebugStringA(message);
}
