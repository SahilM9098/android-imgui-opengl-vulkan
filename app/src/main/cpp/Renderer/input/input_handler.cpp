#include "input_handler.hpp"
#include "log_config.hpp"
#include "renderer.hpp"

#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <mutex>

#include "dobby.h"

#define LOG_TAG "InputHandler"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_INPUT_HANDLER, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_INPUT_HANDLER, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace Renderer {
namespace Input {

    using AInputQueue_getEvent_t = int32_t (*)(AInputQueue*, AInputEvent**);
    using AInputQueue_finishEvent_t = void (*)(AInputQueue*, AInputEvent*, int);
    using AInputEvent_fromJava_t = AInputEvent* (*)(JNIEnv*, jobject);
    using InputConsumer_consume_t = int32_t (*)(void*, void*, bool, int64_t, uint32_t*, void**);
    using InputConsumer_sendFinishedSignal_t = int32_t (*)(void*, uint32_t, bool);

    static AInputQueue_getEvent_t orig_AInputQueue_getEvent = nullptr;
    static AInputQueue_finishEvent_t orig_AInputQueue_finishEvent = nullptr;
    static AInputEvent_fromJava_t orig_AInputEvent_fromJava = nullptr;
    static InputConsumer_consume_t orig_InputConsumer_consume = nullptr;
    static InputConsumer_sendFinishedSignal_t orig_InputConsumer_sendFinishedSignal = nullptr;
    static std::atomic<int> g_InputLogCount{0};
    static std::atomic<int> g_ConsumedLogCount{0};
    static std::mutex g_MotionDedupeMutex;

    static constexpr int32_t MOTION_ACTION_MASK = 0xff;
    static constexpr int32_t MOTION_POINTER_INDEX_MASK = 0xff00;
    static constexpr int32_t MOTION_POINTER_INDEX_SHIFT = 8;
    static constexpr int32_t MOTION_ACTION_DOWN = 0;
    static constexpr int32_t MOTION_ACTION_UP = 1;
    static constexpr int32_t MOTION_ACTION_MOVE = 2;
    static constexpr int32_t MOTION_ACTION_CANCEL = 3;
    static constexpr int32_t MOTION_ACTION_POINTER_DOWN = 5;
    static constexpr int32_t MOTION_ACTION_POINTER_UP = 6;
    static constexpr int32_t MOTION_ACTION_HOVER_MOVE = 7;

    static bool IsDuplicateMotionEvent(int32_t action, int32_t pointerIndex, int64_t eventTime,
                                       float x, float y, bool consume,
                                       bool* duplicateConsume) {
        static int32_t lastAction = -1;
        static int32_t lastPointerIndex = -1;
        static int64_t lastEventTime = -1;
        static float lastX = -100000.0f;
        static float lastY = -100000.0f;
        static bool lastConsume = false;

        std::lock_guard<std::mutex> lock(g_MotionDedupeMutex);
        bool duplicate =
            lastAction == action &&
            lastPointerIndex == pointerIndex &&
            lastEventTime == eventTime &&
            std::fabs(lastX - x) < 0.5f &&
            std::fabs(lastY - y) < 0.5f;

        if (duplicate) {
            if (duplicateConsume)
                *duplicateConsume = lastConsume;
            return true;
        }

        lastAction = action;
        lastPointerIndex = pointerIndex;
        lastEventTime = eventTime;
        lastX = x;
        lastY = y;
        lastConsume = consume;
        return false;
    }

    static bool QueueMotionEvent(const AInputEvent* event, const char* source) {
        if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
            return false;

        int32_t rawAction = AMotionEvent_getAction(event);
        int32_t pointerIndex = (rawAction & MOTION_POINTER_INDEX_MASK) >> MOTION_POINTER_INDEX_SHIFT;
        int32_t action = rawAction & MOTION_ACTION_MASK;

        if (pointerIndex < 0 || (size_t)pointerIndex >= AMotionEvent_getPointerCount(event))
            pointerIndex = 0;

        float x = AMotionEvent_getX(event, pointerIndex);
        float y = AMotionEvent_getY(event, pointerIndex);
        int64_t eventTime = AMotionEvent_getEventTime(event);
        int touchAction = -1;
        float touchX = x;
        float touchY = y;

        switch (action) {
            case MOTION_ACTION_DOWN:
            case MOTION_ACTION_POINTER_DOWN:
                touchAction = 0;
                break;
            case MOTION_ACTION_UP:
            case MOTION_ACTION_CANCEL:
            case MOTION_ACTION_POINTER_UP:
                touchAction = 1;
                break;
            case MOTION_ACTION_MOVE:
            case MOTION_ACTION_HOVER_MOVE:
                touchAction = 2;
                touchX = AMotionEvent_getX(event, 0);
                touchY = AMotionEvent_getY(event, 0);
                break;
            default:
                return false;
        }

        bool consume = Renderer::ShouldConsumeTouch(touchAction, touchX, touchY);

        bool duplicateConsume = false;
        if (IsDuplicateMotionEvent(action, pointerIndex, eventTime, x, y,
                                   consume, &duplicateConsume)) {
            return duplicateConsume;
        }

        Renderer::HandleTouch(touchAction, touchX, touchY);

        int logCount = g_InputLogCount.fetch_add(1);
        if (logCount < 12) {
            LOGI("%s input action=%d pointer=%d x=%.1f y=%.1f",
                 source, action, pointerIndex, x, y);
        }

        if (consume) {
            int consumedLogCount = g_ConsumedLogCount.fetch_add(1);
            if (consumedLogCount < 12) {
                LOGI("%s consumed by ImGui action=%d pointer=%d x=%.1f y=%.1f",
                     source, action, pointerIndex, x, y);
            }
        }

        return consume;
    }

