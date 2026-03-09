/**
 * Unit tests for data_provider components.
 * Can be compiled standalone for testing, or integrated into test suite.
 */

#include <cassert>
#include <iostream>
#include <sodium.h>
#include <string>

// ============================================================================
// Test Utilities
// ============================================================================
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg)                                  \
    do {                                                             \
        if (condition) {                                             \
            std::cout << "[PASS] " << msg << "\n";                   \
            tests_passed++;                                          \
        } else {                                                     \
            std::cerr << "[FAIL] " << msg << "\n";                   \
            tests_failed++;                                          \
        }                                                            \
    } while (0)

// ============================================================================
// Functions under test (copied/declared for testing)
// ============================================================================
bool validate_id(const std::string& id) {
    if (id.empty() || id.length() > 32) return false;
    for (char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool validate_value(const std::string& value) {
    if (value.empty() || value.length() > 64) return false;
    
    // Reject floating point and scientific notation
    for (char c : value) {
        if (c == '.' || c == 'e' || c == 'E') return false;
    }
    
    try {
        std::stoll(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::string to_hex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string compute_proof(const std::string& id,
                          const std::string& value,
                          const std::string& nonce,
                          const std::string& secret) {
    const std::string message = "id=" + id + ";value=" + value + ";nonce=" + nonce;
    unsigned char digest[crypto_generichash_BYTES] = {0};
    crypto_generichash_state state;

    if (crypto_generichash_init(&state,
                                reinterpret_cast<const unsigned char*>(secret.data()),
                                secret.size(),
                                sizeof(digest)) != 0) {
        throw std::runtime_error("crypto_generichash_init failed");
    }

    if (crypto_generichash_update(&state,
                                   reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size()) != 0) {
        throw std::runtime_error("crypto_generichash_update failed");
    }

    if (crypto_generichash_final(&state, digest, sizeof(digest)) != 0) {
        throw std::runtime_error("crypto_generichash_final failed");
    }

    return to_hex(digest, sizeof(digest));
}

// ============================================================================
// Test Suite: ID Validation
// ============================================================================
void test_validate_id() {
    std::cout << "\n=== Test Suite: validate_id ===\n";

    // Valid IDs
    TEST_ASSERT(validate_id("1"), "Single digit ID");
    TEST_ASSERT(validate_id("provider_1"), "ID with underscore");
    TEST_ASSERT(validate_id("node-01"), "ID with hyphen");
    TEST_ASSERT(validate_id("a1b2c3"), "Mixed alphanumeric");

    // Invalid IDs
    TEST_ASSERT(!validate_id(""), "Empty ID");
    TEST_ASSERT(!validate_id(std::string(33, 'a')), "ID too long (>32 chars)");
    TEST_ASSERT(!validate_id("provider@1"), "ID with special char (@)");
    TEST_ASSERT(!validate_id("provider/1"), "ID with path separator (/)");
    TEST_ASSERT(!validate_id("../escape"), "ID with path traversal");
    TEST_ASSERT(!validate_id("provider 1"), "ID with space");
}

// ============================================================================
// Test Suite: Value Validation
// ============================================================================
void test_validate_value() {
    std::cout << "\n=== Test Suite: validate_value ===\n";

    // Valid values
    TEST_ASSERT(validate_value("0"), "Zero");
    TEST_ASSERT(validate_value("42"), "Positive integer");
    TEST_ASSERT(validate_value("-100"), "Negative integer");
    TEST_ASSERT(validate_value("9223372036854775807"), "Max int64");

    // Invalid values
    TEST_ASSERT(!validate_value(""), "Empty value");
    TEST_ASSERT(!validate_value("abc"), "Non-numeric value");
    TEST_ASSERT(!validate_value("42.5"), "Floating point");
    TEST_ASSERT(!validate_value("1e10"), "Scientific notation");
    TEST_ASSERT(!validate_value(std::string(65, '1')), "Value too long (>64 chars)");
}

// ============================================================================
// Test Suite: Hexadecimal Conversion
// ============================================================================
void test_to_hex() {
    std::cout << "\n=== Test Suite: to_hex ===\n";

    // Test empty data
    TEST_ASSERT(to_hex(nullptr, 0) == "", "Empty data produces empty string");

    // Test single byte
    unsigned char single_byte = 0xAB;
    TEST_ASSERT(to_hex(&single_byte, 1) == "ab", "Single byte 0xAB -> 'ab'");

    // Test multiple bytes
    unsigned char bytes[] = {0x00, 0x0F, 0xF0, 0xFF};
    TEST_ASSERT(to_hex(bytes, 4) == "000ff0ff", "Multiple bytes conversion");

    // Test all hex digits
    unsigned char hex_test[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    TEST_ASSERT(to_hex(hex_test, 8) == "123456789abcdef0", "Full hex range");
}

// ============================================================================
// Test Suite: Cryptographic Proof
// ============================================================================
void test_compute_proof() {
    std::cout << "\n=== Test Suite: compute_proof ===\n";

    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium for crypto tests\n";
        return;
    }

    try {
        // Test deterministic proof generation
        std::string id = "test_provider";
        std::string value = "42";
        std::string nonce = "0123456789abcdef";
        std::string secret = "test-secret";

        std::string proof1 = compute_proof(id, value, nonce, secret);
        std::string proof2 = compute_proof(id, value, nonce, secret);

        TEST_ASSERT(proof1 == proof2, "Same inputs produce same proof");
        TEST_ASSERT(proof1.length() == 64, "Proof is 64 hex chars (32 bytes)");

        // Test different inputs produce different proofs
        std::string proof3 = compute_proof(id, "43", nonce, secret);
        TEST_ASSERT(proof1 != proof3, "Different value produces different proof");

        // Test secret change
        std::string proof4 = compute_proof(id, value, nonce, "different-secret");
        TEST_ASSERT(proof1 != proof4, "Different secret produces different proof");

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Crypto test exception: " << e.what() << "\n";
        tests_failed++;
    }
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    std::cout << "╔════════════════════════════════════════════╗\n";
    std::cout << "║   Data Provider Unit Tests                 ║\n";
    std::cout << "╚════════════════════════════════════════════╝\n";

    test_validate_id();
    test_validate_value();
    test_to_hex();
    test_compute_proof();

    // Summary
    std::cout << "\n╔════════════════════════════════════════════╗\n";
    std::cout << "║   Test Summary                             ║\n";
    std::cout << "╠════════════════════════════════════════════╣\n";
    std::cout << "║  Passed: " << tests_passed << " \n";
    std::cout << "║  Failed: " << tests_failed << " \n";
    std::cout << "╚════════════════════════════════════════════╝\n";

    return tests_failed > 0 ? 1 : 0;
}
