#pragma once
#include "marketdata/auth/IAuthenticator.hpp"
#include <string>
#include <stdexcept>

/// Mock authenticator for testing without real JWT signing
class MockAuthenticator : public IAuthenticator {
public:
    explicit MockAuthenticator(std::string jwt = "test_jwt_token")
        : jwt_(std::move(jwt))
        , should_throw_(false)
    {}

    /// Return the pre-configured JWT
    std::string createJwt() const override {
        if (should_throw_) {
            throw std::runtime_error("Mock JWT creation failed");
        }
        return jwt_;
    }

    /// Configure the mock to throw on next createJwt() call
    void setShouldThrow(bool should_throw) {
        should_throw_ = should_throw;
    }

    /// Update the JWT returned by createJwt()
    void setJwt(std::string jwt) {
        jwt_ = std::move(jwt);
    }

private:
    std::string jwt_;
    bool should_throw_;
};
