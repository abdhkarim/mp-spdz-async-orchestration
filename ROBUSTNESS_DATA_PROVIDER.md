# Data Provider - Améliorations de Robustesse

## Vue d'ensemble

Le module `data_provider` a été amélioré significativement pour offrir une production-grade robustesse, avec un système de logging comprehensive, une validation stricte des entrées, une gestion d'erreurs correcte et des tests unitaires complets.

## Améliorations Implémentées

### 1. 📊 Système de Logging Robuste

**Classe `Logger`** :
- **Niveaux de log** : DEBUG, INFO, WARN, ERROR
- **Timestamps précis** : Format `YYYY-MM-DD HH:MM:SS`
- **Sortie structurée** : `[TIMESTAMP] [LEVEL] message`
- **Séparation stdout/stderr** : Les erreurs vont à `stderr`, le reste à `stdout`

```cpp
g_logger.info("Provider 1 wrote input file: /path/to/file");
g_logger.warn("Using default secret; set MPC_PROVIDER_SECRET for production");
g_logger.error("Invalid provider ID: 'test@123'");
```

**Avantages** :
- Facilite le debugging et les audits
- Permet la traçabilité complète des opérations
- Logs horodatés pour corrélation d'événements

---

### 2. ✅ Validation Stricte des Entrées

#### Validation ID (`validate_id()`)
- ✓ Longueur : 1-32 caractères
- ✓ Contenu : Alphanumérique, underscore, hyphen uniquement
- ✗ Prévention path traversal (`../`, `/`, etc.)

```cpp
validate_id("1")          // ✓ OK
validate_id("node-01")    // ✓ OK
validate_id("../escape")  // ✗ REJETÉ
validate_id("test@1")     // ✗ REJETÉ (caractère invalide)
```

#### Validation Value (`validate_value()`)
- ✓ Longueur : 1-64 caractères
- ✓ Format : Numérique (parseInt via `stoll()`)
- ✗ Pas de valeurs flottantes, scientifiques, ou vides

```cpp
validate_value("42")          // ✓ OK
validate_value("-100")        // ✓ OK
validate_value("42.5")        // ✗ REJETÉ
validate_value("1e10")        // ✗ REJETÉ
```

---

### 3. 🛡️ Gestion d'Erreurs Comprendre

**Try-catch Global** :
```cpp
try {
    // logique principale
} catch (const std::exception& e) {
    g_logger.error("Exception: " + std::string(e.what()));
    return 1;
}
```

**Vérifications Explicites** :
- Retour des appels `crypto_generichash_*` (>= 0 = succès)
- État du fichier après écriture (`out.good()`)
- Initialisation de libsodium
- Ouverture/fermeture de fichiers

**Erreurs Loggées** :
```
[2026-03-09 14:30:15] [ERROR] Invalid provider ID: 'test@1' (must be alphanumeric, 1-32 chars)
[2026-03-09 14:30:16] [ERROR] Failed to open output file: /path/to/file
[2026-03-09 14:30:17] [ERROR] Exception in write_provider_file: crypto_generichash_init failed
```

---

### 4. 🧪 Tests Unitaires Complets

**File** : `node/src/test_data_provider.cpp`

#### Test Suite: `validate_id()`
```
[PASS] Single digit ID
[PASS] ID with underscore
[PASS] ID with hyphen
[PASS] Mixed alphanumeric
[PASS] Empty ID (rejected)
[PASS] ID too long (rejected)
[PASS] ID with special char (rejected)
[PASS] ID with path separator (rejected)
[PASS] ID with path traversal (rejected)
[PASS] ID with space (rejected)
```

#### Test Suite: `validate_value()`
```
[PASS] Zero
[PASS] Positive integer
[PASS] Negative integer
[PASS] Max int64
[PASS] Empty value (rejected)
[PASS] Non-numeric value (rejected)
[PASS] Floating point (rejected)
[PASS] Scientific notation (rejected)
[PASS] Value too long (rejected)
```

#### Test Suite: `to_hex()`
```
[PASS] Empty data produces empty string
[PASS] Single byte 0xAB -> 'ab'
[PASS] Multiple bytes conversion
[PASS] Full hex range
```

#### Test Suite: `compute_proof()`
```
[PASS] Same inputs produce same proof
[PASS] Proof is 64 hex chars (32 bytes)
[PASS] Different value produces different proof
[PASS] Different secret produces different proof
```

---

### 5. 🧹 Nettoyage du Code

#### Séparation des Responsabilités
- **Logger** : Système de logging unifié
- **Validation** : Fonctions dédiées `validate_id()`, `validate_value()`
- **Crypto** : Fonction `compute_proof()` avec vérifications d'erreur
- **I/O** : Fonction `write_provider_file()` centralisée
- **Main** : Orchestration et parsing

#### Headers Réorganisés
```cpp
// math + utilities
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sodium.h>
#include <string>
#include <sstream>
```

