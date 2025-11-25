# Bug Fix: Subscription Flow Failure (2024)

## Problem Summary

After refactoring the dataflow architecture and migrating to MSVC, the subscription flow completely broke. The application would connect successfully to Coinbase WebSocket, but clicking "Subscribe" resulted in no market data flowing. The heatmap remained empty and no subscription acknowledgments were received.

## Root Causes Identified

### 1. I/O Thread Dying on Exceptions (CRITICAL)

**Symptom**: Subscription lambda never executed, even though `net::post()` calls completed successfully.

**Root Cause**: When JWT generation threw an exception in `replaySubscriptionsOnConnect()`, the exception propagated up to `MarketDataCore::run()`, causing `m_ioc.run()` to exit. This killed the I/O thread, so all subsequent posted work (including subscription requests) never executed.

**Fix**: Modified `run()` to catch exceptions and restart the io_context, keeping the I/O thread alive:

```cpp
void MarketDataCore::run() {
    while (m_running.load()) {
        try {
            m_ioc.run();
            if (m_running.load()) {
                m_ioc.restart();
            }
        } catch (const std::exception& e) {
            sLog_Error(QString("IO context thread exception: %1 - restarting I/O loop").arg(e.what()));
            if (m_running.load()) {
                m_ioc.restart();
            }
        }
    }
}
```

**Impact**: I/O thread now survives handler exceptions and continues processing work.

### 2. JWT Nonce Encoding Issue (CRITICAL)

**Symptom**: JWT generation failed with `invalid UTF-8 byte at index 0: 0x89` (or similar binary byte errors).

**Root Cause**: The nonce was being created as raw binary bytes (16 random bytes), but jwt-cpp's JSON serializer requires UTF-8 safe strings. When jwt-cpp tried to serialize the JWT claims to JSON, it encountered invalid UTF-8 bytes in the nonce field.

**Fix**: Base64-encode the nonce before adding it as a JWT claim:

```cpp
// Generate random nonce
unsigned char nonce_raw[16];
RAND_bytes(nonce_raw, sizeof(nonce_raw));

// Base64 encode for JSON safety
BIO *bio, *b64;
BUF_MEM *bufferPtr;
b64 = BIO_new(BIO_f_base64());
bio = BIO_new(BIO_s_mem());
bio = BIO_push(b64, bio);
BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
BIO_write(bio, nonce_raw, sizeof(nonce_raw));
BIO_flush(bio);
BIO_get_mem_ptr(bio, &bufferPtr);
std::string nonce(bufferPtr->data, bufferPtr->length);
BIO_free_all(bio);

// Use base64-encoded nonce in JWT
.set_header_claim("nonce", jwt::claim(nonce))
```

**Impact**: JWT generation now succeeds consistently.

### 3. Missing JWT Error Handling (IMPORTANT)

**Symptom**: When JWT creation failed, the entire I/O thread would crash.

**Root Cause**: JWT creation was called without try/catch in subscription handlers, allowing exceptions to propagate and kill the I/O thread.

**Fix**: Added try/catch around JWT creation in:
- `sendSubscriptionMessage()` subscription handler
- `sendHeartbeatSubscribe()` heartbeat handler

```cpp
std::string jwt;
try {
    jwt = m_auth.createJwt();
} catch (const std::exception& e) {
    sLog_Error(QString("JWT creation failed: %1").arg(e.what()));
    emitError(QString("Failed to create JWT for subscription: %1").arg(e.what()));
    return; // Exit gracefully without crashing I/O thread
}
```

**Impact**: JWT failures are now handled gracefully without crashing the I/O thread.

## Technical Details

### Boost.Asio Strand Posting

**Finding**: Posting to a strand from external threads (like the GUI thread) works correctly in Boost.Asio. The issue was not with strand posting itself, but with the I/O thread being dead.

**Key Insight**: `net::post(strand, handler)` is thread-safe and can be called from any thread. The work is queued and executed by the thread(s) running the io_context. If the io_context thread is dead, posted work will never execute.

### Threading Model

- **GUI Thread**: Qt event loop, calls `subscribeToSymbols()` → `sendSubscriptionMessage()`
- **I/O Thread**: Runs `m_ioc.run()`, processes all async operations and strand work
- **Communication**: GUI → I/O via `net::post(strand, ...)`, I/O → GUI via Qt signals

### Exception Propagation

When an exception is thrown in an async handler:
- If caught within the handler: Handler completes, I/O thread continues
- If not caught: Exception propagates to `io_context::run()`, causing it to exit
- **Solution**: Catch exceptions in handlers AND in `run()` to restart the loop

## Files Modified

1. **`libs/core/marketdata/MarketDataCore.cpp`**
   - Modified `run()` to restart io_context on exceptions
   - Added JWT error handling in `sendSubscriptionMessage()`
   - Added JWT error handling in `sendHeartbeatSubscribe()`
   - Added diagnostic logging (to be cleaned up)

2. **`libs/core/marketdata/auth/Authenticator.cpp`**
   - Added base64 encoding for JWT nonce
   - Added OpenSSL BIO includes for base64 encoding

## Testing & Verification

### Before Fix
- ❌ Connection established but subscriptions failed
- ❌ No subscription frames sent
- ❌ No SubscriptionAckEvent received
- ❌ No market data flowing
- ❌ Heatmap remained empty
- ❌ I/O thread died on JWT errors

### After Fix
- ✅ Connection establishes successfully
- ✅ Subscription frames sent correctly
- ✅ SubscriptionAckEvent received
- ✅ Market data (trades/order book) flows
- ✅ Heatmap updates with live data
- ✅ I/O thread survives JWT errors

## Lessons Learned

1. **Always wrap `io_context::run()` in exception handling** - Handler exceptions can kill the I/O thread
2. **JWT claims must be JSON-safe** - Binary data must be base64-encoded before adding to JWT claims
3. **Error handling in async handlers is critical** - Unhandled exceptions can propagate and crash the I/O loop
4. **Diagnostic logging is essential** - Without extensive logging, we never would have found that the lambda wasn't executing
5. **Boost.Asio strand posting works from any thread** - The issue was the I/O thread being dead, not strand posting

## Related Documentation

- `docs/MARKETDATA_ARCHITECTURE.md` - Overall architecture
- `SUBSCRIPTION_DEBUG_OUTLINE.md` - Debugging process (can be archived)
- `CLEANUP_OUTLINE.md` - Cleanup steps for diagnostic code

## Prevention

To prevent similar issues in the future:

1. **Always wrap `io_context::run()` in try/catch with restart logic**
2. **Validate JWT claim data is JSON-safe** (UTF-8 strings or base64-encoded binary)
3. **Add error handling around all JWT creation calls**
4. **Test subscription flow end-to-end after any refactoring**
5. **Monitor I/O thread health** (add periodic heartbeat logs)

