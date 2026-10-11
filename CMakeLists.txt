cmake_minimum_required(VERSION 3.22)
project(mikeDAW VERSION 0.2.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OPENDAW_PUBLIC_BUILD
    "Public release: no license gate, no auth server, no auto-updater" ON)

# VST2 hosting requires the (no longer distributed) VST2 SDK. If present, VST2
# plugin loading is enabled in addition to VST3.
set(OPENDAW_VST2_SDK_PATH "C:/GitHuhb/vst_sdk2_4_rev2/vstsdk2.4" CACHE PATH
    "Path to a VST2 SDK containing pluginterfaces/vst2.x/aeffect.h")

# ASIO output (direct hardware interfaces) requires the Steinberg ASIO SDK.
# If present, the ASIO device type joins WASAPI/DirectSound in the device list.
set(OPENDAW_ASIO_SDK_PATH "C:/GitHuhb/asiosdk" CACHE PATH
    "Path to the Steinberg ASIO SDK containing common/iasiodrv.h")

add_subdirectory(external/JUCE)

if(WIN32 AND EXISTS "${OPENDAW_ASIO_SDK_PATH}/common/iasiodrv.h")
    set(OPENDAW_HAS_ASIO 1)
    message(STATUS "mikeDAW: ASIO SDK found - ASIO device support enabled")
else()
    set(OPENDAW_HAS_ASIO 0)
    if(WIN32)
        message(STATUS "mikeDAW: ASIO SDK not found - WASAPI/DirectSound only")
    else()
        message(STATUS "mikeDAW: Linux audio backend - ALSA")
    endif()
endif()

if(EXISTS "${OPENDAW_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
    juce_set_vst2_sdk_path("${OPENDAW_VST2_SDK_PATH}")
    set(OPENDAW_HAS_VST2 1)
    message(STATUS "mikeDAW: VST2 SDK found - VST2 hosting enabled")
else()
    set(OPENDAW_HAS_VST2 0)
    message(STATUS "mikeDAW: VST2 SDK not found - VST3 hosting only")
endif()

juce_add_gui_app(mikeDAW
    PRODUCT_NAME "mikeDAW"
    COMPANY_NAME "mikeDAW Project"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/opendaw.png"
    VERSION 0.2.2)

juce_generate_juce_header(mikeDAW)

target_sources(mikeDAW PRIVATE
    Source/Main.cpp
    Source/Model/ProjectModel.cpp
    Source/Model/Automation.cpp
    Source/Import/FLPImport.cpp
    Source/Import/FLNativeFx.cpp
    Source/Import/FLPExport.cpp
    Source/Import/Vst3ClassId.cpp
    Source/Import/MidiImport.cpp
    Source/Engine/AudioEngine.cpp
    Source/Engine/BuiltinDelay.cpp
    Source/Engine/BuiltinDelayBank.cpp
    Source/Engine/BuiltinReverb.cpp
    Source/Engine/BuiltinLimiter.cpp
    Source/Engine/BuiltinMultiband.cpp
    Source/Engine/BuiltinSoundbeastifier.cpp
    Source/Engine/BuiltinVoid.cpp
    Source/Engine/BuiltinLowLifter.cpp
    Source/Engine/BuiltinParamEQ.cpp
    Source/Engine/BuiltinGrossBeat.cpp
    Source/Engine/BuiltinConvolver.cpp
    Source/Engine/BuiltinParterre.cpp
    Source/Engine/BuiltinEquivocal.cpp
    Source/Engine/BuiltinEquivocalEditor.cpp
    Source/Engine/PluginHost.cpp
    Source/Engine/Bridge32Host.cpp
    Source/Net/Session.cpp
    Source/Net/TreeSync.cpp
    Source/Net/DiscordPresence.cpp
    Source/Net/LanDiscovery.cpp
    Source/Net/InviteCode.cpp
    Source/Net/FriendsManager.cpp
    Source/Net/RelayClient.cpp
    Source/Net/RelayTunnel.cpp
    Source/UI/LoadProgress.cpp
    Source/UI/MainComponent.cpp
    Source/UI/ThemeManager.cpp
    Source/UI/TransportBar.cpp
    Source/UI/BrowserPanel.cpp
    Source/UI/ChannelRack.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/Playlist.cpp
    Source/UI/Mixer.cpp
    Source/Debug/ImGuiCanvas.cpp
    Source/Debug/DebugOverlayUI.cpp
    Source/Debug/PerfMonitor.cpp
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp)

if(WIN32)
    target_sources(mikeDAW PRIVATE Source/Engine/Mp3Writer.cpp)
endif()

# The public build does not compile the license gate or the token-gated updater
# at all, so no code path in that exe can degrade or contact the auth servers.
if(OPENDAW_PUBLIC_BUILD)
    target_compile_definitions(mikeDAW PRIVATE OPENDAW_PUBLIC_BUILD=1)
else()
    target_compile_definitions(mikeDAW PRIVATE OPENDAW_PUBLIC_BUILD=0)
    target_sources(mikeDAW PRIVATE
        Source/Net/License.cpp
        Source/Net/Updater.cpp
        external/monocypher/monocypher.c
        external/monocypher/monocypher-ed25519.c)
endif()

target_include_directories(mikeDAW PRIVATE Source
    external/imgui
    external/imgui/backends
    external/monocypher)

if(OPENDAW_HAS_ASIO)
    target_include_directories(mikeDAW PRIVATE "${OPENDAW_ASIO_SDK_PATH}/common")
endif()

target_compile_definitions(mikeDAW PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:mikeDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:mikeDAW,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=${OPENDAW_HAS_VST2}
    JUCE_PLUGINHOST_LV2=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # audio-export encoders: WAV is always available; enable FLAC + Ogg Vorbis
    # (native), and the LAME command-line wrapper for MP3 (needs lame.exe at run)
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_LAME_AUDIO_FORMAT=1)

if(WIN32)
    target_compile_definitions(mikeDAW PRIVATE
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=1
        JUCE_ASIO=${OPENDAW_HAS_ASIO})
else()
    target_compile_definitions(mikeDAW PRIVATE
        JUCE_ALSA=1
        JUCE_JACK=0)

    find_program (OPENDAW_LAME_EXECUTABLE lame)
    if(OPENDAW_LAME_EXECUTABLE)
        target_compile_definitions(mikeDAW PRIVATE
            OPENDAW_LAME_EXECUTABLE="${OPENDAW_LAME_EXECUTABLE}")
        message(STATUS "mikeDAW: LAME encoder found at ${OPENDAW_LAME_EXECUTABLE}")
    else()
        message(WARNING "mikeDAW: LAME encoder not found - Linux MP3 export will be unavailable")
    endif()
endif()

target_link_libraries(mikeDAW PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_cryptography
    juce::juce_gui_extra
    juce::juce_opengl
    juce::juce_recommended_config_flags)

if(WIN32)
    # ole32/oleaut32: IUPnPNAT COM. iphlpapi: default-gateway lookup for NAT-PMP.
    # ws2_32 is pulled in by JUCE already but listed for the socket helpers.
    # mfplat/mfreadwrite/mfuuid: native MP3 export via IMFSinkWriter (Mp3Writer).
    target_link_libraries(mikeDAW PRIVATE ole32 oleaut32 iphlpapi ws2_32
                          mfplat mfreadwrite mfuuid)
    if(MSVC)
        # Release PDB so CrashLogs/*.dmp resolve mikeDAW frames. /DEBUG alone
        # disables /OPT:REF,ICF and changes the layout, so re-enable them.
        target_compile_options(mikeDAW PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(mikeDAW PRIVATE
            $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
endif()
