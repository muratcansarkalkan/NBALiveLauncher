#include "plugin-std.h"
#include "StageSetup.h"
#include "ShotClock.h"
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <intrin.h>
#include "GameStageInternal.h"
#include "Games.h"
#include <algorithm>
#include <cstdarg>

using namespace plugin;

namespace custompaths {
BenchSlotConfig gBenchSlots[2][7] = {};

bool gBenchHasPositionRequests = false;

uintptr_t gBenchActorRecords[2][7] = {};
// Read-only identity evidence: scene factory PLAYERID may differ from runtime native ID.
int32_t gBenchFactoryIds[2][7] = {};
struct BenchAnchor { bool seen=false; float pos[3]={}; bool rejected=false; };
BenchAnchor gBenchAnchors[2][7] = {};
struct BenchSpacing { bool enabled=false; float scale=1.0f; };
BenchSpacing gBenchSpacing[2] = {};
static bool gBenchLoggedIdentity[2][7] = {};
static bool gBenchLoggedApplied[2][7] = {};
static DWORD gBenchLastProbeTick[2][7] = {};
static int gBenchLastDbMatchCount[2][7] = {};
static int gBenchLastTrackedCourtState[2][7] = {};
static bool gBenchDbMatchLogged[2][7] = {};
static bool gBenchReadySpacing[2] = {};
static float gBenchSpacingTargets[2][7][3] = {};
// Full 12-player scene registry. 0..4 = C,PF,PG,SF,SG; 5..11 = bench_1..7.
// These indices describe scene paths only, NOT current seat assignments.
static uintptr_t gBenchAllRecords[2][12] = {};
static int32_t gBenchAllDbIds[2][12] = {};
static bool gBenchFullActorSetLogged = false;
static int gBenchLastSeatByActor[2][12] = {};
static float gBenchSeatWorld[2][7][3] = {};
static bool gBenchSeatWorldReady[2] = {};
static DWORD gBenchLastAssignmentTick = 0;
static int gBenchAssignment[2][12] = {}; // scene actor -> seat, -1 active/unavailable
static bool gBenchAssignmentValid[2] = {};
static int gBenchLastAssignedCount[2] = {};
static int gBenchLastCapturedCount = -1;
// Replay/pause can stop ShotClock polling for >3s. Preserve the last valid
// seat ownership when the cache disappears; do not temporarily fall back to
// original bench_N positioning.
static int gBenchPendingAssignment[2][12] = {};
static DWORD gBenchPendingSourceTick[2] = {};
static bool gBenchPendingAssignmentValid[2] = {};
static DWORD gBenchAcceptedLineupEventRevision[2] = {};



// Always-on for this diagnostic version. Paths are fixed independently of main.ini.
static bool BenchLogAt(const char* path, const char* line) {
    if (!path || !*path) return false;
    FILE* out = nullptr;
    if (fopen_s(&out, path, "ab") || !out) return false;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(out, "[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, line ? line : "");
    const bool ok = (ferror(out) == 0);
    fclose(out);
    return ok;
}
static void BenchLog(const char* fmt, ...) {
    char line[512] = {};
    va_list args; va_start(args, fmt);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    va_end(args);
    char temp[MAX_PATH] = {};
    DWORD len = GetTempPathA(MAX_PATH, temp);
    bool wrote = false;
    if (len && len < MAX_PATH && strlen(temp) + 30 < MAX_PATH) {
        strcat_s(temp, "NBALive06_Bench_Identity.log");
        wrote = BenchLogAt(temp, line);
    }
    char exe[MAX_PATH] = {};
    len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len && len < MAX_PATH) {
        char* slash = strrchr(exe, '\\');
        if (slash && static_cast<size_t>(slash - exe) + 29 < MAX_PATH) {
            slash[1] = 0;
            strcat_s(exe, "bench_identity_trace.log");
            wrote = BenchLogAt(exe, line) || wrote;
        }
    }
    char debug[600] = {};
    sprintf_s(debug, "[BenchID] %s%s\n", line, wrote ? "" : " [FILE_WRITE_FAILED]");
    OutputDebugStringA(debug);
}
static void ResetBenchIdentity() {
    memset(gBenchActorRecords, 0, sizeof(gBenchActorRecords));
    memset(gBenchFactoryIds, 0xFF, sizeof(gBenchFactoryIds));
    memset(gBenchAnchors, 0, sizeof(gBenchAnchors));
    memset(gBenchLoggedIdentity, 0, sizeof(gBenchLoggedIdentity));
    memset(gBenchLoggedApplied, 0, sizeof(gBenchLoggedApplied));
    memset(gBenchLastProbeTick, 0, sizeof(gBenchLastProbeTick));
    memset(gBenchLastDbMatchCount, 0, sizeof(gBenchLastDbMatchCount));
    memset(gBenchLastTrackedCourtState, 0, sizeof(gBenchLastTrackedCourtState));
    memset(gBenchDbMatchLogged, 0, sizeof(gBenchDbMatchLogged));
    memset(gBenchReadySpacing, 0, sizeof(gBenchReadySpacing));
    memset(gBenchSpacingTargets, 0, sizeof(gBenchSpacingTargets));
    memset(gBenchAllRecords, 0, sizeof(gBenchAllRecords));
    memset(gBenchAllDbIds, 0xFF, sizeof(gBenchAllDbIds));
    memset(gBenchSeatWorld, 0, sizeof(gBenchSeatWorld));
    memset(gBenchSeatWorldReady, 0, sizeof(gBenchSeatWorldReady));
    gBenchFullActorSetLogged = false;
    gBenchLastAssignmentTick = 0;
    gBenchLastCapturedCount = -1;
    memset(gBenchPendingAssignment, 0xFF, sizeof(gBenchPendingAssignment));
    memset(gBenchPendingSourceTick, 0, sizeof(gBenchPendingSourceTick));
    memset(gBenchPendingAssignmentValid, 0, sizeof(gBenchPendingAssignmentValid));
    memset(gBenchAcceptedLineupEventRevision, 0, sizeof(gBenchAcceptedLineupEventRevision));
    for (int side=0; side<2; ++side) {
        gBenchAssignmentValid[side] = false;
        gBenchLastAssignedCount[side] = -1;
        for (int i=0; i<12; ++i) {
            gBenchAssignment[side][i] = -1;
            gBenchLastSeatByActor[side][i] = -1;
        }
    }

    BenchLog("GAME_RESET bench actor registry cleared");
}
static void BenchLogIdMatches(int side,int slot) {
    if (gBenchLoggedIdentity[side][slot]) return;
    const int32_t id = gBenchFactoryIds[side][slot];
    __try {
        const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(0x00CB1D30);
        if (!manager) return;  // May not be initialized at scene construction.
        int hits = 0;
        char candidates[192] = {};
        for (int i=0;i<24;++i) {
            const uint32_t nativeId = *reinterpret_cast<const uint32_t*>(manager+8+i*4);
            if (static_cast<uint32_t>(id) == nativeId) {
                char part[24] = {};
                sprintf_s(part, "%s%d", hits ? "," : "", i);
                strcat_s(candidates, part);
                ++hits;
            }
        }
        BenchLog("ID_COMPARE side=%d slot=%d factory_PLAYERID=%d matches=%d managerSlots=%s manager=%08X",
            side,slot+1,id,hits,hits ? candidates : "NONE",static_cast<unsigned>(manager));
        gBenchLoggedIdentity[side][slot] = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        BenchLog("ID_COMPARE_EXCEPTION side=%d slot=%d factory_PLAYERID=%d",side,slot+1,id);
    }
}
static void BuildBenchSpacing(int side) {
    if (gBenchReadySpacing[side] || !gBenchSpacing[side].enabled) return;
    for (int i=0;i<7;++i) if (!gBenchAnchors[side][i].seen) return;
    const float* a = gBenchAnchors[side][0].pos;
    const float* b = gBenchAnchors[side][6].pos;
    const float dx=b[0]-a[0], dz=b[2]-a[2];
    const float span=sqrtf(dx*dx+dz*dz);
    if (!std::isfinite(span) || span < 0.5f) return;
    const float ux=dx/span, uz=dz/span,step=span/6.0f;
    for(int i=1;i<7;++i) {
        const float* p=gBenchAnchors[side][i-1].pos;
        const float* q=gBenchAnchors[side][i].pos;
        const float forward=(q[0]-p[0])*ux+(q[2]-p[2])*uz;
        if (forward < step*0.1f || forward > step*3.0f) return;
    }
    for(int i=0;i<7;++i) {
        const float* p=gBenchAnchors[side][i].pos;
        const float* center=gBenchAnchors[side][3].pos;
        gBenchSpacingTargets[side][i][0]=center[0]+(i-3)*step*gBenchSpacing[side].scale*ux-p[0];
        gBenchSpacingTargets[side][i][2]=center[2]+(i-3)*step*gBenchSpacing[side].scale*uz-p[2];
    }
    gBenchReadySpacing[side]=true;
    BenchLog("SPACING_READY side=%d scale=%.3f nativeStep=%.3f",side,gBenchSpacing[side].scale,step);
}


class BenchJson
    {
    public:
        explicit BenchJson(const std::string& contents) : text(contents) {}
        bool Parse(BenchSlotConfig (&slots)[2][7], BenchSpacing (&spacing)[2], bool& hasPosition)
        {
            hasPosition = false;
            if (!ObjectRoot(slots, spacing, hasPosition)) return false;
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
        bool Side(BenchSlotConfig (&slots)[7], BenchSpacing& spacing, bool& hasPosition)
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
                } else if (k=="spacing") {
                    if (!Number(spacing.scale) || spacing.scale<0.7f || spacing.scale>2.0f) return false;
                    spacing.enabled=true; hasPosition=true;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(',')); return false;
        }
        bool BenchPlayers(BenchSlotConfig (&slots)[2][7], BenchSpacing (&spacing)[2], bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string key; if (!Str(key)||!Take(':')) return false;
                if (key=="home" || key=="away") {
                    if (!Side(slots[key=="away"?1:0],spacing[key=="away"?1:0],hasPosition)) return false;
                } else if (!Skip()) return false;
                if (Take('}')) return true;
            } while (Take(','));
            return false;
        }
        bool ObjectRoot(BenchSlotConfig (&slots)[2][7], BenchSpacing (&spacing)[2], bool& hasPosition)
        {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string key; if (!Str(key)||!Take(':')) return false;
                if (key=="benchPlayers") {
                    if (!BenchPlayers(slots,spacing,hasPosition)) return false;
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
        gBenchSpacing[0] = {}; gBenchSpacing[1] = {};
        if (!id4 || !*id4) return;
        // Use the executable's directory, not the unpredictable process CWD.
        char filename[MAX_PATH]={};
        char exe[MAX_PATH]={};
        const DWORD exeLen=GetModuleFileNameA(nullptr,exe,MAX_PATH);
        if (!exeLen || exeLen>=MAX_PATH) { BenchLog("CONFIG_PATH_ERROR GetModuleFileName failed error=%lu",GetLastError()); return; }
        char* slash=strrchr(exe,'\\');
        if (!slash) { BenchLog("CONFIG_PATH_ERROR no directory separator"); return; }
        slash[1]='\0';
        if (sprintf_s(filename,"%sassets\\stadia\\%s.json",exe,id4)<=0) return;
        std::ifstream file(filename,std::ios::binary);
        if (!file.good()) { BenchLog("CONFIG_NOT_FOUND stadium=%s path=%s",id4,filename); return; }
        std::ostringstream buffer; buffer<<file.rdbuf();
        const std::string json=buffer.str();
        if (json.size()>1048576) {OutputDebugStringA("[CustomPaths] stadium JSON too large for bench settings; ignored.\n");return;}
        BenchSlotConfig proposed[2][7] = {};
        BenchSpacing proposedSpacing[2] = {};
        bool positionRequests=false;
        BenchJson reader(json);
        if (!reader.Parse(proposed,proposedSpacing,positionRequests)) {
            OutputDebugStringA("[CustomPaths] Invalid stadium JSON or benchPlayers; original bench settings retained.\n"); return;
        }
        std::memcpy(gBenchSlots,proposed,sizeof(gBenchSlots));
        std::memcpy(gBenchSpacing,proposedSpacing,sizeof(gBenchSpacing));
        gBenchHasPositionRequests=positionRequests;
        gBenchSeatWorldReady[0] = gBenchSeatWorldReady[1] = false;
        gBenchAssignmentValid[0] = gBenchAssignmentValid[1] = false;
        BenchLog("CONFIG_LOADED stadium=%s manualOffsets=%d spacingHome=%d spacingAway=%d path=%s",
            id4,positionRequests,gBenchSpacing[0].enabled,gBenchSpacing[1].enabled,filename);
        BenchLog("FIXED_SEAT_MODE v6.2: fast confirmed substitution event, two-refresh replay-stable fallback");
        for(int side=0;side<2;++side) for(int slot=0;slot<7;++slot) {
            const BenchSlotConfig& c=gBenchSlots[side][slot];
            BenchLog("CONFIG_SLOT side=%d slot=%d hasOffset=%d offset=(%.3f,%.3f,%.3f)",
                side,slot+1,c.hasOffset,c.offset[0],c.offset[1],c.offset[2]);
        }
        OutputDebugStringA("[CustomPaths] Read benchPlayers from assets\\stadia\\<id4>.json.\n");
        if (positionRequests)
            OutputDebugStringA("[CustomPaths] Bench position offsets loaded; experimental pre-propagation root transform hook enabled.\n");
    }

#if defined(_M_IX86)
static uintptr_t gBenchStartContinue=0x005ED0C9;

static uintptr_t gBenchReplayContinue=0x005ED45F;

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

void __cdecl BenchFactoryCapture(uintptr_t record, uintptr_t name,
        uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5,
        uintptr_t a6, uintptr_t a7, uintptr_t a8)
    {
        kBenchFactory(record,name,a2,a3,a4,a5,a6,a7,a8);
        if (!record || !name) return;
        // Native scene names may begin with '/', as in 5on5.xml.
        // Capture the actual argument before trying to classify it.
        char rawPath[192] = {};
        __try {
            const char* p = reinterpret_cast<const char*>(name);
            size_t i=0;
            for (; i+1<sizeof(rawPath) && p[i]; ++i) rawPath[i]=p[i];
            rawPath[i]=0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            BenchLog("FACTORY_RAW record=%08X namePointer=%08X unreadable", static_cast<unsigned>(record), static_cast<unsigned>(name));
            return;
        }
        BenchLog("FACTORY_RAW record=%08X name=%s factory_PLAYERID=%d", static_cast<unsigned>(record), rawPath, static_cast<int32_t>(a2));
        const char* path = rawPath;
        while (*path == '/' || *path == '\\') ++path;
        const char* home = "scene/homeTeam/bench_";
        const char* away = "scene/awayTeam/bench_";
        int side=-1; const char* suffix=nullptr;
        if (strncmp(path,home,strlen(home))==0) {side=0;suffix=path+strlen(home);}
        else if (strncmp(path,away,strlen(away))==0) {side=1;suffix=path+strlen(away);}
        // Factory calls for five starters are not in the 14 known bench sites.
        // Other direct callsites to this same factory are discovered at startup.
        // Never register the coach: only twelve exact team/player scene names.
        const char* teamRoot = nullptr;
        if (strncmp(path, "scene/homeTeam/", 15) == 0) {
            teamRoot = path + 15;
            if (side < 0) side = 0;
        } else if (strncmp(path, "scene/awayTeam/", 15) == 0) {
            teamRoot = path + 15;
            if (side < 0) side = 1;
        }
        int allSlot = -1;
        if (teamRoot) {
            static const char* const starterNames[5] = {"c", "pf", "pg", "sf", "sg"};
            for (int i=0; i<5; ++i)
                if (strcmp(teamRoot, starterNames[i]) == 0) { allSlot=i; break; }
            if (allSlot < 0 && strncmp(teamRoot, "bench_", 6) == 0 &&
                teamRoot[6]>='1' && teamRoot[6]<='7' && teamRoot[7]==0)
                allSlot=5+(teamRoot[6]-'1');
        }
        if (side>=0 && allSlot>=0) {
            // First bench actor of a newly constructed home scene invalidates
            // the previous game's full actor registry. Factory registration
            // occurs before GameCourt::Setup, so the handoff cannot do this.
            if (side==0 && allSlot==5 && gBenchActorRecords[0][0] &&
                gBenchActorRecords[0][0]!=record) {
                BenchLog("ACTOR12_NEW_SCENE oldFirst=%08X newFirst=%08X",
                    static_cast<unsigned>(gBenchActorRecords[0][0]),
                    static_cast<unsigned>(record));
                ResetBenchIdentity();
            }
            gBenchAllRecords[side][allSlot] = record;
            gBenchAllDbIds[side][allSlot] = static_cast<int32_t>(a2);
            gBenchAssignmentValid[side] = false;
            gBenchPendingAssignmentValid[side] = false;
            if (allSlot>=5) gBenchSeatWorldReady[side] = false;
            BenchLog("ACTOR12_CAPTURE side=%d sceneIndex=%d dbid=%d record=%08X name=%s",
                side, allSlot, static_cast<int32_t>(a2), static_cast<unsigned>(record), path);
        }
        if (side>=0 && suffix && suffix[0]>='1' && suffix[0]<='7' && suffix[1]==0) {
            const int slot=suffix[0]-'1';
            gBenchActorRecords[side][slot]=record;
            gBenchFactoryIds[side][slot]=static_cast<int32_t>(a2);
            gBenchAnchors[side][slot] = {};
            gBenchLoggedIdentity[side][slot] = false;
            gBenchLoggedApplied[side][slot] = false;
            gBenchLastProbeTick[side][slot] = 0;
            gBenchDbMatchLogged[side][slot] = false;
            gBenchLastDbMatchCount[side][slot] = 0;
            gBenchLastTrackedCourtState[side][slot] = -1;
            gBenchReadySpacing[side]=false;
            BenchLog("FACTORY side=%d slot=%d record=%08X factory_PLAYERID=%d hex=%08X",
                side,slot+1,static_cast<unsigned>(record),static_cast<int32_t>(a2),static_cast<unsigned>(a2));
            BenchLogIdMatches(side,slot);
        }
    }

    // Observe the 24-player cache only; keep the existing spatial safety guard
    // as the sole court-displacement decision until lineup state is verified.
    static void BenchProbeDatabaseIdentity(int side, int slot)
    {
        const int dbId = gBenchFactoryIds[side][slot];
        if (dbId < 0) return;
        Live06BenchPlayerSnapshot snapshot = {};
        const int matches = ShotClockLookupLive06BenchPlayerByDbId(dbId, &snapshot);
        if (matches != 1) {
            if (gBenchDbMatchLogged[side][slot] &&
                gBenchLastDbMatchCount[side][slot] == matches) return;
            // A cold cache can have 0 matches. Do not lock out later retries.
            if (matches > 0) {
                BenchLog("DBID_MATCH_AMBIGUOUS side=%d sceneSlot=%d factoryId=%d matches=%d",
                         side, slot+1, dbId, matches);
                gBenchDbMatchLogged[side][slot] = true;
            }
            gBenchLastDbMatchCount[side][slot] = matches;
            return;
        }
        const int courtState = snapshot.trackedOnCourt ? 1 : 0;
        if (gBenchDbMatchLogged[side][slot] &&
            gBenchLastDbMatchCount[side][slot] == matches &&
            gBenchLastTrackedCourtState[side][slot] == courtState) return;
        BenchLog("DBID_MATCH side=%d sceneSlot=%d factoryId=%d runtimeSide=%d "
                 "originalRosterSlot=%d nativeId=%u trackedOnCourt=%d "
                 "courtSlot=%d cacheAgeMs=%u trackerIsHeuristic=1",
                 side, slot+1, dbId, snapshot.side,
                 snapshot.originalRosterSlot, snapshot.nativePlayerId,
                 courtState, snapshot.courtSlot, snapshot.snapshotAgeMs);
        gBenchDbMatchLogged[side][slot] = true;
        gBenchLastDbMatchCount[side][slot] = matches;
        gBenchLastTrackedCourtState[side][slot] = courtState;
    }

// Generate fixed seat world targets from seven *native* anchors and the
// configured JSON differences. Those targets remain constant when players move.
static void BuildFixedSeatWorld(int side)
{
    if (gBenchSeatWorldReady[side]) return;
    for (int i=0; i<7; ++i) if (!gBenchAnchors[side][i].seen) return;
    const BenchSlotConfig& first = gBenchSlots[side][0];
    for (int i=0; i<7; ++i) {
        const BenchSlotConfig& config = gBenchSlots[side][i];
        for (int axis=0; axis<3; ++axis) {
            const float relative = (i>0 && config.hasOffset)
                ? config.offset[axis] - (first.hasOffset ? first.offset[axis] : 0.f)
                : 0.f;
            gBenchSeatWorld[side][i][axis] = gBenchAnchors[side][i].pos[axis] + relative;
        }
    }
    gBenchSeatWorldReady[side] = true;
    BenchLog("FIXED_SEATS_READY side=%d positions derived from seven native anchors + relative JSON", side);
}

// Read the existing ShotClock 24-player cache. Only operate when all 12 actor
// DBIDs resolve uniquely, exactly five are tracked active and seven inactive.
// The independent bench-region spatial guard below prevents court teleporting
// if ShotClock's substitution heuristic is late or incorrect.
static void RefreshFixedSeatAssignments()
{
    const DWORD now = GetTickCount();
    if (gBenchLastAssignmentTick && static_cast<DWORD>(now-gBenchLastAssignmentTick)<100u)
        return;
    gBenchLastAssignmentTick = now ? now : 1;
    int totalCaptured = 0;
    for (int side=0; side<2; ++side)
        for (int i=0; i<12; ++i) if (gBenchAllRecords[side][i]) ++totalCaptured;
    if (totalCaptured != gBenchLastCapturedCount) {
        BenchLog("ACTOR12_PROGRESS captured=%d of 24", totalCaptured);
        gBenchLastCapturedCount = totalCaptured;
    }
    for (int side=0; side<2; ++side) {
        // IMPORTANT: Never clear a working assignment because polling paused.
        // A replay can temporarily stop ShotClock refreshes and invalidate the
        // 3-second snapshot; that must not make original scene slots reappear.
        if (!gBenchSeatWorldReady[side]) continue;
        // An incomplete player set must NOT partially reassign seats.
        bool complete=true;
        for (int i=0; i<12; ++i)
            if (!gBenchAllRecords[side][i] || gBenchAllDbIds[side][i]<0)
                complete=false;
        if (!complete) continue;
        Live06BenchPlayerSnapshot players[12] = {};
        bool seenRoster[12] = {};
        int active=0;
        DWORD lineupSourceTick=0;
        DWORD eventRevision=0;
        DWORD eventTick=0;
        for (int i=0; i<12; ++i) {
            const int found=ShotClockLookupLive06BenchPlayerByDbId(gBenchAllDbIds[side][i], &players[i]);
            if (found!=1 || players[i].side!=side ||
                players[i].originalRosterSlot<0 || players[i].originalRosterSlot>=12 ||
                seenRoster[players[i].originalRosterSlot]) { complete=false; break; }
            seenRoster[players[i].originalRosterSlot]=true;
            if (i==0) {
                lineupSourceTick=players[i].refreshedAtTick;
                eventRevision=players[i].lineupEventRevision;
                eventTick=players[i].lineupEventTick;
            } else if (lineupSourceTick!=players[i].refreshedAtTick ||
                       eventRevision!=players[i].lineupEventRevision ||
                       eventTick!=players[i].lineupEventTick) {
                complete=false;
                break;
            }
            if (players[i].trackedOnCourt) ++active;
        }
        if (!complete || active!=5 || !lineupSourceTick) continue;
        // Persistent seats: a seat belongs to its POSITION, not to a roster
        // index. Existing off-court occupants keep their seats. When a bench
        // player enters the game, their seat becomes available for an outgoing
        // starter. This is essential for bench_1 -> starter -> bench_1 swaps.
        int next[12];
        for (int i=0; i<12; ++i) next[i]=-1;
        bool seatUsed[7] = {};
        int seatCount=0;
        if (gBenchLastAssignedCount[side]==7) {
            // Retain the exact physical position of every surviving occupant.
            for (int actor=0; actor<12; ++actor) {
                const int previous=gBenchAssignment[side][actor];
                if (!players[actor].trackedOnCourt && previous>=0 &&
                    previous<7 && !seatUsed[previous]) {
                    next[actor]=previous;
                    seatUsed[previous]=true;
                    ++seatCount;
                }
            }
        } else {
            // Initial game lineup: the seven native bench_1..bench_7 actors
            // establish the canonical seat identities once per game.
            for (int actor=5; actor<12; ++actor) {
                if (!players[actor].trackedOnCourt) {
                    const int initialSeat=actor-5;
                    next[actor]=initialSeat;
                    seatUsed[initialSeat]=true;
                    ++seatCount;
                }
            }
        }
        // Deterministic assignment for newcomers: take vacated seats only.
        // If several players substitute together, match incoming bench players
        // with outgoing starters without disturbing the seated survivors.
        for (int roster=0; roster<12; ++roster) {
            for (int actor=0; actor<12; ++actor) {
                if (players[actor].trackedOnCourt || next[actor]>=0 ||
                    players[actor].originalRosterSlot!=roster) continue;
                int freeSeat=0;
                while (freeSeat<7 && seatUsed[freeSeat]) ++freeSeat;
                if (freeSeat>=7) { complete=false; break; }
                next[actor]=freeSeat;
                seatUsed[freeSeat]=true;
                ++seatCount;
            }
            if (!complete) break;
        }
        if (!complete || seatCount!=7) continue;
        bool changed=!gBenchAssignmentValid[side];
        if (!changed) for (int i=0; i<12; ++i)
            if (gBenchAssignment[side][i]!=next[i]) { changed=true; break; }
        if (!changed) {
            // An event that produced no seat-map change must not authorize a
            // different, unrelated lineup update later in its 1.5s window.
            gBenchAcceptedLineupEventRevision[side]=eventRevision;
            gBenchPendingAssignmentValid[side]=false;
            continue;
        }
        // A valid substitution event already changed ShotClock's 24-player
        // cache synchronously. When the event is fresh, use that validated
        // *tracker transition* at once rather than waiting for two unrelated
        // presentation refreshes. This does not assert that the FE hook is the
        // earliest native gameplay-commit callback.
        const bool freshEvent = eventRevision && eventTick &&
            eventRevision != gBenchAcceptedLineupEventRevision[side] &&
            static_cast<DWORD>(now-eventTick) <= 1500u;
        if (gBenchAssignmentValid[side] && !freshEvent) {
            // Require two independent ShotClock refreshes with the same new
            // 5-on-court set before replacing established seats. A transient
            // replay/overlay snapshot must not reshuffle the live-game bench.
            bool samePending=gBenchPendingAssignmentValid[side];
            if (samePending) for (int i=0; i<12; ++i)
                if (gBenchPendingAssignment[side][i]!=next[i]) {
                    samePending=false; break;
                }
            if (!samePending) {
                for (int i=0; i<12; ++i)
                    gBenchPendingAssignment[side][i]=next[i];
                gBenchPendingSourceTick[side]=lineupSourceTick;
                gBenchPendingAssignmentValid[side]=true;
                continue;
            }
            // Re-reading the same cached snapshot is NOT confirmation.
            if (gBenchPendingSourceTick[side]==lineupSourceTick) continue;
        }
        // First valid mapping activates immediately. Later transitions only
        // commit after an independently refreshed matching lineup snapshot.
        for (int i=0; i<12; ++i) gBenchAssignment[side][i]=next[i];
        gBenchAssignmentValid[side]=true;
        gBenchPendingAssignmentValid[side]=false;
        gBenchLastAssignedCount[side]=7;
        gBenchAcceptedLineupEventRevision[side]=eventRevision;
        if (freshEvent)
            BenchLog("SEAT_EVENT_FAST side=%d revision=%lu eventAgeMs=%lu (FE event; native court commit not verified)",
                side,static_cast<unsigned long>(eventRevision),
                static_cast<unsigned long>(static_cast<DWORD>(now-eventTick)));
        for (int i=0; i<12; ++i)
            BenchLog("SEAT_ASSIGN side=%d actor=%d dbid=%d originalRoster=%d active=%d seat=%d",
                side,i,gBenchAllDbIds[side][i],players[i].originalRosterSlot,
                players[i].trackedOnCourt ? 1 : 0,next[i]+1);
    }
}

static bool TryApplyFixedSeatToActor(uintptr_t matrix, float* t)
{
    for (int side=0; side<2; ++side) for (int actor=0; actor<12; ++actor) {
        const uintptr_t record=gBenchAllRecords[side][actor];
        if (!record || matrix!=record+0x4F0+0x30) continue;
        const int seat=gBenchAssignmentValid[side] ? gBenchAssignment[side][actor] : -1;
        if (seat<0 || seat>=7) return true; // active player: never touch
        const float* destination=gBenchSeatWorld[side][seat];
        // Physical safety gate. We deliberately do not remove it: if the
        // lineup tracker becomes stale during an actual substitution, snapping
        // an on-court actor back to the bench would be much worse than a
        // temporary native/replay pose. Native positions inside the bench
        // region always resolve to the same fixed world-space seat.
        const float nativeY=t[1];
        const float originalX=gBenchAnchors[side][0].pos[0];
        const float farX=gBenchAnchors[side][6].pos[0];
        const float lo=(std::min)(originalX,farX)-8.f;
        const float hi=(std::max)(originalX,farX)+8.f;
        if (fabsf(nativeY-gBenchAnchors[side][0].pos[1])>1.5f ||
            t[0]<lo || t[0]>hi ||
            fabsf(t[2]-gBenchAnchors[side][0].pos[2])>1.5f)
            return true; // player not physically at bench yet
        t[0]=destination[0]; t[1]=destination[1]; t[2]=destination[2];
        if (gBenchLastSeatByActor[side][actor]!=seat) {
            gBenchLastSeatByActor[side][actor]=seat;
            BenchLog("FIXED_SEAT_APPLIED side=%d actor=%d dbid=%d seat=%d pos=(%.3f,%.3f,%.3f)",
                side,actor,gBenchAllDbIds[side][actor],seat+1,t[0],t[1],t[2]);
        }
        return true;
    }
    return false;
}

void __stdcall BenchApplyRootOffset(uintptr_t matrix)
    {
        if (!matrix || !gBenchHasPositionRequests) return;
        // Also accept starter actor matrices once the complete scene mapping
        // exists. Only move actors whose current assignment is a bench seat.
        RefreshFixedSeatAssignments();
        for (int side=0; side<2; ++side) {
            if (!gBenchAssignmentValid[side]) continue;
            for (int actor=0; actor<5; ++actor) {
                const uintptr_t record=gBenchAllRecords[side][actor];
                if (!record || matrix!=record+0x4F0+0x30) continue;
                __try {
                    float* translation=reinterpret_cast<float*>(matrix+0x30);
                    if (std::isfinite(translation[0]) && std::isfinite(translation[1]) &&
                        std::isfinite(translation[2]))
                        TryApplyFixedSeatToActor(matrix,translation);
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
                return;
            }
        }
        for (int side=0;side<2;++side) for (int slot=0;slot<7;++slot) {
            const uintptr_t record=gBenchActorRecords[side][slot];
            if (!record || matrix!=record+0x4F0+0x30) continue;
            const BenchSlotConfig& config=gBenchSlots[side][slot];
            __try {
                float* translation=reinterpret_cast<float*>(matrix+0x30);
                const float x=translation[0],y=translation[1],z=translation[2];
                if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) return;
                BenchAnchor& anchor=gBenchAnchors[side][slot];
                if (!anchor.seen) {
                    anchor.pos[0]=x; anchor.pos[1]=y; anchor.pos[2]=z;
                    anchor.seen=true;
                    BenchLog("FIRST_ANCHOR side=%d slot=%d native=(%.4f,%.4f,%.4f)",side,slot+1,x,y,z);
                    BenchLogIdMatches(side,slot);
                }
                BuildFixedSeatWorld(side);
                RefreshFixedSeatAssignments();
                if (gBenchAssignmentValid[side]) {
                    TryApplyFixedSeatToActor(matrix,translation);
                    return;
                }
                BuildBenchSpacing(side);
                const float dx=x-anchor.pos[0],dy=y-anchor.pos[1],dz=z-anchor.pos[2];
                const float d2=dx*dx+dy*dy+dz*dz;
                // Periodic, bounded bench probe: original native coordinates before offsets.
                const DWORD now=GetTickCount();
                if (gBenchLastProbeTick[side][slot]==0 ||
                    static_cast<DWORD>(now-gBenchLastProbeTick[side][slot])>=5000) {
                    gBenchLastProbeTick[side][slot]=now ? now : 1;
                    BenchProbeDatabaseIdentity(side,slot);
                    BenchLog("BENCH_PROBE side=%d slot=%d factory_PLAYERID=%d native=(%.3f,%.3f,%.3f) anchorDistance=%.3f",
                        side,slot+1,gBenchFactoryIds[side][slot],x,y,z,sqrtf(d2));
                }
                // Conservative spatial guard; not a verified active-lineup check.
                if (d2>0.49f) {
                    if (!anchor.rejected) {
                        BenchLog("POSITION_REJECT side=%d slot=%d nativeDistance=%.4f; no offset",
                            side,slot+1,sqrtf(d2));
                        anchor.rejected=true;
                    }
                    return;
                }
                // Slot-based displacement: preserve EACH scene slot's native
                // chair position. The first configured slot is the zero-offset
                // reference; remaining offsets are relative to its JSON values.
                // Factory PLAYERID is diagnostic only and NEVER selects a seat.
                const BenchSlotConfig& referenceConfig = gBenchSlots[side][0];
                float rel[3] = {0.f, 0.f, 0.f};
                if (slot != 0 && config.hasOffset) {
                    for (int axis=0; axis<3; ++axis)
                        rel[axis] = config.offset[axis] -
                            (referenceConfig.hasOffset ? referenceConfig.offset[axis] : 0.f);
                    translation[0] = x + rel[0];
                    translation[1] = y + rel[1];
                    translation[2] = z + rel[2];
                }
                // Do not add automatic spacing on top of explicit roster-position
                // coordinates; it would defeat the specified slot layout.
                if (!gBenchLoggedApplied[side][slot]) {
                    gBenchLoggedApplied[side][slot]=true;
                    BenchLog("ROSTERPOS_APPLIED side=%d slot=%d factory_PLAYERID=%d native=(%.3f,%.3f,%.3f) displacement=(%.3f,%.3f,%.3f) output=(%.3f,%.3f,%.3f)",
                        side,slot+1,gBenchFactoryIds[side][slot],x,y,z,
                        rel[0],rel[1],rel[2],translation[0],translation[1],translation[2]);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                OutputDebugStringA("[CustomPaths] Pre-propagation bench root offset failed.\n");
            }
            return;
        }
    }

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
        // Capture all twelve team scene actors. Starter paths use calls to
        // this factory outside the 14 bench-only sites. Scan the bounded
        // GameCourt scene construction region for direct calls to this exact
        // factory; the wrapper always calls the native factory unchanged.
        // An unmatched EXE leaves the scene incomplete and dynamic seating off.
        unsigned int discovered=0;
        for (uintptr_t call=0x0061D000; call+5<=0x0061F100; ++call) {
            if (!BenchValidateRelativeCall(call,0x00605A20)) continue;
            bool known=false;
            for (uintptr_t original:factoryCalls) if (call==original) known=true;
            if (!known) {
                patch::RedirectCall(static_cast<unsigned int>(call),BenchFactoryCapture);
                ++discovered;
            }
            call+=4;
        }
        for (uintptr_t call:factoryCalls) patch::RedirectCall(call,BenchFactoryCapture);
        BenchLog("ACTOR12_FACTORY_SCAN additionalCalls=%u exactFactory=00605A20",discovered);
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
char gOverridePaths[SLOT_COUNT][kOverridePathCapacity] = {};

OverrideCacheEntry gOverrideCache[kOverrideCacheCapacity] = {};

unsigned int gOverrideCacheCount = 0;

unsigned int gOverrideCacheCursor = 0;

bool gGameContextValid = false;

char gCurrentStadiumId4[5] = {};

PostseasonStage gCurrentStage = STAGE_REGULAR;

GameVersion gGameVersion = GAME_UNKNOWN;

#if defined(_M_IX86)
uintptr_t gOpenResolvedContinuation = 0;

bool gOpenResolvedGateInstalled = false;

#endif
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

void BeginNewGameStadiumLoad()
    {
        // GameCourt::Setup can be called AFTER the scene factory has already
        // constructed the 14 bench records. Do not discard them here.
        // The factory capture resets the corresponding anchor and probe state
        // whenever a record is (re)constructed. Configuration and caches still
        // expire on every stadium handoff.
        int retained = 0;
        for (int side = 0; side < 2; ++side)
            for (int slot = 0; slot < 7; ++slot)
                if (gBenchActorRecords[side][slot]) ++retained;
        BenchLog("GAME_HANDOFF retainedBenchActors=%d; preserving factory records", retained);
        DisableAllSlots();
        OutputDebugStringA(
            "[CustomPaths] New game load: expired stadium configuration/cache; bench actor records retained.\n");
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
        const PostseasonStage stage = PrepareGameContext(suppliedOptions);
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

#endif
#if defined(_M_IX86)
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

        InitializeGameContextHooks();
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
    custompaths::BenchLog("BENCH_TRACE_START StageSetup v6.1 persistent-seat-replacement");
    custompaths::ResetBenchIdentity();
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

// Stadium JSON, Dorna and lighting configuration.


using namespace plugin;

void SetArenaLightingStadiumLighting(
    const char* stadiumId,
    bool enabled,
    float brightness,
    float contrast,
    float saturation,
    float gamma);

namespace stadiumconfigdetail
{
    bool gDebugLoggingEnabled = false;

    void InitializeDebugLogging()
    {
        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
            return;

        *(slash + 1) = '\0';

        char iniPath[MAX_PATH] = {};
        std::snprintf(iniPath, sizeof(iniPath), "%smain.ini", exePath);

        gDebugLoggingEnabled =
            GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, iniPath) != 0;

        if (!gDebugLoggingEnabled)
            return;

        char logsPath[MAX_PATH] = {};
        std::snprintf(logsPath, sizeof(logsPath), "%slogs", exePath);
        CreateDirectoryA(logsPath, nullptr);
    }

    bool BuildDebugLogPath(
        char* outPath,
        size_t outSize,
        const char* fileName)
    {
        if (!outPath || !outSize || !fileName || !gDebugLoggingEnabled)
        {
            if (outPath && outSize)
                outPath[0] = '\0';
            return false;
        }

        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            outPath[0] = '\0';
            return false;
        }

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
        {
            outPath[0] = '\0';
            return false;
        }

        *(slash + 1) = '\0';

        const int written =
            std::snprintf(outPath, outSize, "%slogs\\%s", exePath, fileName);

        if (written < 0 || static_cast<size_t>(written) >= outSize)
        {
            outPath[0] = '\0';
            return false;
        }

        return true;
    }

    // ============================================================
    // Shared stadium JSON
    //
    // assets/stadia/<stadium-abbr>.json
    //
    // {
    //   "dorna": {
    //     "ad_count": 12,
    //     "period": 8.6,
    //     "transition_period": 0.6
    //   },
    //   "camera": "assets/camera/boston_low.mgd",
    //   "stadiumLighting": {
    //     "brightness": 0.8,
    //     "contrast": 1.2,
    //     "saturation": 1.15,
    //     "gamma": 1.05
    //   }
    // }
    //
    // Missing file / section / key => EA defaults.
    // ============================================================

    constexpr float kDefaultPeriodSeconds = 8.6f;
    constexpr float kDefaultTransitionSeconds = 0.6f;
    constexpr int   kDefaultAdCount = 4;

    enum GameId
    {
        GAME_NONE = 0,
        GAME_2005 = 2005,
        GAME_2006 = 2006,
        GAME_2007 = 2007,
        GAME_2008 = 2008
    };

    struct GameDornaInfo
    {
        int game = GAME_NONE;

        uintptr_t updateFunction = 0;
        size_t updateScanSize = 0;

        uintptr_t logicalAdCountPatch = 0;

        uintptr_t periodTicksAddress = 0;
        uintptr_t transitionTicksAddress = 0;

        bool usesFloatTiming = false;
        float originalPeriodSeconds = kDefaultPeriodSeconds;
        float originalTransitionSeconds = kDefaultTransitionSeconds;
    };

    GameDornaInfo gGame{};

    // Private runtime values read by patched GameStadium::Update.
    float gDornaScrollStep = 0.25f;
    int   gLegacyPeriodTicks = 0;
    int   gLegacyTransitionTicks = 0;

    float gDornaPeriodSecondsRuntime = kDefaultPeriodSeconds;
    float gDornaTransitionSecondsRuntime = kDefaultTransitionSeconds;

    // EA baseline values captured before stadium overrides.
    int gOriginalPeriodTicks = 0;
    int gOriginalTransitionTicks = 0;

    // Current stadium runtime state.
    std::string gCurrentStadiumId;
    int   gCurrentAdCount = kDefaultAdCount;
    float gCurrentPeriodSeconds = kDefaultPeriodSeconds;
    float gCurrentTransitionSeconds = kDefaultTransitionSeconds;
    std::string gCurrentCameraPath;
    constexpr float kDefaultStadiumBrightness = 1.0f;
    constexpr float kDefaultStadiumContrast = 1.0f;
    constexpr float kDefaultStadiumSaturation = 1.0f;
    constexpr float kDefaultStadiumGamma = 1.0f;
    bool  gCurrentStadiumLightingEnabled = false;
    float gCurrentStadiumBrightness = kDefaultStadiumBrightness;
    float gCurrentStadiumContrast = kDefaultStadiumContrast;
    float gCurrentStadiumSaturation = kDefaultStadiumSaturation;
    float gCurrentStadiumGamma = kDefaultStadiumGamma;

    struct StadiumConfigData
    {
        bool hasAdCount = false;
        int adCount = kDefaultAdCount;

        bool hasPeriod = false;
        float period = kDefaultPeriodSeconds;

        bool hasTransitionPeriod = false;
        float transitionPeriod = kDefaultTransitionSeconds;

        bool hasCamera = false;
        std::string camera;

        bool hasStadiumColourScale = false;
        float stadiumColourScale =
            kDefaultStadiumBrightness;

        bool hasStadiumLighting = false;
        float stadiumBrightness = kDefaultStadiumBrightness;
        float stadiumContrast = kDefaultStadiumContrast;
        float stadiumSaturation = kDefaultStadiumSaturation;
        float stadiumGamma = kDefaultStadiumGamma;
    };

    // ------------------------------------------------------------
    // Logger
    // ------------------------------------------------------------

    std::string GetGameDirectory()
    {
        char path[MAX_PATH]{};

        DWORD len = GetModuleFileNameA(
            nullptr,
            path,
            static_cast<DWORD>(sizeof(path))
        );

        if (len == 0 || len >= sizeof(path))
            return ".";

        std::string result(path, len);
        const size_t slash = result.find_last_of("\\/");

        if (slash == std::string::npos)
            return ".";

        return result.substr(0, slash);
    }

    void Log(const char* format, ...)
    {
        if (!gDebugLoggingEnabled)
            return;

        char message[1024]{};

        va_list args;
        va_start(args, format);

        vsnprintf_s(
            message,
            sizeof(message),
            _TRUNCATE,
            format,
            args
        );

        va_end(args);

        char logPath[MAX_PATH] = {};
        if (!BuildDebugLogPath(
                logPath,
                sizeof(logPath),
                "StadiumConfig.log"))
        {
            return;
        }

        FILE* file = nullptr;

        if (fopen_s(&file, logPath, "a") == 0 && file)
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);

            std::fprintf(
                file,
                "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour,
                st.wMinute,
                st.wSecond,
                st.wMilliseconds,
                message
            );

            std::fclose(file);
        }
    }

    // ------------------------------------------------------------
    // Memory helpers
    // ------------------------------------------------------------

    template <typename T>
    bool WriteValue(uintptr_t address, const T& value)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(
                reinterpret_cast<void*>(address),
                sizeof(T),
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(address),
            &value,
            sizeof(T)
        );

        DWORD ignored = 0;

        VirtualProtect(
            reinterpret_cast<void*>(address),
            sizeof(T),
            oldProtect,
            &ignored
        );

        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            sizeof(T)
        );

        return true;
    }

    bool PatchBytes(
        uintptr_t address,
        const uint8_t* bytes,
        size_t size)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(
                reinterpret_cast<void*>(address),
                size,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(address),
            bytes,
            size
        );

        DWORD ignored = 0;

        VirtualProtect(
            reinterpret_cast<void*>(address),
            size,
            oldProtect,
            &ignored
        );

        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            size
        );

        return true;
    }

    bool IsReadableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION mbi{};

        if (!VirtualQuery(address, &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & PAGE_GUARD)
            return false;

        return (mbi.Protect & 0xFF) != PAGE_NOACCESS;
    }

    bool FloatMatches(
        uintptr_t address,
        float expected,
        float epsilon = 0.0001f)
    {
        if (!IsReadableAddress(
                reinterpret_cast<const void*>(address)))
        {
            return false;
        }

        float value = 0.0f;

        std::memcpy(
            &value,
            reinterpret_cast<const void*>(address),
            sizeof(value)
        );

        return std::fabs(value - expected) <= epsilon;
    }

    bool ReplaceAbsoluteOperand(
        uintptr_t operandAddress,
        const void* replacement)
    {
        const uint32_t replacementAddress =
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(replacement)
            );

        return WriteValue<uint32_t>(
            operandAddress,
            replacementAddress
        );
    }

    int RedirectFloatReferences(
        uintptr_t functionAddress,
        size_t scanSize,
        float originalValue,
        float* replacementValue)
    {
        auto* code =
            reinterpret_cast<uint8_t*>(functionAddress);

        int patched = 0;

        for (size_t i = 0; i + 6 <= scanSize; ++i)
        {
            const bool isFmulAbsolute =
                code[i] == 0xD8 &&
                code[i + 1] == 0x0D;

            const bool isFldAbsolute =
                code[i] == 0xD9 &&
                code[i + 1] == 0x05;

            if (!isFmulAbsolute && !isFldAbsolute)
                continue;

            uint32_t referencedAddress = 0;

            std::memcpy(
                &referencedAddress,
                code + i + 2,
                sizeof(referencedAddress)
            );

            if (!FloatMatches(
                    referencedAddress,
                    originalValue))
            {
                continue;
            }

            if (ReplaceAbsoluteOperand(
                    functionAddress + i + 2,
                    replacementValue))
            {
                ++patched;
                i += 5;
            }
        }

        return patched;
    }

    int RedirectAbsoluteAddressReferences(
        uintptr_t functionAddress,
        size_t scanSize,
        uintptr_t originalAddress,
        const void* replacementAddress)
    {
        auto* code =
            reinterpret_cast<uint8_t*>(functionAddress);

        const uint32_t original =
            static_cast<uint32_t>(originalAddress);

        const uint32_t replacement =
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(replacementAddress)
            );

        int patched = 0;

        for (size_t i = 0;
             i + sizeof(uint32_t) <= scanSize;
             ++i)
        {
            uint32_t candidate = 0;

            std::memcpy(
                &candidate,
                code + i,
                sizeof(candidate)
            );

            if (candidate != original)
                continue;

            if (WriteValue<uint32_t>(
                    functionAddress + i,
                    replacement))
            {
                ++patched;
                i += sizeof(uint32_t) - 1;
            }
        }

        return patched;
    }

    uintptr_t ReadRelativeCallTarget(
        uintptr_t callAddress)
    {
        if (*reinterpret_cast<const uint8_t*>(
                callAddress) != 0xE8)
        {
            return 0;
        }

        int32_t relative = 0;

        std::memcpy(
            &relative,
            reinterpret_cast<const void*>(
                callAddress + 1),
            sizeof(relative)
        );

        return callAddress + 5 + relative;
    }

    // ------------------------------------------------------------
    // JSON/path helpers
    // ------------------------------------------------------------

    std::string NormalizeStadiumId(
        const char* stadiumId)
    {
        if (!stadiumId)
            return {};

        std::string id(stadiumId);

        while (!id.empty() &&
               std::isspace(
                   static_cast<unsigned char>(
                       id.back())))
        {
            id.pop_back();
        }

        size_t first = 0;

        while (first < id.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       id[first])))
        {
            ++first;
        }

        if (first)
            id.erase(0, first);

        std::transform(
            id.begin(),
            id.end(),
            id.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(
                    std::tolower(c));
            }
        );

        id.erase(
            std::remove_if(
                id.begin(),
                id.end(),
                [](char c)
                {
                    const unsigned char uc =
                        static_cast<unsigned char>(c);

                    return !(
                        std::isalnum(uc) ||
                        c == '_' ||
                        c == '-'
                    );
                }
            ),
            id.end()
        );

        return id;
    }

    std::string BuildStadiumJsonPath(
        const std::string& stadiumId)
    {
        return
            GetGameDirectory() +
            "\\assets\\stadia\\" +
            stadiumId +
            ".json";
    }

    bool FindObjectBody(
        const std::string& json,
        const char* key,
        std::string& outBody)
    {
        const std::string quotedKey =
            std::string("\"") + key + "\"";

        const size_t keyPos =
            json.find(quotedKey);

        if (keyPos == std::string::npos)
            return false;

        const size_t colonPos =
            json.find(
                ':',
                keyPos + quotedKey.size()
            );

        if (colonPos == std::string::npos)
            return false;

        const size_t openPos =
            json.find(
                '{',
                colonPos + 1
            );

        if (openPos == std::string::npos)
            return false;

        int depth = 0;

        for (size_t i = openPos;
             i < json.size();
             ++i)
        {
            if (json[i] == '{')
            {
                ++depth;
            }
            else if (json[i] == '}')
            {
                --depth;

                if (depth == 0)
                {
                    outBody =
                        json.substr(
                            openPos + 1,
                            i - openPos - 1
                        );

                    return true;
                }
            }
        }

        return false;
    }

    bool FindNumber(
        const std::string& objectBody,
        const char* key,
        double& outValue)
    {
        const std::string quotedKey =
            std::string("\"") + key + "\"";

        const size_t keyPos =
            objectBody.find(quotedKey);

        if (keyPos == std::string::npos)
            return false;

        const size_t colonPos =
            objectBody.find(
                ':',
                keyPos + quotedKey.size()
            );

        if (colonPos == std::string::npos)
            return false;

        const char* start =
            objectBody.c_str() +
            colonPos +
            1;

        while (*start &&
               std::isspace(
                   static_cast<unsigned char>(
                       *start)))
        {
            ++start;
        }

        char* end = nullptr;

        const double value =
            std::strtod(start, &end);

        if (end == start ||
            !std::isfinite(value))
        {
            return false;
        }

        outValue = value;
        return true;
    }

    bool FindString(
        const std::string& json,
        const char* key,
        std::string& outValue)
    {
        const std::string quotedKey =
            std::string("\"") + key + "\"";

        const size_t keyPos =
            json.find(quotedKey);

        if (keyPos == std::string::npos)
            return false;

        const size_t colonPos =
            json.find(
                ':',
                keyPos + quotedKey.size()
            );

        if (colonPos == std::string::npos)
            return false;

        size_t position = colonPos + 1;

        while (position < json.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       json[position])))
        {
            ++position;
        }

        if (position >= json.size() ||
            json[position] != '"')
        {
            return false;
        }

        ++position;
        std::string value;

        while (position < json.size())
        {
            const char c = json[position++];

            if (c == '"')
            {
                outValue = value;
                return true;
            }

            if (static_cast<unsigned char>(c) < 0x20)
                return false;

            if (c != '\\')
            {
                value.push_back(c);
                continue;
            }

            if (position >= json.size())
                return false;

            const char escaped = json[position++];

            switch (escaped)
            {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default:
                return false;
            }
        }

        return false;
    }

    bool NormalizeCameraPath(
        const std::string& configuredPath,
        std::string& outPath)
    {
        std::string path = configuredPath;

        while (!path.empty() &&
               std::isspace(
                   static_cast<unsigned char>(
                       path.back())))
        {
            path.pop_back();
        }

        size_t first = 0;

        while (first < path.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       path[first])))
        {
            ++first;
        }

        if (first)
            path.erase(0, first);

        std::replace(
            path.begin(),
            path.end(),
            '/',
            '\\'
        );

        if (path.size() <= 7 ||
            path.find(':') != std::string::npos ||
            path[0] == '\\')
        {
            return false;
        }

        for (char c : path)
        {
            const unsigned char uc =
                static_cast<unsigned char>(c);

            if (uc < 0x20 ||
                c == '"' ||
                c == '<' ||
                c == '>' ||
                c == '|' ||
                c == '*' ||
                c == '?')
            {
                return false;
            }
        }

        const char assetsPrefix[] = "assets\\";

        for (size_t i = 0;
             i < sizeof(assetsPrefix) - 1;
             ++i)
        {
            if (std::tolower(
                    static_cast<unsigned char>(path[i])) !=
                assetsPrefix[i])
            {
                return false;
            }
        }

        size_t componentStart = 0;

        while (componentStart <= path.size())
        {
            const size_t separator =
                path.find('\\', componentStart);

            const size_t componentLength =
                (separator == std::string::npos)
                    ? path.size() - componentStart
                    : separator - componentStart;

            if (componentLength == 2 &&
                path[componentStart] == '.' &&
                path[componentStart + 1] == '.')
            {
                return false;
            }

            if (separator == std::string::npos)
                break;

            componentStart = separator + 1;
        }

        if (path.size() < 4)
            return false;

        std::string extension =
            path.substr(path.size() - 4);

        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(
                    std::tolower(c));
            }
        );

        if (extension != ".mgd")
            return false;

        outPath = path;
        return true;
    }

    bool LoadStadiumConfig(
        const std::string& stadiumId,
        StadiumConfigData& outConfig)
    {
        outConfig = {};

        const std::string path =
            BuildStadiumJsonPath(stadiumId);

        Log(
            "Looking for stadium config: %s",
            path.c_str()
        );

        std::ifstream file(
            path,
            std::ios::in |
            std::ios::binary
        );

        if (!file)
        {
            Log(
                "No stadium config found for '%s'. Using EA defaults.",
                stadiumId.c_str()
            );

            return false;
        }

        Log(
            "Found stadium config for '%s'.",
            stadiumId.c_str()
        );

        std::ostringstream stream;
        stream << file.rdbuf();

        const std::string json = stream.str();
        std::string configuredCamera;

        if (FindString(
                json,
                "camera",
                configuredCamera))
        {
            std::string normalizedCamera;

            if (NormalizeCameraPath(
                    configuredCamera,
                    normalizedCamera))
            {
                outConfig.hasCamera = true;
                outConfig.camera = normalizedCamera;

                Log(
                    "  camera = %s",
                    outConfig.camera.c_str()
                );
            }
            else
            {
                Log(
                    "  camera path rejected: %s",
                    configuredCamera.c_str()
                );
            }
        }

        double value = 0.0;

        if (FindNumber(
                json,
                "stadiumColourScale",
                value))
        {
            if (value >= 0.0 && value <= 2.0)
            {
                outConfig.hasStadiumColourScale = true;
                outConfig.stadiumColourScale =
                    static_cast<float>(value);

                Log(
                    "  stadiumColourScale = %.4f",
                    outConfig.stadiumColourScale
                );
            }
            else
            {
                Log(
                    "  stadiumColourScale rejected: %.4f (valid range 0.0-2.0)",
                    value
                );
            }
        }

        std::string lightingBody;

        if (FindObjectBody(
                json,
                "stadiumLighting",
                lightingBody))
        {
            outConfig.hasStadiumLighting = true;

            if (FindNumber(
                    lightingBody,
                    "brightness",
                    value))
            {
                if (value >= 0.0 && value <= 2.0)
                {
                    outConfig.stadiumBrightness =
                        static_cast<float>(value);
                }
                else
                {
                    Log(
                        "  stadiumLighting.brightness rejected: %.4f (valid range 0.0-2.0)",
                        value
                    );
                }
            }

            if (FindNumber(
                    lightingBody,
                    "contrast",
                    value))
            {
                if (value >= 0.0 && value <= 2.0)
                {
                    outConfig.stadiumContrast =
                        static_cast<float>(value);
                }
                else
                {
                    Log(
                        "  stadiumLighting.contrast rejected: %.4f (valid range 0.0-2.0)",
                        value
                    );
                }
            }

            if (FindNumber(
                    lightingBody,
                    "saturation",
                    value))
            {
                if (value >= 0.0 && value <= 2.0)
                {
                    outConfig.stadiumSaturation =
                        static_cast<float>(value);
                }
                else
                {
                    Log(
                        "  stadiumLighting.saturation rejected: %.4f (valid range 0.0-2.0)",
                        value
                    );
                }
            }

            if (FindNumber(
                    lightingBody,
                    "gamma",
                    value))
            {
                if (value >= 0.1 && value <= 3.0)
                {
                    outConfig.stadiumGamma =
                        static_cast<float>(value);
                }
                else
                {
                    Log(
                        "  stadiumLighting.gamma rejected: %.4f (valid range 0.1-3.0)",
                        value
                    );
                }
            }

            Log(
                "  stadiumLighting = { brightness: %.4f, contrast: %.4f, saturation: %.4f, gamma: %.4f }",
                outConfig.stadiumBrightness,
                outConfig.stadiumContrast,
                outConfig.stadiumSaturation,
                outConfig.stadiumGamma
            );
        }

        std::string dornaBody;

        if (!FindObjectBody(
                json,
                "dorna",
                dornaBody))
        {
            Log(
                "Config '%s' has no dorna object. Dorna uses EA defaults.",
                stadiumId.c_str()
            );

            return true;
        }

        if (FindNumber(
                dornaBody,
                "ad_count",
                value))
        {
            const int count =
                static_cast<int>(
                    std::lround(value)
                );

            if (count >= 1 &&
                count <= 127)
            {
                outConfig.hasAdCount = true;
                outConfig.adCount = count;

                Log(
                    "  ad_count = %d",
                    count
                );
            }
        }

        if (FindNumber(
                dornaBody,
                "period",
                value) &&
            value > 0.0)
        {
            outConfig.hasPeriod = true;
            outConfig.period =
                static_cast<float>(value);

            Log(
                "  period = %.4f",
                outConfig.period
            );
        }

        if (FindNumber(
                dornaBody,
                "transition_period",
                value) &&
            value >= 0.0)
        {
            outConfig.hasTransitionPeriod = true;
            outConfig.transitionPeriod =
                static_cast<float>(value);

            Log(
                "  transition_period = %.4f",
                outConfig.transitionPeriod
            );
        }

        return true;
    }

    // ------------------------------------------------------------
    // Shared Dorna runtime
    // ------------------------------------------------------------

    void CaptureOriginalTiming()
    {
        if (gGame.usesFloatTiming)
        {
            Log(
                "Original NBA Live %d timing: period=%.4fs transition=%.4fs",
                gGame.game,
                gGame.originalPeriodSeconds,
                gGame.originalTransitionSeconds
            );
            return;
        }

        if (gOriginalPeriodTicks <= 0)
        {
            gOriginalPeriodTicks =
                *reinterpret_cast<const int*>(
                    gGame.periodTicksAddress
                );
        }

        if (gOriginalTransitionTicks <= 0)
        {
            gOriginalTransitionTicks =
                *reinterpret_cast<const int*>(
                    gGame.transitionTicksAddress
                );
        }

        if (gOriginalPeriodTicks <= 0 ||
            gOriginalPeriodTicks > 100000)
        {
            gOriginalPeriodTicks = 516;
        }

        if (gOriginalTransitionTicks <= 0 ||
            gOriginalTransitionTicks >
                gOriginalPeriodTicks)
        {
            gOriginalTransitionTicks = 37;
        }

        Log(
            "Original NBA Live %d timing: periodTicks=%d transitionTicks=%d",
            gGame.game,
            gOriginalPeriodTicks,
            gOriginalTransitionTicks
        );
    }

    bool ApplyLogicalAdCount(
        int adCount)
    {
        if (adCount < 1 ||
            adCount > 127)
        {
            return false;
        }

        uint8_t patchBytes[7] = {
            0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90
        };

        patchBytes[0] = 0x6B;

        if (gGame.game == GAME_2007 ||
            gGame.game == GAME_2008)
        {
            // 07/08:
            //   lea ecx, ds:0[esi*4]
            // =>
            //   imul ecx, esi, AD_COUNT
            patchBytes[1] = 0xCE;
        }
        else
        {
            // 05/06:
            //   lea esi, ds:0[ecx*4]
            // =>
            //   imul esi, ecx, AD_COUNT
            patchBytes[1] = 0xF1;
        }

        patchBytes[2] =
            static_cast<uint8_t>(adCount);

        return PatchBytes(
            gGame.logicalAdCountPatch,
            patchBytes,
            sizeof(patchBytes)
        );
    }

    void ApplyCurrentDornaTiming()
    {
        CaptureOriginalTiming();

        gDornaScrollStep =
            1.0f /
            static_cast<float>(
                gCurrentAdCount
            );

        ApplyLogicalAdCount(
            gCurrentAdCount
        );

        if (gGame.usesFloatTiming)
        {
            gDornaPeriodSecondsRuntime =
                gCurrentPeriodSeconds;

            gDornaTransitionSecondsRuntime =
                gCurrentTransitionSeconds;

            Log(
                "Applied Dorna runtime: game=%d stadium='%s' adCount=%d step=%.8f period=%.4fs transition=%.4fs",
                gGame.game,
                gCurrentStadiumId.c_str(),
                gCurrentAdCount,
                gDornaScrollStep,
                gDornaPeriodSecondsRuntime,
                gDornaTransitionSecondsRuntime
            );

            return;
        }

        gLegacyPeriodTicks =
            static_cast<int>(
                std::lround(
                    static_cast<double>(
                        gOriginalPeriodTicks
                    ) *
                    (
                        static_cast<double>(
                            gCurrentPeriodSeconds
                        ) /
                        static_cast<double>(
                            kDefaultPeriodSeconds
                        )
                    )
                )
            );

        if (gLegacyPeriodTicks < 1)
            gLegacyPeriodTicks = 1;

        if (std::fabs(
                gCurrentTransitionSeconds -
                kDefaultTransitionSeconds) <
            0.0001f)
        {
            gLegacyTransitionTicks =
                gOriginalTransitionTicks;
        }
        else if (
            gCurrentTransitionSeconds <= 0.0f)
        {
            gLegacyTransitionTicks = 0;
        }
        else
        {
            const double originalTransitionSeconds =
                static_cast<double>(
                    gOriginalTransitionTicks
                ) *
                (
                    static_cast<double>(
                        kDefaultPeriodSeconds
                    ) /
                    static_cast<double>(
                        gOriginalPeriodTicks
                    )
                );

            gLegacyTransitionTicks =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(
                            gOriginalTransitionTicks
                        ) *
                        (
                            static_cast<double>(
                                gCurrentTransitionSeconds
                            ) /
                            originalTransitionSeconds
                        )
                    )
                );
        }

        if (gLegacyTransitionTicks < 0)
            gLegacyTransitionTicks = 0;

        if (gLegacyTransitionTicks >
            gLegacyPeriodTicks)
        {
            gLegacyTransitionTicks =
                gLegacyPeriodTicks;
        }

        Log(
            "Applied Dorna runtime: game=%d stadium='%s' adCount=%d step=%.8f period=%.4fs (%d ticks) transition=%.4fs (%d ticks)",
            gGame.game,
            gCurrentStadiumId.c_str(),
            gCurrentAdCount,
            gDornaScrollStep,
            gCurrentPeriodSeconds,
            gLegacyPeriodTicks,
            gCurrentTransitionSeconds,
            gLegacyTransitionTicks
        );
    }

    void RestoreDefaultDornaConfig()
    {
        gCurrentAdCount =
            kDefaultAdCount;

        gCurrentPeriodSeconds =
            kDefaultPeriodSeconds;

        gCurrentTransitionSeconds =
            kDefaultTransitionSeconds;

        ApplyCurrentDornaTiming();
    }

    void LoadAndApplyStadiumConfig(
        const char* stadiumShortName)
    {
        gCurrentStadiumId =
            NormalizeStadiumId(
                stadiumShortName
            );

        gCurrentCameraPath.clear();
        gCurrentStadiumLightingEnabled = false;
        gCurrentStadiumBrightness = kDefaultStadiumBrightness;
        gCurrentStadiumContrast = kDefaultStadiumContrast;
        gCurrentStadiumSaturation = kDefaultStadiumSaturation;
        gCurrentStadiumGamma = kDefaultStadiumGamma;

        if (gGame.game == GAME_2005)
        {
            SetArenaLightingStadiumLighting(
                gCurrentStadiumId.c_str(),
                false,
                gCurrentStadiumBrightness,
                gCurrentStadiumContrast,
                gCurrentStadiumSaturation,
                gCurrentStadiumGamma
            );
        }

        // Always reset first so a missing JSON cannot inherit
        // settings from the previous arena.
        RestoreDefaultDornaConfig();

        if (gCurrentStadiumId.empty())
            return;

        StadiumConfigData config{};

        if (!LoadStadiumConfig(
                gCurrentStadiumId,
                config))
        {
            return;
        }

        if (config.hasAdCount)
        {
            gCurrentAdCount =
                config.adCount;
        }

        if (config.hasPeriod)
        {
            gCurrentPeriodSeconds =
                config.period;
        }

        if (config.hasTransitionPeriod)
        {
            gCurrentTransitionSeconds =
                config.transitionPeriod;
        }

        if (config.hasCamera)
        {
            gCurrentCameraPath =
                config.camera;
        }

        if (config.hasStadiumLighting)
        {
            gCurrentStadiumLightingEnabled = true;
            gCurrentStadiumBrightness = config.stadiumBrightness;
            gCurrentStadiumContrast = config.stadiumContrast;
            gCurrentStadiumSaturation = config.stadiumSaturation;
            gCurrentStadiumGamma = config.stadiumGamma;
        }
        else if (config.hasStadiumColourScale)
        {
            // Backward-compatible alias for the earlier single-control test.
            gCurrentStadiumLightingEnabled = true;
            gCurrentStadiumBrightness = config.stadiumColourScale;
            gCurrentStadiumContrast = 1.0f;
            gCurrentStadiumSaturation = 1.0f;
            gCurrentStadiumGamma = 1.0f;
        }

        if (gGame.game == GAME_2005)
        {
            SetArenaLightingStadiumLighting(
                gCurrentStadiumId.c_str(),
                gCurrentStadiumLightingEnabled,
                gCurrentStadiumBrightness,
                gCurrentStadiumContrast,
                gCurrentStadiumSaturation,
                gCurrentStadiumGamma
            );
        }

        if (gCurrentTransitionSeconds >
            gCurrentPeriodSeconds)
        {
            gCurrentTransitionSeconds =
                gCurrentPeriodSeconds;
        }

        ApplyCurrentDornaTiming();
    }

    void InstallSharedUpdatePatches()
    {
        CaptureOriginalTiming();

        const int scrollRefs =
            RedirectFloatReferences(
                gGame.updateFunction,
                gGame.updateScanSize,
                0.25f,
                &gDornaScrollStep
            );

        int periodRefs = 0;
        int transitionRefs = 0;

        if (gGame.usesFloatTiming)
        {
            periodRefs =
                RedirectFloatReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.originalPeriodSeconds,
                    &gDornaPeriodSecondsRuntime
                );

            transitionRefs =
                RedirectFloatReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.originalTransitionSeconds,
                    &gDornaTransitionSecondsRuntime
                );
        }
        else
        {
            periodRefs =
                RedirectAbsoluteAddressReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.periodTicksAddress,
                    &gLegacyPeriodTicks
                );

            transitionRefs =
                RedirectAbsoluteAddressReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.transitionTicksAddress,
                    &gLegacyTransitionTicks
                );
        }

        Log(
            "Update redirects installed: game=%d scrollRefs=%d periodRefs=%d transitionRefs=%d",
            gGame.game,
            scrollRefs,
            periodRefs,
            transitionRefs
        );

        RestoreDefaultDornaConfig();
    }

    // ============================================================
    // NBA Live 05
    // ============================================================

    constexpr uintptr_t kSetupDornaBoardsCallSite05 =
        0x005FD785;

    using SetupDornaBoards05_t =
        void (__thiscall*)(
            void* stadium,
            const char* strUBIPath,
            const char* stadiumShortName,
            void* options
        );

    SetupDornaBoards05_t
        gOriginalSetupDornaBoards05 = nullptr;

    void __fastcall SetupDornaBoards05_Hook(
        void* stadium,
        void*,
        const char* strUBIPath,
        const char* stadiumShortName,
        void* options)
    {
        Log(
            "NBA Live 05 GameStadium::Setup detected stadium: arg='%s' this+0x58='%s'",
            stadiumShortName
                ? stadiumShortName
                : "<null>",
            stadium
                ? reinterpret_cast<const char*>(
                    reinterpret_cast<uintptr_t>(
                        stadium) + 0x58)
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumShortName
        );

        gOriginalSetupDornaBoards05(
            stadium,
            strUBIPath,
            stadiumShortName,
            options
        );
    }

    void InstallStadiumConfig05()
    {
        gGame = {
            GAME_2005,
            0x005FD7B0, // GameStadium::Update
            0x180,
            0x005FD7EF, // lea esi,[ecx*4]
            0x00C4AAA8, // period ticks
            0x00C4A998, // transition ticks
            false,
            kDefaultPeriodSeconds,
            kDefaultTransitionSeconds
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        Log(
            "Initializing NBA Live 05 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        const uintptr_t target =
            ReadRelativeCallTarget(
                kSetupDornaBoardsCallSite05
            );

        if (!target)
        {
            Log(
                "ERROR: Could not resolve NBA Live 05 SetupDornaBoards call target at 0x%08X.",
                static_cast<unsigned int>(
                    kSetupDornaBoardsCallSite05)
            );

            return;
        }

        gOriginalSetupDornaBoards05 =
            reinterpret_cast<
                SetupDornaBoards05_t>(
                    target);

        patch::RedirectCall(
            kSetupDornaBoardsCallSite05,
            SetupDornaBoards05_Hook
        );

        Log(
            "NBA Live 05 SetupDornaBoards hook installed: callSite=0x%08X target=0x%08X",
            static_cast<unsigned int>(
                kSetupDornaBoardsCallSite05),
            static_cast<unsigned int>(
                target)
        );
    }

    // ============================================================
    // NBA Live 06
    // ============================================================

    constexpr uintptr_t
        kLoadArenaLightingSettings06 =
            0x005FCEB0;

    constexpr uintptr_t
        kSetupLightingCallSite06 =
            0x0060AF0F;

    using LoadArenaLightingSettings06_t =
        void (__thiscall*)(
            void* stadium,
            const char* stadiumShortName
        );

    LoadArenaLightingSettings06_t
        gOriginalLoadArenaLightingSettings06 =
            reinterpret_cast<
                LoadArenaLightingSettings06_t>(
                    kLoadArenaLightingSettings06
                );

    void __fastcall
    LoadArenaLightingSettings06_Hook(
        void* stadium,
        void*,
        const char* stadiumShortName)
    {
        Log(
            "NBA Live 06 GameStadium::Setup detected stadium: arg='%s' this+0x58='%s'",
            stadiumShortName
                ? stadiumShortName
                : "<null>",
            stadium
                ? reinterpret_cast<const char*>(
                    reinterpret_cast<uintptr_t>(
                        stadium) + 0x58)
                : "<null>"
        );

        gOriginalLoadArenaLightingSettings06(
            stadium,
            stadiumShortName
        );

        LoadAndApplyStadiumConfig(
            stadiumShortName
        );
    }

    void InstallStadiumConfig06()
    {
        gGame = {
            GAME_2006,
            0x0060C3A0, // GameStadium::Update
            0x180,
            0x0060C3DF, // lea esi,[ecx*4]
            0x00CB37D4, // period ticks
            0x00CB3A68, // transition ticks
            false,
            kDefaultPeriodSeconds,
            kDefaultTransitionSeconds
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        Log(
            "Initializing NBA Live 06 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupLightingCallSite06,
            LoadArenaLightingSettings06_Hook
        );

        Log(
            "NBA Live 06 Setup hook installed at 0x%08X.",
            static_cast<unsigned int>(
                kSetupLightingCallSite06)
        );
    }
    // ============================================================
    // NBA Live 07
    // ============================================================

    constexpr uintptr_t kSetupDornaCallSite07 =
        0x0069466C;

    constexpr uintptr_t kSetupDornaFunction07 =
        0x00686E10;

    using SetupDorna07_t =
        int (__thiscall*)(
            void* dornaObject,
            const char* stadiumTeamAbbr,
            void* gameContext
        );

    SetupDorna07_t gOriginalSetupDorna07 =
        reinterpret_cast<SetupDorna07_t>(
            kSetupDornaFunction07
        );

    int __fastcall SetupDorna07_Hook(
        void* dornaObject,
        void*,
        const char* stadiumTeamAbbr,
        void* gameContext)
    {
        Log(
            "NBA Live 07 Dorna setup detected stadium: arg='%s'",
            stadiumTeamAbbr
                ? stadiumTeamAbbr
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumTeamAbbr
        );

        return gOriginalSetupDorna07(
            dornaObject,
            stadiumTeamAbbr,
            gameContext
        );
    }

    void InstallStadiumConfig07()
    {
        gGame = {
            GAME_2007,
            0x00686A90, // GameStadium::UpdateAdBoardScroll
            0x220,
            0x00686B0A, // lea ecx,[esi*4]
            0,
            0,
            true,
            8.6000004f,
            0.60000002f
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        gDornaPeriodSecondsRuntime =
            gGame.originalPeriodSeconds;

        gDornaTransitionSecondsRuntime =
            gGame.originalTransitionSeconds;

        Log(
            "Initializing NBA Live 07 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupDornaCallSite07,
            SetupDorna07_Hook
        );

        Log(
            "NBA Live 07 Dorna setup hook installed at 0x%08X -> 0x%08X.",
            static_cast<unsigned int>(
                kSetupDornaCallSite07),
            static_cast<unsigned int>(
                kSetupDornaFunction07)
        );
    }

    // ============================================================
    // NBA Live 08
    // ============================================================

    constexpr uintptr_t kSetupDornaCallSite08 =
        0x006BB607;

    constexpr uintptr_t kSetupDornaFunction08 =
        0x006AAF50;

    using SetupDorna08_t =
        int (__thiscall*)(
            void* dornaObject,
            const char* stadiumTeamAbbr,
            void* gameContext
        );

    SetupDorna08_t gOriginalSetupDorna08 =
        reinterpret_cast<SetupDorna08_t>(
            kSetupDornaFunction08
        );

    int __fastcall SetupDorna08_Hook(
        void* dornaObject,
        void*,
        const char* stadiumTeamAbbr,
        void* gameContext)
    {
        Log(
            "NBA Live 08 Dorna setup detected stadium: arg='%s'",
            stadiumTeamAbbr
                ? stadiumTeamAbbr
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumTeamAbbr
        );

        return gOriginalSetupDorna08(
            dornaObject,
            stadiumTeamAbbr,
            gameContext
        );
    }

    void InstallStadiumConfig08()
    {
        gGame = {
            GAME_2008,
            0x006AAB20, // GameStadium::UpdateAdBoardScroll
            0x240,
            0x006AABCD, // lea ecx,[esi*4]
            0,
            0,
            true,
            8.6000004f,
            0.60000002f
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        gDornaPeriodSecondsRuntime =
            gGame.originalPeriodSeconds;

        gDornaTransitionSecondsRuntime =
            gGame.originalTransitionSeconds;

        Log(
            "Initializing NBA Live 08 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupDornaCallSite08,
            SetupDorna08_Hook
        );

        Log(
            "NBA Live 08 Dorna setup hook installed at 0x%08X -> 0x%08X.",
            static_cast<unsigned int>(
                kSetupDornaCallSite08),
            static_cast<unsigned int>(
                kSetupDornaFunction08)
        );
    }

}


using namespace stadiumconfigdetail;

bool ResolveStadiumCameraPath(
    const char* stadiumId,
    char* outPath,
    size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return false;

    outPath[0] = '\0';

    const std::string normalizedId =
        NormalizeStadiumId(stadiumId);

    if (normalizedId.empty())
        return false;

    std::string cameraPath;

    if (normalizedId == gCurrentStadiumId)
    {
        cameraPath = gCurrentCameraPath;
    }
    else
    {
        StadiumConfigData config{};

        if (!LoadStadiumConfig(
                normalizedId,
                config) ||
            !config.hasCamera)
        {
            return false;
        }

        cameraPath = config.camera;
    }

    if (cameraPath.empty() ||
        cameraPath.size() + 1 > outPathSize)
    {
        return false;
    }

    strcpy_s(
        outPath,
        outPathSize,
        cameraPath.c_str()
    );

    return true;
}

// ============================================================
// Public entry point
// ============================================================

void InitializeStadiumDornaConfig()
{
    stadiumconfigdetail::InitializeDebugLogging();

    switch (FM::GetEntryPoint())
    {
    case 0xCD8005:
        // NBA Live 2005
        InstallStadiumConfig05();
        break;

    case 0x40109F:
        if (patch::GetFloat(
                0xBD832C) == 1.3333334f)
        {
            // NBA Live 06
            InstallStadiumConfig06();
        }
        else if (patch::GetFloat(
                     0xBBBC3C) == 1.3333334f)
        {
            // NBA Live 07
            InstallStadiumConfig07();
        }
        else if (patch::GetFloat(
                     0xC3DF84) == 1.3333334f)
        {
            // NBA Live 08
            InstallStadiumConfig08();
        }
        break;

    default:
        break;
    }
}
