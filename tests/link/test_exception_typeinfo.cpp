// FM-145 runtime guard. This binary links what sentinel-gui links. If any
// static library in that set supplies a hidden copy of a std exception typeinfo,
// the handlers below bind to it and miss exceptions whose typeinfo chain comes
// from libc++, and the throw escapes the try block (std::terminate).

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[gnu::noinline]] void throwRuntimeError()
{
    throw std::runtime_error("fm145 runtime_error");
}

// Thrown inside libc++.dylib, the way std::stoi reports bad input.
[[gnu::noinline]] int parseBadInt(const std::string &text)
{
    return std::stoi(text);
}

// Thrown through libc++'s __throw_length_error (std::length_error typeinfo was
// one of the copies the bad archive member carried).
[[gnu::noinline]] void reserveTooMuch()
{
    std::vector<int> v;
    v.reserve(v.max_size() + 1);
}

template <typename Fn>
std::string catchAsStdException(Fn fn)
{
    try {
        fn();
    } catch (const std::exception &e) {
        return e.what();
    } catch (...) {
        return "<missed by catch (const std::exception &)>";
    }
    return "<no exception>";
}

} // namespace

TEST(ExceptionTypeinfo, RuntimeErrorCaughtAsStdException)
{
    EXPECT_EQ(catchAsStdException([] { throwRuntimeError(); }), "fm145 runtime_error");
}

TEST(ExceptionTypeinfo, LibcxxInvalidArgumentCaughtAsStdException)
{
    const std::string what = catchAsStdException([] { (void)parseBadInt("not a number"); });
    EXPECT_NE(what, "<missed by catch (const std::exception &)>");
    EXPECT_NE(what, "<no exception>");
}

TEST(ExceptionTypeinfo, LengthErrorCaughtAsLogicError)
{
    bool caught = false;
    try {
        reserveTooMuch();
    } catch (const std::logic_error &) {
        caught = true;
    } catch (...) {
    }
    EXPECT_TRUE(caught);
}
