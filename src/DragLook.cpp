#include "DragLook.h"

#include <RE/B/BSInputEventUser.h>

#include <mutex>

#include "Dialogue.h"
#include "VtableHook.h"

namespace DragLook {
    namespace {
        namespace ID {
            constexpr REL::ID MenuVtable{460296};
            constexpr REL::ID ProcessMessage{113830};
            constexpr REL::ID MouseEventAt{130634};
            constexpr REL::ID InputUserVtable{460298};
            constexpr REL::ID ShouldHandleEvent{91901};
            constexpr REL::ID MovieVtable{303817};  // Scaleform::GFx::MovieImpl
            constexpr REL::ID GetViewport{163061};
            constexpr REL::ID HitTest{163073};
            constexpr REL::ID MessageQueue{937897};
            constexpr REL::ID AddMessage{130660};
            constexpr REL::ID MenuCursor{937972};
        }

        constexpr std::size_t kProcessMessageSlot = 8;
        constexpr std::size_t kMouseEventAtSlot = 25;
        constexpr std::size_t kShouldHandleEventSlot = 1;
        constexpr std::size_t kGetViewportSlot = 13;
        constexpr std::size_t kHitTestSlot = 38;

        constexpr std::size_t kInputUserOffset = 0x10;
        constexpr std::size_t kMovieOffset = 0x88;
        constexpr std::size_t kMenuFlagsOffset = 0xC0;
        constexpr std::uint32_t kUsesCursor = 1u << 3;
        constexpr std::size_t kMessageTypeOffset = 0x8;
        constexpr std::size_t kMessageDataOffset = 0x10;
        constexpr std::uint32_t kHideMenuMessage = 2;
        constexpr std::uint32_t kUserMessage = 7;
        constexpr std::int32_t kLeftButton = 0;
        constexpr std::uint32_t kMouseDown = 2;
        constexpr std::uint32_t kHitTestShapesNoInvisible = 3;
        constexpr std::size_t kCursorWarpOffset = 0x94;
        constexpr std::uint32_t kMaxSettleFrames = 10;

        struct MouseEvent {
            std::uint32_t type;
            std::uint32_t modifiers;
            float x;
            float y;
            float scrollDelta;
            std::uint32_t button;
            std::uint32_t mouseIndex;
        };

        struct Viewport {
            std::int32_t bufferWidth;
            std::int32_t bufferHeight;
            std::int32_t left;
            std::int32_t top;
            std::byte rest[0x24];
        };
        static_assert(sizeof(Viewport) == 0x34);

        using ProcessMessage_t = std::uint32_t (*)(void*, const std::byte*);
        using MouseEventAt_t = void (*)(void*, MouseEvent*, std::uint32_t, std::uint32_t);
        using ShouldHandleEvent_t = bool (*)(void*, RE::InputEvent*);
        using GetViewport_t = void (*)(const void*, Viewport*);
        using HitTest_t = bool (*)(void*, float, float, std::uint32_t, std::uint32_t);
        using AddMessage_t = void (*)(void*, const RE::BSFixedString*, std::uint32_t, const RE::BSFixedString*);

        ProcessMessage_t g_processMessage{nullptr};
        ShouldHandleEvent_t g_shouldHandleEvent{nullptr};
        MouseEventAt_t g_mouseEventAt{nullptr};
        GetViewport_t g_getViewport{nullptr};
        HitTest_t g_hitTest{nullptr};
        AddMessage_t g_addMessage{nullptr};
        std::uintptr_t g_movieVtable{0};
        void* const* g_messageQueue{nullptr};
        const std::byte* const* g_menuCursor{nullptr};
        bool g_debug{false};

        // Never destroyed
        RE::BSFixedString& g_look = *new RE::BSFixedString;
        RE::BSFixedString& g_menuName = *new RE::BSFixedString;
        RE::BSFixedString& g_showCursor = *new RE::BSFixedString;
        RE::BSFixedString& g_hideCursor = *new RE::BSFixedString;

        std::atomic<bool> g_armed{false};
        std::atomic<bool> g_dragging{false};

        // After a drag, until the game has moved the OS cursor back
        enum class Settle : std::uint8_t { kNone, kAwaitWarp, kWarping };
        std::atomic<Settle> g_settle{Settle::kNone};
        std::atomic<std::uint32_t> g_settleFrames{0};

        // Cursor state the game asked for while a drag hides it
        std::mutex g_lock;
        bool g_wantCursor{false};
        bool g_ownHidePending{false};

        [[nodiscard]] const void* EntryOf(const RE::BSFixedString& a_string) noexcept {
            return *reinterpret_cast<const void* const*>(&a_string);
        }

        void Send(const RE::BSFixedString& a_message) {
            if (const auto queue = *g_messageQueue) {
                g_addMessage(queue, &g_menuName, kUserMessage, &a_message);
            }
        }

