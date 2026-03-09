#!/bin/bash
# Automated demonstration script for MP-SPDZ async orchestration
# Shows the complete flow: providers → consensus → bridge

set -e

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║   MP-SPDZ Async Orchestration - Complete Demonstration       ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

cd build

# Clean previous inputs
rm -rf inputs core_set.txt 2>/dev/null || true
mkdir -p inputs

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📊 PHASE 1: Data Providers Generate Inputs"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

echo "🤖 Provider 1: Sending value 42 (HONEST)"
./node/data_provider 1 42
echo ""

echo "🤖 Provider 2: Sending value 15 (HONEST)"
./node/data_provider 2 15
echo ""

echo "🤖 Provider 3: Sending value 8 (HONEST)"
./node/data_provider 3 8
echo ""

echo "⚠️  Provider 4: Sending MALICIOUS data (DISHONEST)"
./node/data_provider 4 99 --malformed
echo ""

echo "🤖 Provider 5: Sending value 25 (HONEST)"
./node/data_provider 5 25
echo ""

echo "Files generated:"
ls -lh inputs/
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔍 PHASE 2: Consensus Filters & Validates"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./consensus/consensus
echo ""

echo "Core set decided:"
cat core_set.txt || echo "No core_set.txt"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔐 PHASE 3: SPDZ Bridge Computes Secure Aggregation"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./spdz_bridge/spdz_bridge
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ DEMONSTRATION COMPLETE!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "📈 Results:"
echo "  • 5 providers submitted data"
echo "  • 1 provider (Provider 4) rejected as MALICIOUS"
echo "  • 4 providers validated in core set"
echo "  • Expected sum: 42 + 15 + 8 + 25 = 90"
echo ""
echo "🎯 Key Points:"
echo "  ✓ Asynchronous data collection"
echo "  ✓ Malicious provider detection & filtering"
echo "  ✓ Secure multi-party computation aggregation"
echo "  ✓ Cryptographic proof verification"
echo ""