    static int32_t hook_AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent) {
        int32_t result = orig_AInputQueue_getEvent(queue, outEvent);
        if (result >= 0 && outEvent && *outEvent && QueueMotionEvent(*outEvent, "AInputQueue")) {
            if (orig_AInputQueue_finishEvent)
                orig_AInputQueue_finishEvent(queue, *outEvent, 1);
            *outEvent = nullptr;
            return -EWOULDBLOCK;
        }
        return result;
    }

    static AInputEvent* hook_AInputEvent_fromJava(JNIEnv* env, jobject inputEvent) {
        AInputEvent* event = orig_AInputEvent_fromJava(env, inputEvent);
        QueueMotionEvent(event, "AInputEvent_fromJava");
        return event;
    }

    static int32_t hook_InputConsumer_consume(void* self, void* factory, bool consumeBatches,
                                              int64_t frameTime, uint32_t* outSeq,
                                              void** outEvent) {
        int32_t result = orig_InputConsumer_consume(self, factory, consumeBatches,
                                                    frameTime, outSeq, outEvent);
        if (result == 0 && outEvent && *outEvent &&
            QueueMotionEvent((const AInputEvent*)*outEvent, "libinput")) {
            if (orig_InputConsumer_sendFinishedSignal && outSeq)
                orig_InputConsumer_sendFinishedSignal(self, *outSeq, true);
            *outEvent = nullptr;
            return -EWOULDBLOCK;
        }
        return result;
    }

    static bool InstallLibInputHook() {
        if (orig_InputConsumer_consume)
            return true;

        void* libinput = dlopen("libinput.so", RTLD_NOW | RTLD_NOLOAD);
        if (!libinput)
            libinput = dlopen("libinput.so", RTLD_NOW);

        const char* consumeSymbol =
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";

        void* consume = nullptr;
        if (libinput)
            consume = dlsym(libinput, consumeSymbol);
        if (!consume)
            consume = DobbySymbolResolver("libinput.so", consumeSymbol);

        if (!consume) {
            LOGE("Failed to resolve libinput InputConsumer::consume for app-process touch fallback");
            return false;
        }

        if (DobbyHook(consume, (void*)hook_InputConsumer_consume,
                      (void**)&orig_InputConsumer_consume) != 0) {
            LOGE("Failed to hook libinput InputConsumer::consume at %p", consume);
            return false;
        }

        const char* sendFinishedSymbol = "_ZN7android13InputConsumer18sendFinishedSignalEjb";
        void* sendFinished = nullptr;
        if (libinput)
            sendFinished = dlsym(libinput, sendFinishedSymbol);
        if (!sendFinished)
            sendFinished = DobbySymbolResolver("libinput.so", sendFinishedSymbol);
        if (sendFinished)
            orig_InputConsumer_sendFinishedSignal = (InputConsumer_sendFinishedSignal_t)sendFinished;
        else
            LOGE("Failed to resolve libinput InputConsumer::sendFinishedSignal; app-process consumed input may not finish cleanly");

        LOGI("libinput InputConsumer::consume hooked for app-process touch fallback");
        return true;
    }

    bool Init() {
        LOGI("Installing input hooks (auto-detect strategy)...");

        bool anyHooked = false;
        void* libandroid = dlopen("libandroid.so", RTLD_NOW);
        if (!libandroid) {
            LOGE("Failed to open libandroid.so for input hook: %s", dlerror());
            return InstallLibInputHook();
        }

        void* getEvent = dlsym(libandroid, "AInputQueue_getEvent");
        orig_AInputQueue_finishEvent =
            (AInputQueue_finishEvent_t)dlsym(libandroid, "AInputQueue_finishEvent");
        if (!orig_AInputQueue_finishEvent)
            LOGE("Failed to resolve AInputQueue_finishEvent; native consumed input may not finish cleanly");

        if (getEvent) {
            if (DobbyHook(getEvent, (void*)hook_AInputQueue_getEvent,
                          (void**)&orig_AInputQueue_getEvent) == 0) {
                LOGI("AInputQueue_getEvent hooked for ImGui touch input");
                anyHooked = true;
            } else {
                LOGE("Failed to hook AInputQueue_getEvent at %p", getEvent);
            }
        } else {
            LOGE("Failed to resolve AInputQueue_getEvent");
        }

        void* fromJava = dlsym(libandroid, "AInputEvent_fromJava");
        if (fromJava) {
            if (DobbyHook(fromJava, (void*)hook_AInputEvent_fromJava,
                          (void**)&orig_AInputEvent_fromJava) == 0) {
                LOGI("AInputEvent_fromJava hooked for Java MotionEvent fallback");
                anyHooked = true;
            } else {
                LOGE("Failed to hook AInputEvent_fromJava at %p", fromJava);
            }
        }

        anyHooked = InstallLibInputHook() || anyHooked;
        return anyHooked;
    }

} // namespace Input
} // namespace Renderer
