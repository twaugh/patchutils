#!/bin/bash
# Run AFL++ fuzzing on patchutils tools

set -e

SCRIPT_DIR="$(dirname "$0")"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CORPUS_DIR="$SCRIPT_DIR/corpus"
RESULTS_DIR="$SCRIPT_DIR/results"
DICT_FILE="$SCRIPT_DIR/patch.dict"

# Default tool to fuzz
TOOL="${1:-filterdiff}"

echo "Starting AFL++ fuzzing for $TOOL..."

# Ensure we have a corpus
if [ ! -d "$CORPUS_DIR" ] || [ -z "$(ls -A "$CORPUS_DIR")" ]; then
    echo "Generating corpus..."
    "$SCRIPT_DIR/generate_corpus.sh"
fi

# Ensure results directory exists
mkdir -p "$RESULTS_DIR/$TOOL"

# Check if we have the normal binaries built
if [ ! -f "$PROJECT_DIR/src/$TOOL" ]; then
    echo "Building patchutils tools..."
    cd "$PROJECT_DIR"
    make clean >/dev/null 2>&1 || true

    # Try to configure if not already done
    if [ ! -f Makefile ]; then
        if [ ! -f configure ]; then
            ./bootstrap
        fi
        ./configure
    fi

    make -j$(nproc)
    cd - >/dev/null
fi

# AFL++ system configuration check
echo "Checking AFL++ system configuration..."
if [ -f /proc/sys/kernel/core_pattern ]; then
    if grep -q "|" /proc/sys/kernel/core_pattern; then
        echo "Warning: Core pattern contains pipe - this may interfere with fuzzing"
        echo "Consider running: echo core | sudo tee /proc/sys/kernel/core_pattern"
    fi
fi

# Set AFL++ environment variables for better performance
export AFL_SKIP_CPUFREQ=1
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1

echo "Starting fuzzing campaign for $TOOL..."
echo "Corpus: $CORPUS_DIR ($(ls -1 "$CORPUS_DIR" | wc -l) files)"
echo "Results: $RESULTS_DIR/$TOOL"
echo "Dictionary: $DICT_FILE"
echo
echo "Press Ctrl+C to stop fuzzing"
echo

# Create specific fuzzing commands for different tools
case "$TOOL" in
    filterdiff)
        # Test filterdiff with list mode (most common usage)
        afl-fuzz \
            -i "$CORPUS_DIR" \
            -o "$RESULTS_DIR/$TOOL" \
            -x "$DICT_FILE" \
            -t 5000 \
            -m none \
            -n \
            -- "$PROJECT_DIR/src/filterdiff" --list
        ;;
    interdiff)
        # For interdiff, we need two patch files - use a simple approach
        echo "Note: interdiff fuzzing requires two patch files"
        echo "Using first corpus file as patch1, fuzzing patch2"
        PATCH1=$(ls "$CORPUS_DIR" | head -1)
        afl-fuzz \
            -i "$CORPUS_DIR" \
            -o "$RESULTS_DIR/$TOOL" \
            -x "$DICT_FILE" \
            -t 5000 \
            -m none \
            -n \
            -- "$PROJECT_DIR/src/interdiff" "$CORPUS_DIR/$PATCH1" @@
        ;;
    rediff)
        # Test rediff (patch correction)
        afl-fuzz \
            -i "$CORPUS_DIR" \
            -o "$RESULTS_DIR/$TOOL" \
            -x "$DICT_FILE" \
            -t 5000 \
            -m none \
            -n \
            -- "$PROJECT_DIR/src/rediff"
        ;;
    grepdiff)
        # Test grepdiff with a simple pattern
        afl-fuzz \
            -i "$CORPUS_DIR" \
            -o "$RESULTS_DIR/$TOOL" \
            -x "$DICT_FILE" \
            -t 5000 \
            -m none \
            -n \
            -- "$PROJECT_DIR/src/grepdiff" ".*"
        ;;
    lsdiff)
        # Test lsdiff (list files in patch)
        afl-fuzz \
            -i "$CORPUS_DIR" \
            -o "$RESULTS_DIR/$TOOL" \
            -x "$DICT_FILE" \
            -t 5000 \
            -m none \
            -n \
            -- "$PROJECT_DIR/src/lsdiff"
        ;;
    *)
        echo "Unknown tool: $TOOL"
        echo "Supported tools: filterdiff, interdiff, rediff, grepdiff, lsdiff"
        exit 1
        ;;
esac
