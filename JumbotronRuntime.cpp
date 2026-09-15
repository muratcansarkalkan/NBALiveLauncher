#include "plugin-std.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

using namespace plugin;

namespace jumbotron
{
    // ---------------------------------------------------------------------
    // NBA Live 06 PC addresses
    // ---------------------------------------------------------------------

    static constexpr uintptr_t kRunRMConstructors =
        0x006F1C47;

    static constexpr uintptr_t kRunRMConstructorsCall =
        0x006F4448;

    static constexpr uintptr_t kGetLoaderContext =
        0x006E6050;

    static constexpr uintptr_t kPublishRuntime =
        0x006FEA4D;

    static constexpr uintptr_t kRMState =
        0x00CBFC41;

    static constexpr uintptr_t kScrollTextureDimRuntime =
        0x00C8C114;

    static constexpr uintptr_t kScrollTextureDimDraw =
        0x009EF090;

    // ScrollTextureDim::Draw uses this when an external binding is an
    // instance array. It is the current render-instance index.
    static constexpr uintptr_t kCurrentInstanceIndex =
        0x00CC0A3C;

    static constexpr size_t kRenderMethodRuntimeSize =
        0x28;

    // Deliberately obvious horizontal UV shift for the first test.
    // UVOffsetAndRGBScale.x += 0.25f
    static constexpr float kTestUOffset =
        0.25f;


    // ---------------------------------------------------------------------
    // Runtime / binding layouts
    // ---------------------------------------------------------------------

    // 0x006FEA4D consumes exportName (+0x0C) and runtime (+0x10).
    struct RMExportRecord
    {
        uint32_t unused00;        // +0x00
        uint32_t unused04;        // +0x04
        uint32_t unused08;        // +0x08

        const char* exportName;    // +0x0C
        void* runtime;             // +0x10

        uint32_t unused14;
        uint32_t unused18;
        uint32_t unused1C;
        uint32_t unused20;
    };

    static_assert(sizeof(RMExportRecord) == 0x24);


    // ScrollTextureDim has four 0x14-byte external-binding records.
    //
    // For each record:
    //   +0x0C = data pointer
    //   +0x10 = flags
    //
    // flags bit 0 = data is a per-instance array
    // flags bit 1 = dirty / changed
    struct ExternalBinding
    {
        uint32_t unknown00;
        uint32_t unknown04;
        uint32_t unknown08;
        void* data;
        uint32_t flags;
    };

    static_assert(sizeof(ExternalBinding) == 0x14);


    using RunRMConstructorsFn =
        void(__cdecl*)();

    using GetLoaderContextFn =
        void* (__cdecl*)();

    using PublishRuntimeFn =
        void(__thiscall*)(
            RMExportRecord* self,
            void* loaderContext
        );

    // 0x009EF090 is a cdecl function with two arguments.
    using ScrollTextureDimDrawFn =
        void(__cdecl*)(
            void* drawContext,
            void* drawData
        );


    // ---------------------------------------------------------------------
    // Logging
    // ---------------------------------------------------------------------

    static void Log(const char* format, ...)
    {
        FILE* f = nullptr;

        if (fopen_s(&f, "JumboRuntime.log", "a") != 0 || !f)
            return;

        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);

