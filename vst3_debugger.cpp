// vst3_debugger.cpp
// VST3 plug-in debug launcher for macOS
//
// Loads a .vst3 bundle step-by-step, logging every API call before invoking it.
// A signal handler catches crashes (SIGSEGV/SIGBUS/SIGILL) and reports which
// VST3 call caused the crash.
//
// Build:
//   clang++ -std=c++17 -g \
//       -framework CoreFoundation -framework AppKit \
//       -o vst3_debugger vst3_debugger.cpp
//
// Usage:
//   ./vst3_debugger "/Library/Audio/Plug-Ins/VST3/Rhodes Pianology.vst3"
//   ./vst3_debugger ~/Library/Audio/Plug-Ins/VST3/MyPlugin.vst3 [--class-index N]
//   ./vst3_debugger ... --skip-editor     (skip IEditController / IPlugView)
//   ./vst3_debugger ... --skip-dlclose
//   ./vst3_debugger ... --dlclose-delay N

#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <iostream>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// ============================================================
// Minimal VST3 interface definitions (no SDK dependency)
// Adapted from Steinberg VST3 SDK (BSD 3-Clause).
// ============================================================

typedef int32_t   tresult;
typedef char      String128[128];
typedef int32_t   TBool;
typedef int32_t   int32;
typedef uint32_t  uint32;
typedef int64_t   int64;
typedef uint64_t  uint64;
typedef double    SampleRate;
typedef uint64_t  SpeakerArrangement;

static const tresult kResultOk        = 0;
static const tresult kResultFalse     = 1;
static const tresult kNoInterface     = (tresult)0x80004002u;
static const tresult kNotImplemented  = (tresult)0x80004001u;
static const tresult kInternalError   = (tresult)0x80004005u;
static const tresult kInvalidArgument = (tresult)0x80070057u;

static std::string tresultStr(tresult r) {
    switch ((uint32_t)r) {
        case 0x00000000u: return "kResultOk (S_OK)";
        case 0x00000001u: return "kResultFalse (S_FALSE)";
        case 0x80004002u: return "kNoInterface (E_NOINTERFACE)";
        case 0x80004001u: return "kNotImplemented (E_NOTIMPL)";
        case 0x80004005u: return "kInternalError (E_FAIL)";
        case 0x80070057u: return "kInvalidArgument (E_INVALIDARG)";
        default: {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)r);
            return buf;
        }
    }
}

static std::string tuidStr(const uint8_t* uid) {
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        uid[0],  uid[1],  uid[2],  uid[3],  uid[4],  uid[5],  uid[6],  uid[7],
        uid[8],  uid[9],  uid[10], uid[11], uid[12], uid[13], uid[14], uid[15]);
    return buf;
}

#define DEFINE_IID(sym, b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,ba,bb,bc,bd,be,bf) \
    static const uint8_t sym[16] = {b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,ba,bb,bc,bd,be,bf}

