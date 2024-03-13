#define SEGMENT_NAME "segment1"
#include "test_parser2.hpp"

TEST_CASE("parser test various cases version 2 segment 1") {
#ifdef CMAKE_GITHUB_CI
    using escape = ss::escape<'\\'>;

    test_option_combinations3<>();
    test_option_combinations3<escape>();
#endif
}
