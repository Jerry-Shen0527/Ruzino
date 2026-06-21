// Unit tests for WindowEventSystem — the broadcast event bus used to deliver
// viewport input (brush / pick / editor-creation) to geometry editors.
//
// These tests validate the core invariant that the refactor relies on:
// every subscriber sees every event, regardless of registration order, with
// no destroy-on-read starvation. They construct only a WindowEventSystem
// (self-contained, no RHI/Window/ImGui), so they run fully headless.

#include <gtest/gtest.h>

#include <any>
#include <string>

#include "GUI/viewport_events.h"
#include "GUI/window.h"

using namespace Ruzino;

// ---------------------------------------------------------------------------
// Fixture: a freshly-constructed event system per test.
// ---------------------------------------------------------------------------
class WindowEventSystemTest : public ::testing::Test {
   protected:
    WindowEventSystem events;
};

// --- Basic any-bus semantics ------------------------------------------------

TEST_F(WindowEventSystemTest, SubscribeAnyReceivesEmittedPayload) {
    int received = 0;
    int captured_value = 0;

    events.subscribe_any(
        "test.event", [&](const std::any& data) {
            received++;
            try {
                captured_value = std::any_cast<int>(data);
            }
            catch (const std::bad_any_cast&) {
                FAIL() << "Payload type mismatch";
            }
        });

    events.emit_any("test.event", std::any(42));

    EXPECT_EQ(received, 1);
    EXPECT_EQ(captured_value, 42);
}

// --- THE key invariant: multiple subscribers all fire ----------------------
// This is the property the brush_capture bug violated under the old
// consume_* destroy-on-read model: the first consumer cleared the state and
// the second saw nothing. The broadcast bus must deliver to everyone.
TEST_F(WindowEventSystemTest, MultipleSubscribersAllReceiveEmission) {
    int hit_a = 0, hit_b = 0, hit_c = 0;

    events.subscribe_any(
        ViewportEvents::BRUSH_STATE, [&](const std::any&) { hit_a++; });
    events.subscribe_any(
        ViewportEvents::BRUSH_STATE, [&](const std::any&) { hit_b++; });
    events.subscribe_any(
        ViewportEvents::BRUSH_STATE, [&](const std::any&) { hit_c++; });

    events.emit_any(ViewportEvents::BRUSH_STATE, std::any(0));

    EXPECT_EQ(hit_a, 1);
    EXPECT_EQ(hit_b, 1);
    EXPECT_EQ(hit_c, 1);
}

// --- Every subscriber gets the SAME payload (no mutation between calls) -----
// A subscriber must not be able to "consume" the event away from later
// subscribers. Each receives an independent copy of the std::any.
TEST_F(WindowEventSystemTest, EachSubscriberGetsIndependentPayloadCopy) {
    int sum_a = 0, sum_b = 0;

    events.subscribe_any(
        "test.sum", [&](const std::any& data) {
            sum_a += std::any_cast<int>(data);
        });
    events.subscribe_any(
        "test.sum", [&](const std::any& data) {
            sum_b += std::any_cast<int>(data);
        });

    events.emit_any("test.sum", std::any(10));
    events.emit_any("test.sum", std::any(20));

    EXPECT_EQ(sum_a, 30);
    EXPECT_EQ(sum_b, 30);
}

// --- Distinct event names don't cross-talk ---------------------------------
TEST_F(WindowEventSystemTest, UnrelatedEventNamesDoNotCrossFire) {
    int brush_hits = 0;
    int pick_hits = 0;
    int editor_hits = 0;

    events.subscribe_any(
        ViewportEvents::BRUSH_STATE, [&](const std::any&) { brush_hits++; });
    events.subscribe_any(
        ViewportEvents::PICK_EVENT, [&](const std::any&) { pick_hits++; });
    events.subscribe_any(
        ViewportEvents::EDITOR_CREATION,
        [&](const std::any&) { editor_hits++; });

    events.emit_any(ViewportEvents::BRUSH_STATE, std::any(0));
    events.emit_any(ViewportEvents::PICK_EVENT, std::any(0));

    EXPECT_EQ(brush_hits, 1);
    EXPECT_EQ(pick_hits, 1);
    EXPECT_EQ(editor_hits, 0);
}

// --- Emitting an event with no subscribers is a no-op ----------------------
TEST_F(WindowEventSystemTest, EmitWithNoSubscribersIsSafe) {
    EXPECT_NO_THROW({
        events.emit_any("nobody.listening", std::any(0));
    });
}

// --- Registration order is delivery order ----------------------------------
// Subscribers fire in the order they were registered (push-back order). The
// geometry-editor fix relies on every editor seeing the event, but not on a
// specific order; this test just pins the documented behavior so regressions
// are caught.
TEST_F(WindowEventSystemTest, SubscribersFireInRegistrationOrder) {
    std::string sequence;

    events.subscribe_any(
        "order.test", [&](const std::any&) { sequence += "first;"; });
    events.subscribe_any(
        "order.test", [&](const std::any&) { sequence += "second;"; });
    events.subscribe_any(
        "order.test", [&](const std::any&) { sequence += "third;"; });

    events.emit_any("order.test", std::any(0));

    EXPECT_EQ(sequence, "first;second;third;");
}

// --- Realistic brush-state-style broadcast round-trip ---------------------
// Mirror what the production code emits/consumes: a structured payload
// delivered to multiple editors (subscribers), each updating its own buffer.
// Uses a plain struct to avoid coupling the unit test to glm headers.
TEST_F(WindowEventSystemTest, StructuredPayloadBroadcastsToMultipleEditors) {
    struct FakeBrush {
        float x, y, z;
        bool new_point;
    };
    struct EditorBuffer {
        float px = 0.0f;
        bool new_point = false;
    };
    EditorBuffer editor_a, editor_b;

    auto subscribe_editor = [&](EditorBuffer& buf) {
        events.subscribe_any(
            ViewportEvents::BRUSH_STATE,
            [&](const std::any& data) {
                try {
                    auto state = std::any_cast<FakeBrush>(data);
                    buf.px = state.x;
                    buf.new_point = state.new_point;
                }
                catch (const std::bad_any_cast&) {
                    FAIL() << "Expected FakeBrush payload";
                }
            });
    };
    subscribe_editor(editor_a);
    subscribe_editor(editor_b);

    // Simulate a brush drag: emit a new point.
    FakeBrush stroke{1.5f, 2.0f, 0.0f, true};
    events.emit_any(ViewportEvents::BRUSH_STATE, std::any(stroke));

    // Both editors must see the exact same point and the new-point flag.
    EXPECT_EQ(editor_a.new_point, true);
    EXPECT_EQ(editor_b.new_point, true);
    EXPECT_FLOAT_EQ(editor_a.px, 1.5f);
    EXPECT_FLOAT_EQ(editor_b.px, 1.5f);
}