        [[nodiscard]] bool CursorShown(const std::byte* a_menu) noexcept {
            return (*reinterpret_cast<const std::uint32_t*>(a_menu + kMenuFlagsOffset) & kUsesCursor) != 0;
        }

        [[nodiscard]] bool OverMenu(std::byte* a_menu) {
            const auto movie = *reinterpret_cast<void**>(a_menu + kMovieOffset);
            if (!movie || *reinterpret_cast<const std::uintptr_t*>(movie) != g_movieVtable) {
                return true;
            }

            // Same coordinates the menu hands to Scaleform for its own clicks
            MouseEvent event{};
            g_mouseEventAt(a_menu, &event, kMouseDown, kLeftButton);
            Viewport viewport{};
            g_getViewport(movie, &viewport);
            return g_hitTest(movie, event.x - static_cast<float>(viewport.left),
                             event.y - static_cast<float>(viewport.top), kHitTestShapesNoInvisible, 0);
        }

        void StartDrag() {
            {
                std::scoped_lock lock{g_lock};
                g_wantCursor = true;
                g_ownHidePending = true;
                g_dragging.store(true, std::memory_order_release);
            }
            Send(g_hideCursor);
            if (g_debug) {
                REX::INFO("DragLook: start");
            }
        }

        void EndDrag() {
            bool show{};
            {
                std::scoped_lock lock{g_lock};
                if (!g_dragging.exchange(false, std::memory_order_acq_rel)) {
                    return;
                }
                show = g_wantCursor;
            }
            if (show) {
                g_settleFrames.store(0, std::memory_order_relaxed);
                g_settle.store(Settle::kAwaitWarp, std::memory_order_release);
                Send(g_showCursor);
            }
            if (g_debug) {
                REX::INFO("DragLook: end, cursor {}", show ? "shown" : "left hidden");
            }
        }

        void ResetDrag() {
            {
                std::scoped_lock lock{g_lock};
                g_dragging.store(false, std::memory_order_release);
                g_ownHidePending = false;
            }
            g_settle.store(Settle::kNone, std::memory_order_release);
        }

        // True when the event belongs to the drag and must not reach the menu.
        [[nodiscard]] bool OnLeftButton(std::byte* a_menu, const RE::ButtonEvent& a_event) {
            const bool pressed = a_event.value != 0.0f && a_event.heldDownSecs == 0.0f;
            if (g_dragging.load(std::memory_order_acquire)) {
                if (!pressed) {
                    if (a_event.value == 0.0f) {
                        EndDrag();
                    }
                    return true;
                }
                // Release lost, e.g. to another menu
                EndDrag();
            }

            if (!pressed || !CursorShown(a_menu) || OverMenu(a_menu)) {
                return false;
            }
            StartDrag();
            return true;
        }

        [[nodiscard]] bool IsLeftButton(const RE::InputEvent& a_event) noexcept {
            return a_event.eventType == RE::InputEvent::EventType::kButton &&
                   a_event.deviceType == RE::InputEvent::DeviceType::kMouse &&
                   static_cast<const RE::IDEvent&>(a_event).idCode == kLeftButton;
        }

        // UI input processing, under the lock the game holds for Scaleform input
        bool OnShouldHandleEvent(void* a_user, RE::InputEvent* a_event) {
            if (g_armed.load(std::memory_order_acquire) && a_event && IsLeftButton(*a_event) &&
                !Dialogue::IsCameraEnabled()) {
                const auto menu = static_cast<std::byte*>(a_user) - kInputUserOffset;
                if (OnLeftButton(menu, static_cast<const RE::ButtonEvent&>(*a_event))) {
                    a_event->status = RE::InputEvent::Status::kStop;
                    return false;
                }
            }
            return g_shouldHandleEvent(a_user, a_event);
        }

        [[nodiscard]] bool HoldCursorMessage(const std::byte* a_message) {
            const auto type = *reinterpret_cast<const std::uint32_t*>(a_message + kMessageTypeOffset);
            if (type == kHideMenuMessage) {
                ResetDrag();
                return false;
            }
            if (type != kUserMessage) {
                return false;
            }
            const auto data = *reinterpret_cast<const void* const*>(a_message + kMessageDataOffset);

            std::scoped_lock lock{g_lock};
            if (!g_dragging.load(std::memory_order_relaxed)) {
                return false;
            }
            if (data == EntryOf(g_showCursor)) {
                g_wantCursor = true;
                return true;
            }
            if (data == EntryOf(g_hideCursor)) {
                if (g_ownHidePending) {
                    g_ownHidePending = false;
                } else {
                    g_wantCursor = false;
                }
            }
            return false;
        }

        // The game shows the cursor with the reply list; a drag keeps it hidden.
        std::uint32_t OnProcessMessage(void* a_menu, const std::byte* a_message) {
            if (g_armed.load(std::memory_order_acquire) && a_message && HoldCursorMessage(a_message)) {
                return 0;
            }
            return g_processMessage(a_menu, a_message);
        }