#### Meilleure Documentation
```cpp
// ============================================================================
// Logging System
// ============================================================================

// ============================================================================
// Utility Functions
// ============================================================================

// ============================================================================
// Cryptographic Functions
// ============================================================================

// ============================================================================
// Main Provider Logic
// ============================================================================
```

---

## Compilation et Exécution

### Compilation Standard
```bash
cd build
cmake ..
cmake --build .
```

L'exécutable `data_provider` est généré dans `build/node/`.

### Compilation + Tests
```bash
cd build
cmake ..
cmake --build .
ctest --output-on-failure
```

Ou pour exécuter les tests directement :
```bash
./node/test_data_provider
```

**Sortie attendue** :
```
╔════════════════════════════════════════════╗
║   Data Provider Unit Tests                 ║
╚════════════════════════════════════════════╝

=== Test Suite: validate_id ===
[PASS] Single digit ID
[PASS] ID with underscore
...
[PASS] ID with path traversal

=== Test Suite: validate_value ===
[PASS] Zero
...

=== Test Suite: to_hex ===
...

=== Test Suite: compute_proof ===
...

╔════════════════════════════════════════════╗
║   Test Summary                             ║
╠════════════════════════════════════════════╣
║  Passed: 28
║  Failed: 0
╚════════════════════════════════════════════╝
```

---

## Exemples d'Utilisation

### Exécution Normal avec Logs
```bash
./data_provider 1 42

# Output:
# [2026-03-09 14:30:15] [INFO ] === Data Provider Initialization ===
# [2026-03-09 14:30:15] [DEBUG] ID: 1, Value: 42, Mode: NORMAL
# [2026-03-09 14:30:15] [DEBUG] Input validation passed
# [2026-03-09 14:30:15] [DEBUG] Libsodium initialized
# [2026-03-09 14:30:15] [WARN ] Using default secret; set MPC_PROVIDER_SECRET for production
# [2026-03-09 14:30:15] [DEBUG] Created/verified inputs directory: ...
# [2026-03-09 14:30:15] [DEBUG] Opened output file: ...
# [2026-03-09 14:30:15] [DEBUG] Generated nonce: 0123456789...
# [2026-03-09 14:30:15] [DEBUG] Computed cryptographic proof
# [2026-03-09 14:30:15] [INFO ] Valid signature generated for provider 1 with value 42
# [2026-03-09 14:30:15] [INFO ] Provider 1 wrote input file: .../provider_1.txt
# [2026-03-09 14:30:15] [INFO ] === Provider execution successful ===
```

### Avec Secret Personnalisé
```bash
export MPC_PROVIDER_SECRET="my-secure-secret"
./data_provider 2 100
```

### Mode Malveillant
```bash
./data_provider 3 99 --malformed

# Output:
# [2026-03-09 14:30:20] [INFO ] === Data Provider Initialization ===
# ...
# [2026-03-09 14:30:20] [WARN ] Writing MALFORMED data for provider 3 (simulated attack)
# [2026-03-09 14:30:20] [INFO ] Provider 3 wrote input file: ...
```

### Validation d'Erreurs
```bash
./data_provider "test@invalid" 42
# [2026-03-09 14:30:25] [ERROR] Invalid provider ID: 'test@invalid' (...)

./data_provider bad_id
# Usage: ./data_provider <id> <value> [--malformed]

./data_provider 1 corrupted_value
# [2026-03-09 14:30:30] [ERROR] Invalid value: 'corrupted_value' (...)
```

---

## Architecture Améliorations

```
data_provider.cpp
├── Logger (class)
│   └── log(), info(), warn(), error(), debug()
├── Utility Functions
│   ├── to_hex()
│   ├── validate_id()
│   └── validate_value()
├── Cryptographic Functions
│   └── compute_proof()
├── Provider Logic
│   └── write_provider_file()
└── main()
```

---

## Security & Reliability

| Aspect | Avant | Après |
|--------|-------|-------|
| **Logging** | Minimal basic output | Structured timestamps, levels, audit trail |
| **Input Validation** | Aucune | Strict alphanumeric, length checks, path traversal prevention |
| **Error Handling** | Try-catch minimal | Comprehensive error checks, crypto return values |
| **File I/O Safety** | Basic open/close | State verification after writes |
| **Crypto Verification** | Implicit success | Explicit >= 0 checks on all operations |
| **Testing** | Aucun | 28 unit tests covering all components |
| **Code Organization** | Linear | Separated concerns, clear sections |

---

## Prochaines Étapes (Optionnel)

1. **Integration Testing** : Tests d'intégration avec `consensus` module
2. **Performance Testing** : Benchmark de génération de preuve
3. **Fuzzing** : Test avec inputs aléatoires/malformés
4. **Seccomp/Sandbox** : Isolation du processus provider
5. **Audit Logging** : Sauvegarder les logs en fichier sécurisé
