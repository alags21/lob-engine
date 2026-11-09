#include <gtest/gtest.h>

#include <string>

#include "lob/version.hpp"

TEST(Smoke, VersionIsSet) { EXPECT_FALSE(std::string(lob::version()).empty()); }