DEFINE_IID(IID_FUnknown,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_IID(IID_IPluginFactory,
    0x7A,0x4D,0x81,0x1C,0x52,0x11,0x4A,0x1F,0xAE,0xD9,0xD2,0xEE,0x0B,0x43,0xBF,0x9F);
DEFINE_IID(IID_IPluginBase,
    0x22,0x88,0x8D,0xDB,0x15,0x6E,0x45,0xAE,0x83,0x58,0xB3,0x48,0x08,0x19,0x06,0x25);
DEFINE_IID(IID_IComponent,
    0xE8,0x31,0xFF,0x31,0xF2,0xD5,0x43,0x01,0x92,0x8E,0xBB,0xEE,0x25,0x69,0x78,0x02);
DEFINE_IID(IID_IAudioProcessor,
    0x42,0x04,0x3F,0x99,0xB7,0xDA,0x45,0x3C,0xA5,0x69,0xE7,0x9D,0x9A,0xAE,0xC3,0x3D);
DEFINE_IID(IID_IEditController,
    0xDC,0xD7,0xBB,0xE3,0x77,0x42,0x44,0x8D,0xA8,0x74,0xAA,0xCC,0x97,0x9C,0x75,0x9E);
DEFINE_IID(IID_IHostApplication,
    0x58,0xE5,0x95,0xCC,0xDB,0x2D,0x49,0x69,0x8B,0x6A,0xAF,0x8C,0x36,0xA6,0x64,0xE5);
DEFINE_IID(IID_IPlugView,
    0xD2,0x45,0x5E,0xC6,0xC4,0x23,0xA8,0x08,0x1B,0x5B,0x6C,0xC4,0xBF,0x67,0x25,0x14);
DEFINE_IID(IID_IComponentHandler,
    0x93,0xA0,0xBE,0xA3,0x0C,0x55,0x46,0x77,0x87,0xCA,0xB6,0xBC,0x0D,0xC4,0xE7,0x87);
DEFINE_IID(IID_IPlugFrame,
    0x86,0x34,0x05,0x09,0x3B,0x35,0x4F,0x1A,0x88,0xCB,0x5E,0x26,0x45,0x53,0xA4,0x14);
DEFINE_IID(IID_IConnectionPoint,
    0x70,0xA4,0x15,0x6F,0x6B,0xBE,0x4C,0x8D,0xBA,0x33,0x01,0x71,0xF8,0x69,0xE9,0x64);
DEFINE_IID(IID_IPluginFactory2,
    0x0C,0x1D,0xB6,0x82,0xAA,0xF2,0x44,0x40,0xB0,0xFB,0x67,0x70,0x6B,0xAA,0x42,0xC2);
DEFINE_IID(IID_IPluginFactory3,
    0x4,0x55,0x5A,0x2C,0xC1,0x9E,0x40,0x96,0xBE,0xEE,0xEE,0xA7,0xC6,0xF6,0x4B,0x74);
DEFINE_IID(IID_IEditController2,
    0x7F,0x4E,0xFE,0x59,0xF3,0x20,0x4C,0xF9,0xA5,0x31,0xA4,0x98,0x10,0xC9,0x71,0xA4);

// FUnknown vtable: [0] queryInterface  [1] addRef  [2] release
struct FUnknown {
    virtual tresult queryInterface(const void* iid, void** obj) = 0;
    virtual uint32  addRef()  = 0;
    virtual uint32  release() = 0;
};

struct PFactoryInfo { char vendor[64]; char url[256]; char email[128]; int32 flags; };
struct PClassInfo   { uint8_t cid[16]; int32 cardinality; char category[32]; char name[64]; };

// IPluginFactory vtable: [0-2] FUnknown, [3] getFactoryInfo, [4] countClasses,
//                        [5] getClassInfo, [6] createInstance
struct IPluginFactory : FUnknown {
    virtual tresult getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32   countClasses() = 0;
    virtual tresult getClassInfo(int32 index, PClassInfo* info) = 0;
    virtual tresult createInstance(const void* cid, const void* iid, void** obj) = 0;
};

// PClassInfo2 / PClassInfoW — extended class info returned by IPluginFactory2/3
struct PClassInfo2 {
    uint8_t cid[16];
    int32   cardinality;
    char    category[32];
    char    name[64];
    uint32  classFlags;
    char    subCategories[128];
    char    vendor[64];
    char    version[64];
    char    sdkVersion[64];
};

// IPluginFactory2: adds getClassInfo2
struct IPluginFactory2 : IPluginFactory {
    virtual tresult getClassInfo2(int32 index, PClassInfo2* info) = 0;
};

// IPluginFactory3: adds getClassInfoUnicode and setHostContext
struct IPluginFactory3 : IPluginFactory2 {
    virtual tresult getClassInfoUnicode(int32 index, void* info) = 0; // PClassInfoW
    virtual tresult setHostContext(FUnknown* context) = 0;
};

// IEditController2: adds setKnobMode and openHelp/openAboutBox
struct IEditController2 : FUnknown {
    virtual tresult setKnobMode(int32 mode) = 0;
    virtual tresult openHelp(TBool onlyCheck) = 0;
    virtual tresult openAboutBox(TBool onlyCheck) = 0;
};

// IPluginBase vtable: [0-2] FUnknown, [3] initialize, [4] terminate
struct IPluginBase : FUnknown {
    virtual tresult initialize(FUnknown* context) = 0;
    virtual tresult terminate() = 0;
};

typedef int32 MediaType;
typedef int32 BusDirection;
typedef int32 BusType;
typedef int32 IoMode;
enum MediaTypeE    { kAudio = 0, kEvent = 1 };
enum BusDirectionE { kInput = 0, kOutput = 1 };
enum IoModeE       { kSimple = 0 };
enum ProcessModeE  { kRealtime = 0 };
enum SampleSizeE   { kSample32 = 0, kSample64 = 1 };

struct BusInfo {
    MediaType mediaType; BusDirection direction; int32 channelCount;
    String128 name; BusType busType; uint32 flags;
};
struct RoutingInfo { MediaType mediaType; int32 busIndex; int32 channel; };

struct IBStream : FUnknown {
    virtual tresult read(void* buf, int32 n, int32* nRead) = 0;
    virtual tresult write(void* buf, int32 n, int32* nWritten) = 0;
    virtual tresult seek(int64 pos, int32 mode, int64* result) = 0;
    virtual tresult tell(int64* pos) = 0;
};

// IComponent vtable: [0-2] FUnknown, [3-4] IPluginBase,
//   [5] getControllerClassId, [6] setIoMode, [7] getBusCount,
//   [8] getBusInfo, [9] getRoutingInfo, [10] activateBus,
//   [11] setActive, [12] setState, [13] getState
struct IComponent : IPluginBase {
    virtual tresult getControllerClassId(void* classId) = 0;
    virtual tresult setIoMode(IoMode mode) = 0;
    virtual int32   getBusCount(MediaType type, BusDirection dir) = 0;
    virtual tresult getBusInfo(MediaType type, BusDirection dir, int32 idx, BusInfo& bus) = 0;
    virtual tresult getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult activateBus(MediaType type, BusDirection dir, int32 idx, TBool state) = 0;
    virtual tresult setActive(TBool state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
};

struct ProcessSetup {
    int32 processMode; int32 symbolicSampleSize; int32 maxSamplesPerBlock; SampleRate sampleRate;
};
struct ProcessData; // process() is not called in this launcher

// IAudioProcessor vtable: [0-2] FUnknown,
//   [3] setBusArrangements, [4] getBusArrangement, [5] canProcessSampleSize,
//   [6] getLatencySamples, [7] setupProcessing, [8] setProcessing,
//   [9] process, [10] getTailSamples
struct IAudioProcessor : FUnknown {
    virtual tresult setBusArrangements(SpeakerArrangement* ins, int32 numIns,
                                       SpeakerArrangement* outs, int32 numOuts) = 0;
    virtual tresult getBusArrangement(BusDirection dir, int32 idx, SpeakerArrangement& arr) = 0;
    virtual tresult canProcessSampleSize(int32 symbolicSampleSize) = 0;
    virtual uint32  getLatencySamples() = 0;
    virtual tresult setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult setProcessing(TBool state) = 0;
    virtual tresult process(ProcessData& data) = 0;
    virtual uint32  getTailSamples() = 0;
};

// IHostApplication vtable: [0-2] FUnknown, [3] getName, [4] createInstance
struct IHostApplication : FUnknown {
    virtual tresult getName(String128 name) = 0;
    virtual tresult createInstance(const void* cid, const void* iid, void** obj) = 0;
};

// ViewRect (used by IPlugView::getSize / onSize)
struct ViewRect { int32 left, top, right, bottom; };

// IPlugFrame vtable: [0-2] FUnknown, [3] resizeView
struct IPlugView;
struct IPlugFrame : FUnknown {
    virtual tresult resizeView(IPlugView* view, ViewRect* newSize) = 0;
};

// IPlugView vtable: [0-2] FUnknown,
//   [3] isPlatformTypeSupported, [4] attached, [5] removed,
//   [6] onWheel, [7] onKeyDown, [8] onKeyUp, [9] getSize,
//   [10] onSize, [11] onFocus, [12] setFrame,
//   [13] canResize, [14] checkSizeConstraint
struct IPlugView : FUnknown {
    virtual tresult isPlatformTypeSupported(const char* type) = 0;
    virtual tresult attached(void* parent, const char* type) = 0;
    virtual tresult removed() = 0;
    virtual tresult onWheel(float distance) = 0;
    virtual tresult onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult getSize(ViewRect* size) = 0;
    virtual tresult onSize(ViewRect* newSize) = 0;
    virtual tresult onFocus(TBool state) = 0;
    virtual tresult setFrame(IPlugFrame* frame) = 0;
    virtual tresult canResize() = 0;
    virtual tresult checkSizeConstraint(ViewRect* rect) = 0;
};

// IComponentHandler vtable: [0-2] FUnknown,
//   [3] beginEdit, [4] performEdit, [5] endEdit, [6] restartComponent
struct IComponentHandler : FUnknown {
    virtual tresult beginEdit(uint32 id) = 0;
    virtual tresult performEdit(uint32 id, double valueNormalized) = 0;
    virtual tresult endEdit(uint32 id) = 0;
    virtual tresult restartComponent(int32 flags) = 0;
};

// IEditController vtable: [0-2] FUnknown, [3-4] IPluginBase,
//   [5] setComponentState, [6] setState, [7] getState,
//   [8] getParameterCount, [9] getParameterInfo,
//   [10] getParamStringByValue, [11] getParamValueByString,
//   [12] normalizedParamToPlain, [13] plainParamToNormalized,
//   [14] getParamNormalized, [15] setParamNormalized,
//   [16] setComponentHandler, [17] createView
struct ParameterInfo {
    uint32    id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32     stepCount;
    double    defaultNormalizedValue;
    int32     unitId;
    int32     flags;
};
// IMessage — opaque message passed through IConnectionPoint
struct IMessage : FUnknown {
    virtual const char* getMessageID() = 0;
    virtual void        setMessageID(const char* id) = 0;
    // IAttributeList not needed for our purposes; left out
};

// IConnectionPoint — component <-> controller communication channel
struct IConnectionPoint : FUnknown {
    virtual tresult connect(FUnknown* other) = 0;
    virtual tresult disconnect(FUnknown* other) = 0;
    virtual tresult notify(IMessage* message) = 0;
};

struct IEditController : IPluginBase {
    virtual tresult setComponentState(IBStream* state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
    virtual int32   getParameterCount() = 0;
    virtual tresult getParameterInfo(int32 paramIndex, ParameterInfo& info) = 0;
    virtual tresult getParamStringByValue(uint32 id, double valueNormalized,
                                          String128 string) = 0;
    virtual tresult getParamValueByString(uint32 id, char16_t* string,
                                          double& valueNormalized) = 0;
    virtual double  normalizedParamToPlain(uint32 id, double valueNormalized) = 0;
    virtual double  plainParamToNormalized(uint32 id, double plainValue) = 0;
    virtual double  getParamNormalized(uint32 id) = 0;
    virtual tresult setParamNormalized(uint32 id, double value) = 0;
    virtual tresult setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* createView(const char* name) = 0;
};

// Implemented in host_window.mm (Objective-C++)
extern "C" {
    void* hostview_create(int w, int h);   // returns NSView* (content view)
    void  hostview_show_and_run(double seconds);
    void  hostview_destroy();
}

// ============================================================
// Crash / signal handling
// ============================================================

// Set to true when a signal fires inside PROTECTED_CALL.
// All subsequent PROTECTED_CALL / STEP invocations become no-ops.
static bool                  g_crashed           = false;
static volatile sig_atomic_t g_inProtectedRegion = 0;
static sigjmp_buf            g_crashJmp;
static const char*           g_currentStep       = "(startup)";

#define LOG(msg)  do { std::cout << msg << "\n"; std::cout.flush(); } while(0)
#define LOGI(msg) LOG("[INFO]  " << msg)
#define LOGE(msg) do { std::cerr << "[ERROR] " << msg << "\n"; std::cerr.flush(); } while(0)

#define STEP(name) do { \
    if (!g_crashed) { \
        g_currentStep = (name); \
        std::cout << "\n[STEP]  >>> " << (name) << "\n"; \
        std::cout.flush(); \
    } \
} while(0)

#define RESULT(r) do { \
    if (!g_crashed) { \
        std::cout << "[RES]   " << tresultStr(r) << "\n"; \
        std::cout.flush(); \
    } \
} while(0)

// Wraps a single expression in crash-recovery.
// On signal: marks g_crashed=true; all later PROTECTED_CALL blocks are skipped.
#define PROTECTED_CALL(expr) do { \
    if (!g_crashed) { \
        g_inProtectedRegion = 1; \
        if (sigsetjmp(g_crashJmp, 1) != 0) { \
            g_inProtectedRegion = 0; \
            g_crashed = true; \
            LOGE("Signal caught -- remaining steps will be skipped."); \
        } else { \
            (expr); \
            g_inProtectedRegion = 0; \
        } \
    } \
} while(0)

