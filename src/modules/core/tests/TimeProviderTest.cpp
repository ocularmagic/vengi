/**
 * @file
 */

#include "core/TimeProvider.h"
#include "core/tests/TestHelper.h"

namespace core {

class TimeProviderTest : public testing::Test {};

TEST_F(TimeProviderTest, testToStringEpoch) {
	EXPECT_EQ("01-01-1970 00-00-00", TimeProvider::toString(0u));
	EXPECT_EQ("01-01-1970 00-00-01", TimeProvider::toString(1000u));
}

TEST_F(TimeProviderTest, testToStringBeyond32BitMillis) {
	// 2021-01-01 00:00:00 UTC. Milliseconds do not fit in 32-bit unsigned long
	// (the previous parameter type), which made Windows file dialog dates show 1970.
	const uint64_t millis = 1609459200000ull;
	EXPECT_EQ("01-01-2021 00-00-00", TimeProvider::toString(millis));
}

TEST_F(TimeProviderTest, testToStringCustomFormat) {
	const uint64_t millis = 1609459200000ull;
	EXPECT_EQ("2021-01-01", TimeProvider::toString(millis, "%Y-%m-%d"));
}

} // namespace core
