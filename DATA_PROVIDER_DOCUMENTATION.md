# Module: Data Provider (node/src/data_provider.cpp)

**Responsabilité** : Génération et signature cryptographique des contributions individuelles des data providers.

## Architecture

```
main()
├─ Parse & Validate Arguments
│  ├─ validate_id()
│  └─ validate_value()
├─ Initialize Cryptography
│  └─ sodium_init()
├─ Load Environment
│  └─ MPC_PROVIDER_SECRET (fallback: "mpc-demo-secret")
└─ Execute
   ├─ write_provider_file()
   │  ├─ Create/verify inputs/ directory
   │  ├─ If --malformed:
   │  │  └─ Write "this_file_is_malformed\n"
   │  └─ Else:
   │     ├─ Generate nonce (16 random bytes) -> hex
   │     ├─ Compute BLAKE2b proof
   │     └─ Write: id, value, nonce, proof
   └─ Log completion
```

## Classes & Functions

### Logger Class

```cpp
enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
  public:
    void log(LogLevel level, const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);
    void debug(const std::string& msg);
};
```

**Behavior**:
- Timestamps: `[YYYY-MM-DD HH:MM:SS]`
- Levels: `[DEBUG]`, `[INFO ]`, `[WARN ]`, `[ERROR]`
- Routing: ERROR → stderr, others → stdout
- Default: INFO level

### Input Validation Functions

#### `validate_id(const std::string& id) -> bool`

```cpp
bool validate_id(const std::string& id) {
    if (id.empty() || id.length() > 32) return false;
    for (char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}
```

**Rules**:
- ✅ Min: 1 char
- ✅ Max: 32 chars
- ✅ Allowed: [a-zA-Z0-9_-]
- ❌ Reject: empty, >32, special chars, path traversal

**Examples**:
- `"1"` → ✅
- `"node-01"` → ✅
- `"../escape"` → ❌
- `"test@evil"` → ❌

#### `validate_value(const std::string& value) -> bool`

```cpp
bool validate_value(const std::string& value) {
    if (value.empty() || value.length() > 64) return false;
    
    // Reject floating point and scientific notation
    for (char c : value) {
        if (c == '.' || c == 'e' || c == 'E') return false;
    }
    
    try {
        std::stoll(value);  // Parse as 64-bit integer
        return true;
    } catch (...) {
        return false;
    }
}
```

**Rules**:
- ✅ Min: 1 char
- ✅ Max: 64 chars
- ✅ Content: Decimal integers (signed or unsigned)
- ❌ Reject: empty, >64, floats, scientific, non-numeric

**Examples**:
- `"42"` → ✅
- `"0"` → ✅
- `"-100"` → ✅
- `"42.5"` → ❌
- `"1e10"` → ❌

### Cryptographic Functions

#### `to_hex(const unsigned char* data, size_t len) -> std::string`

Converts binary data to lowercase hexadecimal string.

**Example**:
```cpp
unsigned char bytes[] = {0xAB, 0xCD};
std::string hex = to_hex(bytes, 2);  // "abcd"
```

#### `compute_proof(...) -> std::string`

Computes BLAKE2b hash (64 hex chars = 32 bytes).

```cpp
std::string compute_proof(
    const std::string& id,           // e.g. "1"
    const std::string& value,        // e.g. "42"
    const std::string& nonce,        // e.g. "0123456789abcdef..."
    const std::string& secret        // shared secret
) -> std::string;  // 64 hex chars
```

**Message format**:
```
id=<id>;value=<value>;nonce=<nonce>
```

**Error handling**:
- Throws `std::runtime_error` if `crypto_generichash_*` fails
- All libsodium calls checked for return code >= 0

### File Writer Function

#### `write_provider_file(...) -> int`

Core logic for file generation. Wraps all provider functionality.

**Workflow**:
1. Create `inputs/` directory (subdirectory of CWD)
2. Open `inputs/provider_<id>.txt` for writing
3. Check file state after opening
4. If `malformed` flag:
   - Write single line: `"this_file_is_malformed\n"`