        [[nodiscard]] bool CursorWarpPending() noexcept {
            const auto cursor = *g_menuCursor;
            return cursor && *reinterpret_cast<const volatile std::uint8_t*>(cursor + kCursorWarpOffset) != 0;
        }

        // ShowCursor restores the cursor, then the game warps the OS cursor on a later frame.
        [[nodiscard]] bool Settling() {
            const auto state = g_settle.load(std::memory_order_acquire);
            if (state == Settle::kNone) {
                return false;
            }
            const bool warp = CursorWarpPending();
            if (g_settleFrames.fetch_add(1, std::memory_order_relaxed) >= kMaxSettleFrames ||
                (state == Settle::kWarping && !warp)) {
                g_settle.store(Settle::kNone, std::memory_order_release);
            } else if (state == Settle::kAwaitWarp && warp) {
                g_settle.store(Settle::kWarping, std::memory_order_release);
            }
            return true;
        }

        [[nodiscard]] bool VtableSlotIs(REL::ID a_vtable, std::size_t a_slot, REL::ID a_expected) {
            const auto vtable = reinterpret_cast<const std::uintptr_t*>(a_vtable.address());
            return vtable[a_slot] == a_expected.address();
        }
    }

    bool Install(const Settings& a_settings) {
        if (!a_settings.dragRotation) {
            return true;
        }

        if (!VtableSlotIs(ID::MenuVtable, kMouseEventAtSlot, ID::MouseEventAt) ||
            !VtableSlotIs(ID::MovieVtable, kGetViewportSlot, ID::GetViewport) ||
            !VtableSlotIs(ID::MovieVtable, kHitTestSlot, ID::HitTest)) {
            REX::ERROR("DialogueMenu or Scaleform not recognized, drag rotation disabled");
            return false;
        }

        g_mouseEventAt = reinterpret_cast<MouseEventAt_t>(ID::MouseEventAt.address());
        g_getViewport = reinterpret_cast<GetViewport_t>(ID::GetViewport.address());
        g_hitTest = reinterpret_cast<HitTest_t>(ID::HitTest.address());
        g_addMessage = reinterpret_cast<AddMessage_t>(ID::AddMessage.address());
        g_movieVtable = ID::MovieVtable.address();
        g_messageQueue = reinterpret_cast<void* const*>(ID::MessageQueue.address());
        g_menuCursor = reinterpret_cast<const std::byte* const*>(ID::MenuCursor.address());
        g_debug = a_settings.debugLog;

        g_processMessage = VtableHook::Install<ProcessMessage_t>(ID::MenuVtable, kProcessMessageSlot,
                                                                 ID::ProcessMessage, &OnProcessMessage);
        if (!g_processMessage) {
            REX::ERROR("DialogueMenu messages not recognized, drag rotation disabled");
            return false;
        }
        g_shouldHandleEvent = VtableHook::Install<ShouldHandleEvent_t>(ID::InputUserVtable, kShouldHandleEventSlot,
                                                                       ID::ShouldHandleEvent, &OnShouldHandleEvent);
        if (!g_shouldHandleEvent) {
            REX::ERROR("DialogueMenu input not recognized, drag rotation disabled");
            return false;
        }
        return true;
    }

    void Arm(bool a_armed) {
        if (!g_shouldHandleEvent) {
            return;
        }
        if (a_armed && g_look.empty()) {
            g_look = RE::BSFixedString{"Look"};
            g_menuName = RE::BSFixedString{"DialogueMenu"};
            g_showCursor = RE::BSFixedString{"ShowCursor"};
            g_hideCursor = RE::BSFixedString{"HideCursor"};
        }
        ResetDrag();
        g_armed.store(a_armed, std::memory_order_release);
    }

    void Cancel() {
        if (g_dragging.load(std::memory_order_acquire)) {
            EndDrag();
        }
    }

    bool Update(RE::InputEvent* a_head) {
        const bool dragging = g_dragging.load(std::memory_order_acquire);
        if (!dragging && !Settling()) {
            return false;
        }
        bool held{false};
        for (auto* event = a_head; event; event = event->next) {
            if (event->deviceType != RE::InputEvent::DeviceType::kMouse) {
                continue;
            }
            if (IsLeftButton(*event)) {
                held = true;
            } else if (event->eventType == RE::InputEvent::EventType::kCursorMove) {
                // Hidden, the OS cursor is held at the screen centre
                event->status = RE::InputEvent::Status::kStop;
            } else if (dragging && event->eventType == RE::InputEvent::EventType::kMouseMove) {
                auto& id = *static_cast<RE::IDEvent*>(event);
                id.strUserEvent = g_look;
                reinterpret_cast<std::uint8_t&>(id.disabled) = 0;
            }
        }
        // The game repeats a held button every frame, so none means the release was lost (e.g. alt-tab)
        if (dragging && a_head && !held) {
            EndDrag();
        }
        return dragging;
    }
}