// ============================================================
// Minimal host implementations (need logging macros above)
// ============================================================

// IComponentHandler — receives parameter-change notifications from the controller.
class MinimalComponentHandler : public IComponentHandler {
    int refCount = 1;
public:
    tresult queryInterface(const void* iid, void** obj) override {
        if (memcmp(iid, IID_IComponentHandler, 16) == 0 ||
            memcmp(iid, IID_FUnknown, 16) == 0) {
            *obj = this; addRef(); return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 addRef()  override { return static_cast<uint32>(++refCount); }
    uint32 release() override { return static_cast<uint32>(--refCount); }
    tresult beginEdit(uint32 id) override {
        LOGI("  [IComponentHandler] beginEdit(paramId=" << id << ")");
        return kResultOk;
    }
    tresult performEdit(uint32 id, double v) override {
        LOGI("  [IComponentHandler] performEdit(paramId=" << id << ", value=" << v << ")");
        return kResultOk;
    }
    tresult endEdit(uint32 id) override {
        LOGI("  [IComponentHandler] endEdit(paramId=" << id << ")");
        return kResultOk;
    }
    tresult restartComponent(int32 flags) override {
        LOGI("  [IComponentHandler] restartComponent(flags=0x" << std::hex << flags << std::dec << ")");
        return kResultOk;
    }
};

// IPlugFrame — receives resize requests from the plugin view.
class MinimalPlugFrame : public IPlugFrame {
    int refCount = 1;
public:
    tresult queryInterface(const void* iid, void** obj) override {
        if (memcmp(iid, IID_IPlugFrame, 16) == 0 ||
            memcmp(iid, IID_FUnknown, 16) == 0) {
            *obj = this; addRef(); return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 addRef()  override { return static_cast<uint32>(++refCount); }
    uint32 release() override { return static_cast<uint32>(--refCount); }
    tresult resizeView(IPlugView* /*view*/, ViewRect* r) override {
        if (r) LOGI("  [IPlugFrame] resizeView requested: "
                    << r->right - r->left << "x" << r->bottom - r->top);
        return kResultOk;
    }
};

// IHostApplication — passed as context to IPluginBase::initialize().
class MinimalHost : public IHostApplication {
    int refCount = 1;
public:
    tresult queryInterface(const void* iid, void** obj) override {
        if (memcmp(iid, IID_IHostApplication, 16) == 0 ||
            memcmp(iid, IID_FUnknown, 16) == 0) {
            *obj = static_cast<IHostApplication*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 addRef()  override { return static_cast<uint32>(++refCount); }
    uint32 release() override { return static_cast<uint32>(--refCount); }
    tresult getName(String128 name) override {
        strncpy(name, "VST3 Debugger", 127); name[127] = '\0'; return kResultOk;
    }
    tresult createInstance(const void*, const void*, void** obj) override {
        *obj = nullptr; return kNoInterface;
    }
};

static void signalHandler(int sig, siginfo_t* info, void* /*ctx*/) {
    const char* sigName =
        sig == SIGSEGV ? "SIGSEGV (segmentation fault)" :
        sig == SIGBUS  ? "SIGBUS  (bus error)"           :
        sig == SIGILL  ? "SIGILL  (illegal instruction)" :
        sig == SIGABRT ? "SIGABRT (abort)"               : "UNKNOWN";

    char msg[1024];
    snprintf(msg, sizeof(msg),
        "\n+==============================================+\n"
        "|  CRASH DETECTED                              |\n"
        "+==============================================+\n"
        "  Signal    : %s (%d)\n"
        "  Fault addr: %p\n"
        "  Step      : %s\n\n",
        sigName, sig, info ? info->si_addr : nullptr, g_currentStep);
    write(STDERR_FILENO, msg, strlen(msg));

    void* frames[32];
    int n = backtrace(frames, 32);
    const char* hdr = "  Backtrace (best-effort):\n";
    write(STDERR_FILENO, hdr, strlen(hdr));
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    write(STDERR_FILENO, "\n", 1);

    if (g_inProtectedRegion) { siglongjmp(g_crashJmp, sig); }
    _exit(1);
}

static void installSignalHandlers() {
    struct sigaction sa{};
    sa.sa_sigaction = signalHandler;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

// ============================================================
// Bundle helpers
// ============================================================
static std::string findBundleBinary(const std::string& bundlePath) {
    std::string base = bundlePath;
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::string name = base;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 5 && name.substr(name.size() - 5) == ".vst3")
        name = name.substr(0, name.size() - 5);

    const std::vector<std::string> candidates = {
        base + "/Contents/MacOS/" + name,
        base + "/Contents/MacOS/" + name + ".dylib",
        base + "/Contents/x86_64-mac/" + name,
        base + "/Contents/arm64-mac/" + name,
    };
    for (const auto& p : candidates) {
        struct stat st{};
        if (stat(p.c_str(), &st) == 0) { LOGI("Binary: " << p); return p; }
    }
    return {};
}

static void shellDiag(const std::string& cmd, const std::string& label) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return;
    LOGI(label);
    char line[512];
    while (fgets(line, sizeof(line), fp)) std::cout << "    " << line;
    pclose(fp);
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <Plugin.vst3> [--class-index N]\n"
                     "                [--dlclose-delay <seconds>]\n"
                     "                [--skip-dlclose]\n"
                     "\n"
                     "  --class-index N       Force instantiation of class N (0-based)\n"
                     "  --dlclose-delay N     Sleep N seconds before dlclose (default 0)\n"
                     "  --skip-dlclose        Don't call dlclose at all (mimics many DAWs)\n";
        return 1;
    }

    const std::string bundlePath = argv[1];
    int  targetClassIndex = -1;
    int  dlcloseDelay     = 0;
    bool skipDlclose      = false;
    bool skipEditor       = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--class-index" && i + 1 < argc)
            targetClassIndex = atoi(argv[++i]);
        else if (arg == "--dlclose-delay" && i + 1 < argc)
            dlcloseDelay = atoi(argv[++i]);
        else if (arg == "--skip-dlclose")
            skipDlclose = true;
        else if (arg == "--skip-editor")
            skipEditor = true;
    }

    std::cout <<
        "+==============================================+\n"
        "|  VST3 Debug Launcher                         |\n"
        "+==============================================+\n"
        "  Bundle: " << bundlePath << "\n\n";

    installSignalHandlers();

    // 1 -- locate binary
    STEP("1. Locate binary inside .vst3 bundle");
    std::string binaryPath = findBundleBinary(bundlePath);
    if (binaryPath.empty()) {
        LOGE("Could not find executable inside: " << bundlePath);
        return 1;
    }
    shellDiag("lipo -info \"" + binaryPath + "\" 2>&1",  "Architecture:");
    shellDiag("otool -L \""   + binaryPath + "\" 2>&1",  "Linked libraries:");

    // 2 -- dlopen
    STEP("2. dlopen(RTLD_LAZY | RTLD_LOCAL)");
    void* handle = nullptr;
    PROTECTED_CALL(handle = dlopen(binaryPath.c_str(), RTLD_LAZY | RTLD_LOCAL));
    if (!g_crashed && !handle) {
        LOGE("dlopen failed: " << dlerror()); return 1;
    }
    if (!g_crashed) LOGI("dlopen handle: " << handle);

    // 3 -- optional bundleEntry
    if (!g_crashed) {
        STEP("3. Look up optional bundleEntry symbol");
        typedef bool (*BundleEntryFn)(CFBundleRef);
        BundleEntryFn bundleEntry = nullptr;
        PROTECTED_CALL(bundleEntry = (BundleEntryFn)dlsym(handle, "bundleEntry"));
        if (!g_crashed && bundleEntry) {
            STEP("3b. Call bundleEntry(nullptr)");
            bool ok = false;
            PROTECTED_CALL(ok = bundleEntry(nullptr));
            if (!g_crashed) LOGI("bundleEntry => " << (ok ? "true" : "false"));
        } else if (!g_crashed) {
            LOGI("bundleEntry not present (optional)");
        }
    }

    // 4 -- GetPluginFactory symbol
    typedef IPluginFactory* (*GetPluginFactoryFn)();
    GetPluginFactoryFn getPluginFactory = nullptr;
    if (!g_crashed) {
        STEP("4. Look up GetPluginFactory symbol");
        PROTECTED_CALL(getPluginFactory =
            (GetPluginFactoryFn)dlsym(handle, "GetPluginFactory"));
        if (!g_crashed && !getPluginFactory) {
            LOGE("GetPluginFactory not found: " << dlerror());
            dlclose(handle); return 1;
        }
        if (!g_crashed) LOGI("GetPluginFactory at " << (void*)getPluginFactory);
    }

    // 5 -- call GetPluginFactory()
    IPluginFactory* factory = nullptr;
    if (!g_crashed) {
        STEP("5. Call GetPluginFactory()");
        PROTECTED_CALL(factory = getPluginFactory());
        if (!g_crashed && !factory) {
            LOGE("GetPluginFactory() returned nullptr");
            dlclose(handle); return 1;
        }
        if (!g_crashed) LOGI("Factory ptr: " << factory);
    }

    if (!g_crashed && factory) {
        tresult r = kResultFalse;

        // 6 -- getFactoryInfo
        STEP("6. factory->getFactoryInfo()");
        {
            PFactoryInfo fi{};
            PROTECTED_CALL(r = factory->getFactoryInfo(&fi));
            if (!g_crashed) {
                RESULT(r);
                if (r == kResultOk) {
                    LOGI("  Vendor : " << fi.vendor);
                    LOGI("  URL    : " << fi.url);
                    LOGI("  E-mail : " << fi.email);
                    LOGI("  Flags  : 0x" << std::hex << fi.flags << std::dec);
                }
            }
        }

        // 6b -- probe IPluginFactory2
        STEP("6b. factory->queryInterface(IPluginFactory2)");
        {
            IPluginFactory2* factory2 = nullptr;
            PROTECTED_CALL(r = factory->queryInterface(
                IID_IPluginFactory2, reinterpret_cast<void**>(&factory2)));
            if (!g_crashed) {
                RESULT(r);
                if (r == kResultOk && factory2) {
                    LOGI("  IPluginFactory2 supported");
                    // Dump extended class info for all registered classes
                    int32 nClasses = 0;
                    PROTECTED_CALL(nClasses = factory2->countClasses());
                    for (int32 ci = 0; ci < nClasses && !g_crashed; ++ci) {
                        PClassInfo2 ci2{};
                        PROTECTED_CALL(r = factory2->getClassInfo2(ci, &ci2));
                        if (!g_crashed && r == kResultOk) {
                            LOGI("  [" << ci << "] subCategories=\"" << ci2.subCategories
                                 << "\"  vendor=\"" << ci2.vendor
                                 << "\"  version=\"" << ci2.version
                                 << "\"  sdkVersion=\"" << ci2.sdkVersion
                                 << "\"  classFlags=0x" << std::hex << ci2.classFlags << std::dec);
                        }
                    }
                    factory2->release();
                } else {
                    LOGI("  IPluginFactory2 NOT supported");
                }
            }
        }

        // 6c -- probe IPluginFactory3 (adds setHostContext)
        STEP("6c. factory->queryInterface(IPluginFactory3)");
        {
            IPluginFactory3* factory3 = nullptr;
            PROTECTED_CALL(r = factory->queryInterface(
                IID_IPluginFactory3, reinterpret_cast<void**>(&factory3)));
            if (!g_crashed) {
                RESULT(r);
                if (r == kResultOk && factory3) {
                    LOGI("  IPluginFactory3 supported");
                    // Call setHostContext — DAWs do this to inject host services
                    static MinimalHost factoryHost;
                    STEP("6c-i. factory3->setHostContext(hostApp)");
                    PROTECTED_CALL(r = factory3->setHostContext(
                        static_cast<FUnknown*>(&factoryHost)));
                    if (!g_crashed) RESULT(r);
                    factory3->release();
                } else {
                    LOGI("  IPluginFactory3 NOT supported");
                }
            }
        }

        // 7 -- countClasses
        STEP("7. factory->countClasses()");
        int32 classCount = 0;
        PROTECTED_CALL(classCount = factory->countClasses());
        if (!g_crashed) LOGI("countClasses() = " << classCount);

        // 8 -- enumerate classes
        int     chosenIndex = -1;
        uint8_t chosenCid[16]{};

        for (int32 i = 0; i < classCount && !g_crashed; ++i) {
            std::ostringstream ss;
            ss << "8." << i << ". factory->getClassInfo(" << i << ")";
            STEP(ss.str().c_str());
            PClassInfo ci{};
            PROTECTED_CALL(r = factory->getClassInfo(i, &ci));
            if (g_crashed) break;
            RESULT(r);
            if (r != kResultOk) continue;
            LOGI("  [" << i << "] Name     : " << ci.name);
            LOGI("  [" << i << "] Category : " << ci.category);
            LOGI("  [" << i << "] CID      : " << tuidStr(ci.cid));
            if (chosenIndex < 0) {
                bool isAudio = strncmp(ci.category, "Audio Module Class", 32) == 0;
                if ((targetClassIndex < 0 && isAudio) || targetClassIndex == i) {
                    chosenIndex = i;
                    memcpy(chosenCid, ci.cid, 16);
                    LOGI("  *** Selected for instantiation ***");
                }
            }
        }

        if (!g_crashed && chosenIndex < 0) {
            std::string errMsg = targetClassIndex >= 0
                ? "Class index " + std::to_string(targetClassIndex) + " not found."
                : "No Audio Module Class found. Try --class-index N.";
            LOGE(errMsg);
        } else if (!g_crashed) {
            // 9 -- createInstance
            {
                std::ostringstream ss;
                ss << "9. factory->createInstance(class[" << chosenIndex << "], IComponent)";
                STEP(ss.str().c_str());
            }
            void* rawObj = nullptr;
            PROTECTED_CALL(r = factory->createInstance(chosenCid, IID_IComponent, &rawObj));
            if (!g_crashed) {
                RESULT(r);
                if (r != kResultOk || !rawObj) {
                    LOGE("createInstance failed or returned nullptr");
                }
            }

            IComponent* component = g_crashed ? nullptr : static_cast<IComponent*>(rawObj);
            if (!g_crashed && component) LOGI("Component raw ptr: " << component);

            // 10 -- queryInterface(IComponent) to get canonical pointer
            if (!g_crashed && component) {
                STEP("10. component->queryInterface(IComponent)");
                void* qiPtr = nullptr;
                PROTECTED_CALL(r = component->queryInterface(IID_IComponent, &qiPtr));
                if (!g_crashed) {
                    RESULT(r);
                    if (r == kResultOk && qiPtr && qiPtr != rawObj) {
                        LOGI("Canonical QI ptr: " << qiPtr);
                        component->release();
                        component = static_cast<IComponent*>(qiPtr);
                    }
                }
            }

            if (!g_crashed && component) {
                static MinimalHost hostApp;

                // 11 -- initialize
                STEP("11. component->initialize(hostContext)");
                PROTECTED_CALL(r = component->initialize(
                    static_cast<IHostApplication*>(&hostApp)));
                if (!g_crashed) RESULT(r);

                // 12 -- getControllerClassId
                STEP("12. component->getControllerClassId()");
                uint8_t ctrlCid[16]{};
                bool    hasCtrlCid = false;
                {
                    PROTECTED_CALL(r = component->getControllerClassId(ctrlCid));
                    if (!g_crashed) {
                        RESULT(r);
                        if (r == kResultOk) {
                            LOGI("Controller CID: " << tuidStr(ctrlCid));
                            hasCtrlCid = true;
                        } else {
                            LOGI("(single-component or unsupported)");
                        }
                    }
                }

                // ---- IEditController + IPlugView lifecycle ----
                // This is what Reason (and all DAWs) do immediately after
                // instantiation when they need to show the plugin UI.
                if (!g_crashed && hasCtrlCid && !skipEditor) {
                    LOGI("--- IEditController / IPlugView path ---");

                    // C-1. createInstance(controllerCid, IEditController)
                    STEP("C-1. factory->createInstance(controllerCid, IEditController)");
                    void* rawCtrl = nullptr;
                    PROTECTED_CALL(r = factory->createInstance(
                        ctrlCid, IID_IEditController, &rawCtrl));
                    if (!g_crashed) RESULT(r);

                    IEditController* controller = nullptr;
                    if (!g_crashed && r == kResultOk && rawCtrl) {
                        controller = static_cast<IEditController*>(rawCtrl);
                        LOGI("Controller ptr: " << controller);

                        // C-2. queryInterface(IEditController) — canonical pointer
                        STEP("C-2. controller->queryInterface(IEditController)");
                        {
                            void* qiPtr = nullptr;
                            PROTECTED_CALL(r = controller->queryInterface(
                                IID_IEditController, &qiPtr));
                            if (!g_crashed) {
                                RESULT(r);
                                if (r == kResultOk && qiPtr && qiPtr != rawCtrl) {
                                    controller->release();
                                    controller = static_cast<IEditController*>(qiPtr);
                                    LOGI("Canonical controller ptr: " << controller);
                                }
                            }
                        }

                        // C-3. controller->initialize(hostContext)
                        static MinimalHost ctrlHost;
                        STEP("C-3. controller->initialize(hostContext)");
                        PROTECTED_CALL(r = controller->initialize(
                            static_cast<IHostApplication*>(&ctrlHost)));
                        if (!g_crashed) RESULT(r);

                        // C-4. setComponentHandler
                        static MinimalComponentHandler compHandler;
                        STEP("C-4. controller->setComponentHandler(handler)");
                        PROTECTED_CALL(r = controller->setComponentHandler(&compHandler));
                        if (!g_crashed) RESULT(r);

                        // C-4b/C-4c. IConnectionPoint handshake.
                        // DAWs always do this before createView() so the controller
                        // receives component state and can populate its parameter list.
                        IConnectionPoint* compCP   = nullptr;
                        IConnectionPoint* ctrlCP   = nullptr;

                        STEP("C-4b. component->queryInterface(IConnectionPoint)");
                        PROTECTED_CALL(r = component->queryInterface(
                            IID_IConnectionPoint, reinterpret_cast<void**>(&compCP)));
                        if (!g_crashed) {
                            RESULT(r);
                            LOGI("  compCP: " << (void*)compCP);
                        }

                        STEP("C-4c. controller->queryInterface(IConnectionPoint)");
                        PROTECTED_CALL(r = controller->queryInterface(
                            IID_IConnectionPoint, reinterpret_cast<void**>(&ctrlCP)));
                        if (!g_crashed) {
                            RESULT(r);
                            LOGI("  ctrlCP: " << (void*)ctrlCP);
                        }

                        if (!g_crashed && compCP && ctrlCP) {
                            STEP("C-4d. compCP->connect(controller)");
                            PROTECTED_CALL(r = compCP->connect(
                                static_cast<FUnknown*>(controller)));
                            if (!g_crashed) RESULT(r);

                            STEP("C-4e. ctrlCP->connect(component)");
                            PROTECTED_CALL(r = ctrlCP->connect(
                                static_cast<FUnknown*>(component)));
                            if (!g_crashed) RESULT(r);
                        } else if (!g_crashed) {
                            LOGI("  IConnectionPoint not supported -- skipping connect");
                        }

                        // C-5. getParameterCount
                        STEP("C-5. controller->getParameterCount()");
                        int32 paramCount = 0;
                        PROTECTED_CALL(paramCount = controller->getParameterCount());
                        if (!g_crashed) LOGI("Parameter count: " << paramCount);

                        // C-6. Dump first few parameters
                        int32 dumpCount = paramCount < 8 ? paramCount : 8;
                        for (int32 pi = 0; pi < dumpCount && !g_crashed; ++pi) {
                            std::ostringstream ss;
                            ss << "C-6." << pi << ". controller->getParameterInfo(" << pi << ")";
                            STEP(ss.str().c_str());
                            ParameterInfo pi_info{};
                            PROTECTED_CALL(r = controller->getParameterInfo(pi, pi_info));
                            if (!g_crashed && r == kResultOk) {
                                // title is UTF-16; print ASCII bytes as chars
                                char title[129]{};
                                for (int k = 0; k < 128 && pi_info.title[k*2]; ++k)
                                    title[k] = (char)pi_info.title[k*2]; // lo byte only
                                LOGI("  [" << pi << "] id=" << pi_info.id
                                     << "  steps=" << pi_info.stepCount
                                     << "  flags=0x" << std::hex << pi_info.flags << std::dec
                                     << "  title=\"" << title << "\"");
                            }
                        }
                        if (!g_crashed && paramCount > 8)
                            LOGI("  ... (" << paramCount - 8 << " more parameters not shown)");

                        // C-7. createView("editor")
                        STEP("C-7. controller->createView(\"editor\")");
                        IPlugView* plugView = nullptr;
                        PROTECTED_CALL(plugView = controller->createView("editor"));
                        if (!g_crashed) {
                            if (plugView)
                                LOGI("IPlugView ptr: " << plugView);
                            else
                                LOGI("createView(\"editor\") returned nullptr -- no UI / view not supported");
                        }

                        if (!g_crashed && plugView) {
                            // C-8. isPlatformTypeSupported("NSView")
                            STEP("C-8. plugView->isPlatformTypeSupported(\"NSView\")");
                            PROTECTED_CALL(r = plugView->isPlatformTypeSupported("NSView"));
                            if (!g_crashed) RESULT(r);

                            // C-9. getSize (before attach)
                            STEP("C-9. plugView->getSize()");
                            ViewRect viewSize{};
                            PROTECTED_CALL(r = plugView->getSize(&viewSize));
                            if (!g_crashed) {
                                RESULT(r);
                                if (r == kResultOk)
                                    LOGI("View size: "
                                         << viewSize.right - viewSize.left << "x"
                                         << viewSize.bottom - viewSize.top);
                            }

                            // C-10. setFrame
                            static MinimalPlugFrame plugFrame;
                            STEP("C-10. plugView->setFrame(plugFrame)");
                            PROTECTED_CALL(r = plugView->setFrame(&plugFrame));
                            if (!g_crashed) RESULT(r);

                            // C-11. Create a real NSWindow + NSView to host the plugin UI.
                            // Many plugin UIs (especially web-based ones) require a real
                            // NSView with a valid window or they crash / misbehave.
                            STEP("C-11. Create host NSWindow and NSView");
                            int viewW = viewSize.right  - viewSize.left;
                            int viewH = viewSize.bottom - viewSize.top;
                            if (viewW <= 0) viewW = 800;
                            if (viewH <= 0) viewH = 600;
                            void* hostView = nullptr;
                            PROTECTED_CALL(hostView = hostview_create(viewW, viewH));
                            if (!g_crashed)
                                LOGI("NSWindow+NSView created, contentView: " << hostView);

                            // C-12. plugView->attached(nsView, "NSView")
                            // THIS IS THE CALL THAT CRASHES IN DAWS
                            STEP("C-12. plugView->attached(nsView, \"NSView\")");
                            LOGI("  This is the call DAWs make to embed the plugin UI.");
                            LOGI("  A crash here means the plugin UI init is the problem.");
                            PROTECTED_CALL(r = plugView->attached(hostView, "NSView"));
                            if (!g_crashed) {
                                RESULT(r);
                                if (r == kResultOk) {
                                    // C-13. getSize after attach
                                    STEP("C-13. plugView->getSize() after attach");
                                    ViewRect sizeAfter{};
                                    PROTECTED_CALL(r = plugView->getSize(&sizeAfter));
                                    if (!g_crashed && r == kResultOk)
                                        LOGI("View size after attach: "
                                             << sizeAfter.right - sizeAfter.left << "x"
                                             << sizeAfter.bottom - sizeAfter.top);

                                    // C-14. Show the window and run the event loop briefly
                                    STEP("C-14. Show window and run event loop briefly (2s)");
                                    LOGI("  Showing plugin window for 2 seconds...");
                                    PROTECTED_CALL(hostview_show_and_run(2.0));

                                    // C-15. plugView->removed()
                                    STEP("C-15. plugView->removed()");
                                    PROTECTED_CALL(r = plugView->removed());
                                    if (!g_crashed) RESULT(r);
                                }
                            }

                            if (!g_crashed) hostview_destroy();
                            {
                                uint32 rc = 0;
                                PROTECTED_CALL(rc = plugView->release());
                                if (!g_crashed) LOGI("plugView->release() refcount: " << rc);
                            }
                        }

                        // C-16b/C-16c. Disconnect IConnectionPoint peers (mirror of 4d/4e).
                        if (!g_crashed && compCP && ctrlCP) {
                            STEP("C-16b. compCP->disconnect(controller)");
                            PROTECTED_CALL(r = compCP->disconnect(
                                static_cast<FUnknown*>(controller)));
                            if (!g_crashed) RESULT(r);

                            STEP("C-16c. ctrlCP->disconnect(component)");
                            PROTECTED_CALL(r = ctrlCP->disconnect(
                                static_cast<FUnknown*>(component)));
                            if (!g_crashed) RESULT(r);
                        }
                        if (compCP) { compCP->release(); compCP = nullptr; }
                        if (ctrlCP) { ctrlCP->release(); ctrlCP = nullptr; }

                        // C-17. controller->terminate()
                        STEP("C-17. controller->terminate()");
                        PROTECTED_CALL(r = controller->terminate());
                        if (!g_crashed) RESULT(r);

                        // C-18. controller->release()
                        STEP("C-18. controller->release()");
                        {
                            uint32 rc = 0;
                            PROTECTED_CALL(rc = controller->release());
                            if (!g_crashed) LOGI("controller->release() refcount: " << rc);
                        }
                    } else if (!g_crashed) {
                        LOGI("createInstance(controllerCid) failed or returned nullptr -- skipping editor path");
                    }
                    LOGI("--- IEditController / IPlugView path done ---");
                } else if (skipEditor) {
                    LOGI("--- IEditController / IPlugView path SKIPPED (--skip-editor) ---");
                }

                // Probe component directly for IPlugView, IEditController, IEditController2.
                // Some non-standard plugins expose the view on the component itself.
                STEP("12b. component->queryInterface(IPlugView) [non-standard]");
                {
                    IPlugView* compView = nullptr;
                    PROTECTED_CALL(r = component->queryInterface(
                        IID_IPlugView, reinterpret_cast<void**>(&compView)));
                    if (!g_crashed) {
                        RESULT(r);
                        if (r == kResultOk && compView) {
                            LOGI("  Component exposes IPlugView directly! ptr=" << (void*)compView);
                            LOGI("  This is the non-standard path -- view->attached() may crash here.");
                            // isPlatformTypeSupported
                            STEP("12b-i. compView->isPlatformTypeSupported(\"NSView\")");
                            PROTECTED_CALL(r = compView->isPlatformTypeSupported("NSView"));
                            if (!g_crashed) RESULT(r);
                            // getSize
                            STEP("12b-ii. compView->getSize()");
                            ViewRect vsz{};
                            PROTECTED_CALL(r = compView->getSize(&vsz));
                            if (!g_crashed && r == kResultOk)
                                LOGI("  Size: " << vsz.right-vsz.left << "x" << vsz.bottom-vsz.top);
                            compView->release();
                        } else {
                            LOGI("  Component does NOT expose IPlugView (expected for standard plugins)");
                        }
                    }
                }

                STEP("12c. component->queryInterface(IEditController) [single-component]");
                {
                    IEditController* compCtrl = nullptr;
                    PROTECTED_CALL(r = component->queryInterface(
                        IID_IEditController, reinterpret_cast<void**>(&compCtrl)));
                    if (!g_crashed) {
                        RESULT(r);
                        if (r == kResultOk && compCtrl) {
                            LOGI("  Component IS also an IEditController (single-component plugin)!");
                            LOGI("  paramCount=" << compCtrl->getParameterCount());
                            IPlugView* sv = nullptr;
                            PROTECTED_CALL(sv = compCtrl->createView("editor"));
                            if (!g_crashed)
                                LOGI("  createView(\"editor\") from component-as-controller: " << (void*)sv);
                            if (sv) sv->release();
                            compCtrl->release();
                        } else {
                            LOGI("  Component does NOT implement IEditController directly");
                        }
                    }
                }

                STEP("12d. controller->queryInterface(IEditController2)");
                // IEditController2 advertises setKnobMode / openHelp / openAboutBox
                // If present, the plugin has a help/about path which may also reveal UI details.
                {
                    // We need to re-create the controller just for this QI probe since
                    // we already released it above. Skip if we never had a valid CID.
                    if (hasCtrlCid) {
                        void* rawCtrl2 = nullptr;
                        PROTECTED_CALL(r = factory->createInstance(
                            ctrlCid, IID_IEditController, &rawCtrl2));
                        if (!g_crashed && r == kResultOk && rawCtrl2) {
                            IEditController* ctrl2 = static_cast<IEditController*>(rawCtrl2);
                            IEditController2* ec2 = nullptr;
                            PROTECTED_CALL(r = ctrl2->queryInterface(
                                IID_IEditController2, reinterpret_cast<void**>(&ec2)));
                            if (!g_crashed) {
                                RESULT(r);
                                if (r == kResultOk && ec2) {
                                    LOGI("  IEditController2 supported");
                                    ec2->release();
                                } else {
                                    LOGI("  IEditController2 NOT supported");
                                }
                            }
                            ctrl2->release();
                        }
                    }
                }

                STEP("13. component->setIoMode(kSimple)");
                PROTECTED_CALL(r = component->setIoMode(kSimple));
                if (!g_crashed) RESULT(r);

                // 14-17 -- bus counts
                int32 audioIns = 0, audioOuts = 0, eventIns = 0, eventOuts = 0;

                STEP("14. component->getBusCount(kAudio, kInput)");
                PROTECTED_CALL(audioIns = component->getBusCount(kAudio, kInput));
                if (!g_crashed) LOGI("Audio input buses  : " << audioIns);

                STEP("15. component->getBusCount(kAudio, kOutput)");
                PROTECTED_CALL(audioOuts = component->getBusCount(kAudio, kOutput));
                if (!g_crashed) LOGI("Audio output buses : " << audioOuts);

                STEP("16. component->getBusCount(kEvent, kInput)");
                PROTECTED_CALL(eventIns = component->getBusCount(kEvent, kInput));
                if (!g_crashed) LOGI("Event input buses  : " << eventIns);

                STEP("17. component->getBusCount(kEvent, kOutput)");
                PROTECTED_CALL(eventOuts = component->getBusCount(kEvent, kOutput));
                if (!g_crashed) LOGI("Event output buses : " << eventOuts);
                (void)eventIns; (void)eventOuts;

                for (int32 i = 0; i < audioIns && !g_crashed; ++i) {
                    std::ostringstream ss;
                    ss << "17a." << i << ". getBusInfo(kAudio,kInput," << i << ")";
                    STEP(ss.str().c_str());
                    BusInfo bi{};
                    PROTECTED_CALL(r = component->getBusInfo(kAudio, kInput, i, bi));
                    if (!g_crashed && r == kResultOk)
                        LOGI("  \"" << bi.name << "\"  ch=" << bi.channelCount);
                }
                for (int32 i = 0; i < audioOuts && !g_crashed; ++i) {
                    std::ostringstream ss;
                    ss << "17b." << i << ". getBusInfo(kAudio,kOutput," << i << ")";
                    STEP(ss.str().c_str());
                    BusInfo bi{};
                    PROTECTED_CALL(r = component->getBusInfo(kAudio, kOutput, i, bi));
                    if (!g_crashed && r == kResultOk)
                        LOGI("  \"" << bi.name << "\"  ch=" << bi.channelCount);
                }

                // 18 -- queryInterface(IAudioProcessor)
                STEP("18. component->queryInterface(IAudioProcessor)");
                {
                    void* rawProc = nullptr;
                    PROTECTED_CALL(r = component->queryInterface(IID_IAudioProcessor, &rawProc));
                    if (!g_crashed) {
                        RESULT(r);
                        if (r == kResultOk && rawProc) {
                            IAudioProcessor* proc = static_cast<IAudioProcessor*>(rawProc);
                            LOGI("IAudioProcessor: " << proc);

                            STEP("19. processor->canProcessSampleSize(kSample32)");
                            PROTECTED_CALL(r = proc->canProcessSampleSize(kSample32));
                            if (!g_crashed) RESULT(r);

                            STEP("20. processor->canProcessSampleSize(kSample64)");
                            PROTECTED_CALL(r = proc->canProcessSampleSize(kSample64));
                            if (!g_crashed) RESULT(r);

                            STEP("21. processor->setupProcessing(44100, 512, kSample32)");
                            {
                                ProcessSetup ps{};
                                ps.processMode        = kRealtime;
                                ps.symbolicSampleSize = kSample32;
                                ps.maxSamplesPerBlock = 512;
                                ps.sampleRate         = 44100.0;
                                PROTECTED_CALL(r = proc->setupProcessing(ps));
                                if (!g_crashed) RESULT(r);
                            }

                            STEP("22. processor->setBusArrangements(stereo)");
                            {
                                SpeakerArrangement stereo = 0x3; // L|R
                                SpeakerArrangement* ins  = audioIns  > 0 ? &stereo : nullptr;
                                SpeakerArrangement* outs = audioOuts > 0 ? &stereo : nullptr;
                                PROTECTED_CALL(r = proc->setBusArrangements(
                                    ins,  audioIns  > 0 ? 1 : 0,
                                    outs, audioOuts > 0 ? 1 : 0));
                                if (!g_crashed) RESULT(r);
                            }

                            STEP("23. processor->getLatencySamples()");
                            {
                                uint32 lat = 0;
                                PROTECTED_CALL(lat = proc->getLatencySamples());
                                if (!g_crashed) LOGI("Latency: " << lat << " samples");
                            }

                            STEP("24. processor->getTailSamples()");
                            {
                                uint32 tail = 0;
                                PROTECTED_CALL(tail = proc->getTailSamples());
                                if (!g_crashed) LOGI("Tail: " << tail << " samples");
                            }
                        } else if (!g_crashed) {
                            LOGI("IAudioProcessor not available (normal for controller-only)");
                        }
                    }
                }

                // 25 -- setActive
                STEP("25. component->setActive(true)");
                PROTECTED_CALL(r = component->setActive(true));
                if (!g_crashed) RESULT(r);

                // Cleanup
                STEP("26. component->setActive(false)");
                PROTECTED_CALL(r = component->setActive(false));
                if (!g_crashed) RESULT(r);

                STEP("27. component->terminate()");
                PROTECTED_CALL(r = component->terminate());
                if (!g_crashed) RESULT(r);
            }

            if (!g_crashed && component) {
                STEP("28. component->release()");
                uint32 rc = 0;
                PROTECTED_CALL(rc = component->release());
                if (!g_crashed) LOGI("release() refcount: " << rc);
            }
        }
    }

    // 29 -- optional bundleExit
    if (!g_crashed) {
        STEP("29. Look up optional bundleExit symbol");
        typedef bool (*BundleExitFn)();
        BundleExitFn bundleExit = nullptr;
        PROTECTED_CALL(bundleExit = (BundleExitFn)dlsym(handle, "bundleExit"));
        if (!g_crashed && bundleExit) {
            bool ok = false;
            PROTECTED_CALL(ok = bundleExit());
            if (!g_crashed) LOGI("bundleExit => " << (ok ? "true" : "false"));
        } else if (!g_crashed) {
            LOGI("bundleExit not present (optional)");
        }
    }

    // 30 -- dlclose (optional, with optional delay)
    if (skipDlclose) {
        STEP("30. dlclose() -- SKIPPED (--skip-dlclose)");
        LOGI("Skipping dlclose. Background threads (if any) may still be running.");
        LOGI("Calling _exit(0) to terminate cleanly without running atexit handlers.");
        std::cout <<
            "\n+==============================================+\n"
            "|  Initialization sequence COMPLETE            |\n"
            "|  (dlclose skipped -- use --dlclose-delay N   |\n"
            "|   to drain background threads before unload) |\n"
            "+==============================================+\n";
        _exit(0);
    }

    if (dlcloseDelay > 0) {
        std::ostringstream ss;
        ss << "30. Waiting " << dlcloseDelay
           << "s for background threads before dlclose...";
        STEP(ss.str().c_str());
        LOGI("Sleeping " << dlcloseDelay << " second(s) -- watching for late output...");
        std::cout.flush();
        sleep(static_cast<unsigned>(dlcloseDelay));
        LOGI("Sleep done.");
    }

    STEP("30. dlclose()");
    LOGI("Note: if a crash follows here, a background thread spawned by the plug-in");
    LOGI("is still running after terminate()/release(). This is a plug-in bug.");
    LOGI("Try --dlclose-delay 5 or --skip-dlclose to confirm.");
    if (handle) dlclose(handle);
    if (!g_crashed) LOGI("dlclose done");

    if (g_crashed) {
        std::cerr <<
            "\n+==============================================+\n"
            "|  SEQUENCE ABORTED - CRASH DURING:            |\n"
            "|  " << g_currentStep << "\n"
            "|                                              |\n"
            "|  LIKELY CAUSE: a background thread spawned   |\n"
            "|  by the plug-in is still running when the    |\n"
            "|  binary is unloaded by dlclose(). This is a  |\n"
            "|  plug-in bug -- it must join/stop all its    |\n"
            "|  threads before terminate() returns.         |\n"
            "|                                              |\n"
            "|  Run with --skip-dlclose to confirm the      |\n"
            "|  VST3 lifecycle itself is fine.              |\n"
            "+==============================================+\n";
        return 1;
    }

    std::cout <<
        "\n+==============================================+\n"
        "|  Initialization sequence COMPLETE            |\n"
        "+==============================================+\n";
    return 0;
}