        fprintf(f, "\n");
        fclose(f);
    }


    // ---------------------------------------------------------------------
    // Custom JumboTest runtime
    // ---------------------------------------------------------------------

    static void __cdecl JumboDraw(
        void* drawContext,
        void* drawData
    );

    // The native runtime's +0x00 points to a one-entry draw-handler table.
    static void* gJumboDrawHandlerTable[1] =
    {
        reinterpret_cast<void*>(&JumboDraw)
    };

    // We copy the complete 0x28-byte native ScrollTextureDim runtime here,
    // then replace only +0x00 with our draw-handler table.
    alignas(16) static uint8_t gJumboRuntime[kRenderMethodRuntimeSize]{};

    static bool gRuntimeCloneReady = false;
    static bool gLoggedFirstDraw = false;

    static RMExportRecord gJumboTestExport =
    {
        0,
        0,
        0,

        "gJumboTest_RMRuntime",
        nullptr,

        0,
        0,
        0,
        0
    };


    static void BuildRuntimeClone()
    {
        if (gRuntimeCloneReady)
            return;

        const void* nativeRuntime =
            reinterpret_cast<const void*>(
                kScrollTextureDimRuntime
            );

        std::memcpy(
            gJumboRuntime,
            nativeRuntime,
            sizeof(gJumboRuntime)
        );

        // RenderMethodRuntime + 0x00 = pointer to draw-handler table.
        *reinterpret_cast<void**>(
            gJumboRuntime + 0x00
        ) = gJumboDrawHandlerTable;

        gJumboTestExport.runtime =
            gJumboRuntime;

        gRuntimeCloneReady = true;

        const uint32_t numVariables =
            *reinterpret_cast<uint32_t*>(
                gJumboRuntime + 0x20
            );

        const uint32_t numExternalVariables =
            *reinterpret_cast<uint32_t*>(
                gJumboRuntime + 0x24
            );

        Log(
            "Built JumboTest runtime clone: native=%p clone=%p "
            "drawTable=%p vars=%u externals=%u",
            reinterpret_cast<void*>(kScrollTextureDimRuntime),
            gJumboRuntime,
            gJumboDrawHandlerTable,
            static_cast<unsigned>(numVariables),
            static_cast<unsigned>(numExternalVariables)
        );
    }


    // ---------------------------------------------------------------------
    // JumboTest Draw
    // ---------------------------------------------------------------------

    static void __cdecl JumboDraw(
        void* drawContext,
        void* drawData
    )
    {
        auto nativeDraw =
            reinterpret_cast<ScrollTextureDimDrawFn>(
                kScrollTextureDimDraw
            );

        if (!drawData)
        {
            nativeDraw(drawContext, drawData);
            return;
        }

        // ScrollTextureDim::Draw:
        //   [drawData + 0x04] = ExternalBinding*
        //
        // External #3 is:
        //   EA::Math::Vector4 UVOffsetAndRGBScale
        auto* externalBindings =
            *reinterpret_cast<ExternalBinding**>(
                reinterpret_cast<uint8_t*>(drawData) + 0x04
            );

        if (!externalBindings)
        {
            nativeDraw(drawContext, drawData);
            return;
        }

        ExternalBinding& uvBinding =
            externalBindings[3];

        void* const oldData =
            uvBinding.data;

        const uint32_t oldFlags =
            uvBinding.flags;


        // Preserve the material's original Vector4, including RGB scale.
        //
        // If bit 0 is set, EA treats data as a Vector4 array and selects
        // the current instance with a 0x10-byte stride.
        const float* sourceVector =
            reinterpret_cast<const float*>(
                oldData
            );

        if (sourceVector && (oldFlags & 1u))
        {
            const uint16_t instanceIndex =
                *reinterpret_cast<volatile uint16_t*>(
                    kCurrentInstanceIndex
                );

            sourceVector =
                reinterpret_cast<const float*>(
                    reinterpret_cast<const uint8_t*>(
                        sourceVector
                    ) +
                    static_cast<uint32_t>(
                        instanceIndex
                    ) * 0x10u
                );
        }


        // Persistent storage is important: native ScrollTextureDim::Draw
        // caches this pointer while it runs.
        alignas(16) static float testVector[4];

        if (sourceVector)
        {
            std::memcpy(
                testVector,
                sourceVector,
                sizeof(testVector)
            );
        }
        else
        {
            // Safe diagnostic fallback.
            testVector[0] = 0.0f;
            testVector[1] = 0.0f;
            testVector[2] = 1.0f;
            testVector[3] = 1.0f;
        }


        // First visual proof:
        // shift only JumboTest horizontally by 1/4 texture width.
        testVector[0] +=
            kTestUOffset;


        if (!gLoggedFirstDraw)
        {
            Log(
                "JumboDraw reached: drawData=%p ext=%p "
                "oldData=%p flags=%08X "
                "UVOffsetAndRGBScale=(%.6f, %.6f, %.6f, %.6f) "
                "test=(%.6f, %.6f, %.6f, %.6f)",
                drawData,
                externalBindings,
                oldData,
                static_cast<unsigned>(oldFlags),

                sourceVector ? sourceVector[0] : 0.0f,
                sourceVector ? sourceVector[1] : 0.0f,
                sourceVector ? sourceVector[2] : 0.0f,
                sourceVector ? sourceVector[3] : 0.0f,

                testVector[0],
                testVector[1],
                testVector[2],
                testVector[3]
            );

            gLoggedFirstDraw = true;
        }


        // Replace only external #3 for this draw.
        //
        // Clear bit 0 so our replacement is treated as one Vector4,
        // not an instance array.
        //
        // Set bit 1 so ScrollTextureDim notices that the value changed.
        uvBinding.data =
            testVector;

        uvBinding.flags =
            (oldFlags & ~1u) | 2u;


        nativeDraw(
            drawContext,
            drawData
        );


        // Restore the real material binding immediately afterward so the
        // custom material does not contaminate normal renderer state.
        uvBinding.data =
            oldData;

        uvBinding.flags =
            oldFlags;
    }


    // ---------------------------------------------------------------------
    // Runtime publication
    // ---------------------------------------------------------------------

    static void PublishAlias(
        const char* stage
    )
    {
        BuildRuntimeClone();

        auto getLoader =
            reinterpret_cast<GetLoaderContextFn>(
                kGetLoaderContext
            );

        void* loaderContext =
            getLoader();

        const uint8_t state =
            *reinterpret_cast<uint8_t*>(
                kRMState
            );

        Log(
            "[%s] state=%u loader=%p runtime=%p",
            stage,
            static_cast<unsigned>(state),
            loaderContext,
            gJumboTestExport.runtime
        );


        if (!loaderContext)
        {
            Log(
                "[%s] DynamicLoader not available yet.",
                stage
            );

            return;
        }


        auto publish =
            reinterpret_cast<PublishRuntimeFn>(
                kPublishRuntime
            );

        publish(
            &gJumboTestExport,
            loaderContext
        );

        Log(
            "[%s] published %s -> %p",
            stage,
            gJumboTestExport.exportName,
            gJumboTestExport.runtime
        );
    }


    static void __cdecl RunRMConstructorsHook()
    {
        Log(
            "RunRMConstructorsHook entered."
        );

        // Native EA runtime initialization first.
        reinterpret_cast<RunRMConstructorsFn>(
            kRunRMConstructors
        )();

        // Publish our clone after all native callbacks/resources exist.
        PublishAlias(
            "after-native-rm"
        );

        Log(
            "RunRMConstructorsHook finished."
        );
    }


    void Initialize()
    {
        BuildRuntimeClone();

        const uint8_t state =
            *reinterpret_cast<uint8_t*>(
                kRMState
            );

        auto getLoader =
            reinterpret_cast<GetLoaderContextFn>(
                kGetLoaderContext
            );

        void* loaderContext =
            getLoader();

        Log(
            "----------------------------------------"
        );

        Log(
            "Jumbotron runtime init: state=%u loader=%p",
            static_cast<unsigned>(state),
            loaderContext
        );


        // Late ASI initialization: loader already exists.
        if (loaderContext)
        {
            PublishAlias(
                "plugin-init"
            );
        }


        // Early ASI initialization: intercept EA's normal RM constructor pass.
        if (state == 0)
        {
            patch::RedirectCall(
                kRunRMConstructorsCall,
                RunRMConstructorsHook
            );

            Log(
                "Installed RunRMConstructors hook at %08X.",
                static_cast<unsigned>(
                    kRunRMConstructorsCall
                )
            );
        }
        else
        {
            PublishAlias(
                "late-plugin-init"
            );
        }
    }
}


void InitializeJumbotronRuntime()
{
    jumbotron::Initialize();
}
