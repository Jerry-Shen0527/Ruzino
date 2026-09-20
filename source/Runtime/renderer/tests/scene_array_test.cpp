#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <algorithm>

#include "../source/internal/memory/SceneArray.hpp"

using namespace Ruzino;

class SceneArrayTest : public ::testing::Test {
   protected:
    void SetUp() override
    {
        RHI::init();
        spdlog::set_level(spdlog::level::warn);
    }
    void TearDown() override
    {
        RHI::shutdown();
    }

    struct Entry {
        int value = 0;
        float weight = 0.0f;
    };
};

// Acceptance counterpart of MemoryPoolTest.DISABLED_dense_read_after_erase:
// after removing a middle entry, the dense view must contain exactly the
// live values — no stale holes, no compaction step required.
TEST_F(SceneArrayTest, dense_view_after_erase)
{
    SceneArray<Entry> array;

    auto a = array.add({ 111, 1.0f });
    auto b = array.add({ 222, 2.0f });
    auto c = array.add({ 333, 3.0f });
    ASSERT_EQ(array.count(), 3u);

    b = nullptr;  // drop the middle entry

    ASSERT_EQ(array.count(), 2u);
    array.flush();

    auto dense = array.get_dense_content();
    ASSERT_EQ(dense.size(), 2u);
    for (const auto& e : dense) {
        EXPECT_TRUE(e.value == 111 || e.value == 333)
            << "dense view holds " << e.value << " (stale hole data 222?)";
    }
}

// Acceptance counterpart of
// MemoryPoolTest.DISABLED_cross_reference_breaks_on_compress: a slot id
// captured when the entry was created must keep resolving to the same data
// through any churn (removals + additions reusing slots).
TEST_F(SceneArrayTest, slot_stability_through_churn)
{
    SceneArray<Entry> array;

    auto a = array.add({ 111, 1.0f });
    auto b = array.add({ 222, 2.0f });
    auto c = array.add({ 333, 3.0f });

    const uint32_t saved_slot_of_c = c->slot();

    b = nullptr;                        // middle entry removed
    auto d = array.add({ 444, 4.0f });  // may reuse b's slot
    array.flush();

    // c's slot is untouched by any bookkeeping elsewhere
    EXPECT_EQ(c->slot(), saved_slot_of_c);

    // and the entry still resolves through update
    EXPECT_TRUE(array.update(c, { 333, 9.0f }));
    array.flush();  // push the updated row to the GPU mirror
    auto dense = array.get_dense_content();
    int hits = 0;
    for (const auto& e : dense) {
        if (e.value == 333) {
            EXPECT_FLOAT_EQ(e.weight, 9.0f);
            hits++;
        }
    }
    EXPECT_EQ(hits, 1);
}

// Stale handles cannot normally be produced (RAII removes on last release,
// and generation bumps invalidate any slot reuse), so this exercises the
// positive path: the recycled-slot occupant updates fine and the dense view
// reflects it exactly.
TEST_F(SceneArrayTest, stale_update_rejected)
{
    SceneArray<Entry> array;

    auto occupant = array.add({ 222, 2.0f });
    const uint32_t slot = occupant->slot();

    auto also = occupant;  // second reference keeps the entry alive
    occupant = nullptr;    // NOT removed yet (one reference left)
    EXPECT_EQ(array.count(), 1u);

    EXPECT_TRUE(array.update(also, { 222, 5.0f }));

    auto dense = array.get_dense_content();
    ASSERT_EQ(dense.size(), 1u);
    EXPECT_EQ(dense[0].value, 222);
    EXPECT_FLOAT_EQ(dense[0].weight, 5.0f);
    EXPECT_EQ(also->slot(), slot);
}

// GPU round trip: add entries, flush, read back the dense view.
TEST_F(SceneArrayTest, gpu_roundtrip)
{
    SceneArray<Entry> array;

    std::vector<SceneArray<Entry>::Handle> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(array.add({ i * 10, float(i) }));
    }
    // remove every other entry to punch holes
    for (size_t i = 0; i < handles.size(); i += 2) {
        handles[i] = nullptr;
    }
    array.flush();

    auto dense = array.get_dense_content();
    ASSERT_EQ(dense.size(), 5u);
    for (const auto& e : dense) {
        EXPECT_EQ(e.value % 20, 10) << "even entries must all be gone";
    }
}

// A handle may outlive the pool (nothing structurally prevents it): the
// liveness token must make its destruction a no-op instead of a UAF.
TEST_F(SceneArrayTest, handle_outlives_pool)
{
    auto* array = new SceneArray<Entry>();
    auto handle = array->add({ 42, 1.0f });
    array->flush();
    delete array;  // pool dies first, handle still alive

    // destruction of the handle must not touch the dead pool
    EXPECT_NO_FATAL_FAILURE({ handle = nullptr; });
}

// Dense-output mode (pure-leaf tables like draw-indirect args): the GPU
// buffer must contain exactly the live entries, contiguously, in any
// removal order — the property ExecuteIndirect relies on.
TEST_F(SceneArrayTest, dense_output_contiguous_after_churn)
{
    SceneArray<Entry, true> array;

    std::vector<SceneArray<Entry, true>::Handle> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(array.add({ i * 10, float(i) }));
    }
    // remove scattered entries to punch holes
    handles[1] = nullptr;
    handles[2] = nullptr;
    handles[5] = nullptr;
    array.flush();

    ASSERT_EQ(array.count(), 5u);
    auto dense = array.get_dense_content();
    ASSERT_EQ(dense.size(), 5u);
    // exactly the live values, no stale holes
    std::vector<int> values;
    for (const auto& e : dense) {
        values.push_back(e.value);
    }
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int>{ 0, 30, 40, 60, 70 }));

    // dense buffer address must be stable across flushes that do not grow
    // capacity (consumers cache the pointer between frames)
    auto* buf_before = array.get_dense_buffer();
    handles[3] = nullptr;
    array.flush();
    EXPECT_EQ(array.get_dense_buffer(), buf_before);
    EXPECT_EQ(array.count(), 4u);
}