5. Else:
   - Generate 16 random bytes as nonce
   - Convert nonce to hex (32 chars)
   - Compute BLAKE2b proof
   - Write 4 lines:
     ```
     id=<id>
     value=<value>
     nonce=<nonce_hex>
     proof=<proof_hex>
     ```
6. Verify write succeeded (`out.good()`)
7. Close file and log result

**Return codes**:
- `0` : Success
- `1` : Any error (invalid input, file I/O, crypto failure)

## Output Format

### Successful Provider File

```
id=1
value=42
nonce=0123456789abcdef0123456789abcdef
proof=abcd1234...6789def0
```

### Malformed Provider File

```
this_file_is_malformed
```

## Log Output Examples

### Normal Execution

```
[2026-03-09 15:20:40] [INFO ] === Data Provider Initialization ===
[2026-03-09 15:20:40] [DEBUG] ID: 1, Value: 42, Mode: NORMAL
[2026-03-09 15:20:40] [DEBUG] Input validation passed
[2026-03-09 15:20:40] [DEBUG] Libsodium initialized
[2026-03-09 15:20:40] [WARN ] Using default secret; set MPC_PROVIDER_SECRET for production
[2026-03-09 15:20:40] [DEBUG] Created/verified inputs directory: /path/to/build/inputs
[2026-03-09 15:20:40] [DEBUG] Opened output file: /path/to/build/inputs/provider_1.txt
[2026-03-09 15:20:40] [DEBUG] Generated nonce: 0123456789...
[2026-03-09 15:20:40] [DEBUG] Computed cryptographic proof
[2026-03-09 15:20:40] [INFO ] Valid signature generated for provider 1 with value 42
[2026-03-09 15:20:40] [INFO ] Provider 1 wrote input file: /path/to/build/inputs/provider_1.txt
[2026-03-09 15:20:40] [INFO ] === Provider execution successful ===
```

### Malformed Mode

```
[2026-03-09 15:21:09] [INFO ] === Data Provider Initialization ===
[2026-03-09 15:21:09] [WARN ] Using default secret; set MPC_PROVIDER_SECRET for production
[2026-03-09 15:21:09] [WARN ] Writing MALFORMED data for provider 4 (simulated attack)
[2026-03-09 15:21:09] [INFO ] Provider 4 wrote input file: /path/to/build/inputs/provider_4.txt
[2026-03-09 15:21:09] [INFO ] === Provider execution successful ===
```

### Error Cases

```
[2026-03-09 15:21:20] [ERROR] Invalid provider ID: 'test@evil' (must be alphanumeric, 1-32 chars)

[2026-03-09 15:21:21] [ERROR] Invalid value: '42.5' (must be numeric, max 64 chars)

[2026-03-09 15:21:22] [ERROR] Failed to initialize libsodium

[2026-03-09 15:21:23] [ERROR] Failed to open output file: /path/to/build/inputs/provider_1.txt
```

## Usage

```bash
# Basic
./data_provider 1 42

# With custom secret
export MPC_PROVIDER_SECRET="production-secret-key"
./data_provider 2 100

# Simulate malicious provider
./data_provider 4 99 --malformed

# Invalid inputs (errors logged)
./data_provider "test@evil" 42      # Invalid ID
./data_provider 1 "not-numeric"     # Invalid value
```

## Security Considerations

| Aspect | Implementation |
|--------|-----------------|
| **Random Nonce** | `randombytes_buf()` from libsodium (cryptographically secure) |
| **Hash Function** | BLAKE2b (64-bit security, 32 bytes output) |
| **Key Derivation** | Direct use of shared secret (no KDF in this demo) |
| **Input Sanitization** | Strict validation before any processing |
| **Path Traversal** | Alphanumeric-only ID prevents `../` attacks |
| **Command Injection** | No shell execution in provider |

## Testing

See `ROBUSTNESS_DATA_PROVIDER.md` for 28 comprehensive unit tests.

## Dependencies

- `libsodium` (crypto library)
- `<filesystem>` (C++17 STL)
- `<chrono>` (timestamps)

## Compilation

```bash
cmake -B build && cmake --build build
```

Generates: `build/node/data_provider` (executable)

---

**Last Updated**: 9 Mars 2026
