#include "servermodel/BookRecorder.hpp"
#include <gtest/gtest.h>
#include <random>
using namespace recording;
TEST(PriceDecimal, ParsesPlainDecimalsExactly) {
    const auto p = [](const char *t) { return parsePriceDecimal(t); };
    auto d = p("97.1");
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 971u);
    EXPECT_EQ(d.fraction, 1u);
    d = p("97.10000000000001");
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 9710000000000001u);
    EXPECT_EQ(d.fraction, 14u);
    d = p("0.00000428");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(428u, 8u, true));
    d = p("100.500");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction)), std::tuple(1005u, 1u));
    d = p("1.000000000000000000000000000");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(1u, 0u, true));
    d = p("12345678901234567"); // 17 significant digits
    EXPECT_TRUE(d.exact);
    EXPECT_EQ(d.digits, 12345678901234567u);
    d = p("0.000000000000000000001"); // one significant digit, 21 places
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction), bool(d.exact)), std::tuple(1u, 21u, true));
    d = p("000123.4500");
    EXPECT_EQ(std::tuple(uint64_t(d.digits), uint64_t(d.fraction)), std::tuple(12345u, 2u));
}
TEST(PriceDecimal, MalformedLongAndExponentTextIsNeverExact) {
    // Coinbase sends plain decimals only; anything else has no guessed identity.
    for (const char *t : {"", ".5", "5.", "1e5", "1E-7", "1.5e2", "-1", "+1", " 1", "1 ", "1.2.3", "1,5", "0x1p3",
                          "inf", "nan", "1_000", "\xD9\xA1"})
        EXPECT_FALSE(parsePriceDecimal(t).exact) << t;
    EXPECT_FALSE(parsePriceDecimal("123456789012345678").exact);    // 18 significant digits
    EXPECT_FALSE(parsePriceDecimal("1.00000000000000001").exact);   // 18 significant digits
    EXPECT_FALSE(parsePriceDecimal(("0." + std::string(63, '0') + "1").c_str()).exact); // 64 places
    EXPECT_TRUE(parsePriceDecimal(("0." + std::string(62, '0') + "1").c_str()).exact);  // 63 places
}
TEST(PriceDecimal, IdentityAgainstTheRecorderKeyAndCheckedOverflow) {
    EXPECT_EQ(priceScaleDigits(100), 2);
    EXPECT_EQ(priceScaleDigits(1e8), 8);
    EXPECT_EQ(priceScaleDigits(1), 0);
    EXPECT_EQ(priceScaleDigits(1e18), 18);
    EXPECT_EQ(priceScaleDigits(1e19), -1);
    EXPECT_EQ(priceScaleDigits(250), -1);
    // The double hides it, the text does not: 97.10000000000001 passes the
    // binary64 OnGrid test at scale 100 and is still finer than the tick.
    const double hidden = std::strtod("97.10000000000001", nullptr);
    EXPECT_EQ(pricePrecision(hidden, 9710, 100), RecorderSink::Precision::OnGrid);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.10000000000001"), 2, 9710), DecimalIdentity::Finer);
    EXPECT_EQ(std::strtod("99.000000000000001", nullptr), 99.0); // identical double, 17 digits
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("99.000000000000001"), 2, 9900), DecimalIdentity::Finer);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("99.0000000000000001"), 2, 9900), DecimalIdentity::NoText); // 18 digits
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), 2, 9710), DecimalIdentity::Exact);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), 2, 9711), DecimalIdentity::Mismatch);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("0.00000428"), 8, 428), DecimalIdentity::Exact);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("0.000004285"), 8, 428), DecimalIdentity::Finer);
    EXPECT_EQ(decimalIdentity(PriceDecimal{}, 2, 9710), DecimalIdentity::NoText);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("97.1"), -1, 9710), DecimalIdentity::NoText);
    // Checked arithmetic: 9.2 at 10^18 fits int64; 9.3 overflows and never wraps.
    EXPECT_EQ(decimalUnits(parsePriceDecimal("9.2"), 18), std::optional<int64_t>(9'200'000'000'000'000'000));
    EXPECT_EQ(decimalUnits(parsePriceDecimal("9.3"), 18), std::nullopt);
    EXPECT_EQ(decimalIdentity(parsePriceDecimal("9.3"), 18, 0), DecimalIdentity::Mismatch);
    EXPECT_EQ(decimalUnits(parsePriceDecimal("92233720368547758"), 2), std::optional<int64_t>(9223372036854775800));
    EXPECT_EQ(decimalUnits(parsePriceDecimal("92233720368547759"), 2), std::nullopt); // 2^63 - 1 + 93
    EXPECT_EQ(decimalUnits(parsePriceDecimal("97.1"), 19), std::nullopt);
    // Random on-grid decimals at both production scales are always Exact against
    // the recorder's own key; one more digit is always Finer.
    std::mt19937_64 rng(11);
    for (const int k : {2, 8})
        for (int i = 0; i < 100'000; ++i) {
            const int64_t units = int64_t(rng() % (int64_t{1} << 40)) + 1;
            std::string text = std::to_string(units);
            if (text.size() <= size_t(k)) text.insert(0, size_t(k) + 1 - text.size(), '0');
            text.insert(text.size() - size_t(k), 1, '.');
            const auto key = *priceUnits(std::strtod(text.c_str(), nullptr), double(kPow10[size_t(k)]));
            ASSERT_EQ(decimalIdentity(parsePriceDecimal(text), k, key), DecimalIdentity::Exact) << text;
            ASSERT_EQ(decimalIdentity(parsePriceDecimal(text + "1"), k, key), DecimalIdentity::Finer) << text;
        }
}
TEST(PriceDecimal, DiagnosticTextKeepsHeadTailAndLength) {
    EXPECT_EQ(diagnosticPriceText("99999.5abc"), "99999.5abc");
    const std::string longText = "99999." + std::string(600, '1') + "XYZ";
    const auto d = diagnosticPriceText(longText);
    EXPECT_LE(d.size(), kMaxPriceText + 48);
    EXPECT_EQ(d.substr(0, 6), "99999.");
    EXPECT_NE(d.find("[609 chars, middle omitted]"), std::string::npos) << d;
    EXPECT_EQ(d.substr(d.size() - 3), "XYZ");
}
