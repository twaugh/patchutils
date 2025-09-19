#!/bin/bash
# Analyze crashes found by AFL++ fuzzing

set -e

SCRIPT_DIR="$(dirname "$0")"
RESULTS_DIR="$SCRIPT_DIR/results"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Allow overriding binary directory (for instrumented builds)
FUZZ_BINDIR="${FUZZ_BINDIR:-$PROJECT_DIR/src}"

echo "Analyzing AFL++ fuzzing results..."

if [ ! -d "$RESULTS_DIR" ]; then
    echo "No results directory found. Run fuzzing first with ./run_fuzz.sh"
    exit 1
fi

# Function to analyze crashes for a specific tool
analyze_tool_crashes() {
    local tool="$1"
    local tool_results="$RESULTS_DIR/$tool"

    if [ ! -d "$tool_results" ]; then
        echo "No results found for $tool"
        return
    fi

    echo ""
    echo "=== Analysis for $tool ==="

    # Check for crashes
    local crashes_dir="$tool_results/crashes"
    if [ -d "$crashes_dir" ] && [ "$(ls -A "$crashes_dir" 2>/dev/null | wc -l)" -gt 0 ]; then
        local crash_count=$(ls -1 "$crashes_dir" | grep -v README | wc -l)
        echo "Found $crash_count crash(es):"

        for crash_file in "$crashes_dir"/*; do
            if [ -f "$crash_file" ] && [ "$(basename "$crash_file")" != "README.txt" ]; then
                echo "  - $(basename "$crash_file")"

                # Try to reproduce the crash
                echo "    Reproducing crash..."
                # Use instrumented binary if available
                local binary_path="$FUZZ_BINDIR/$tool"
                if [ -f "$FUZZ_BINDIR/fuzz-$tool" ]; then
                    binary_path="$FUZZ_BINDIR/fuzz-$tool"
                fi
                if timeout 10s "$binary_path" < "$crash_file" >/dev/null 2>&1; then
                    echo "    ⚠ Crash not reproduced (may be timing-dependent)"
                else
                    local exit_code=$?
                    echo "    ✓ Crash reproduced (exit code: $exit_code)"

                    # Get more details with gdb if available
                    if command -v gdb >/dev/null 2>&1; then
                        echo "    Getting stack trace..."
                        timeout 30s gdb -batch -ex run -ex bt -ex quit \
                            --args "$binary_path" < "$crash_file" 2>/dev/null | \
                            grep -A 20 "Program received signal" | head -20 || true
                    fi
                fi
                echo ""
            fi
        done
    else
        echo "No crashes found"
    fi

    # Check for hangs
    local hangs_dir="$tool_results/hangs"
    if [ -d "$hangs_dir" ] && [ "$(ls -A "$hangs_dir" 2>/dev/null | wc -l)" -gt 0 ]; then
        local hang_count=$(ls -1 "$hangs_dir" | grep -v README | wc -l)
        echo "Found $hang_count hang(s):"

        for hang_file in "$hangs_dir"/*; do
            if [ -f "$hang_file" ] && [ "$(basename "$hang_file")" != "README.txt" ]; then
                echo "  - $(basename "$hang_file")"

                # Check file size to understand the hang
                local size=$(stat -c%s "$hang_file")
                echo "    File size: $size bytes"

                if [ "$size" -gt 1000000 ]; then
                    echo "    ⚠ Large input file - may cause memory exhaustion"
                else
                    echo "    Testing with timeout..."
                    # Use instrumented binary if available
                    local binary_path="$FUZZ_BINDIR/$tool"
                    if [ -f "$FUZZ_BINDIR/fuzz-$tool" ]; then
                        binary_path="$FUZZ_BINDIR/fuzz-$tool"
                    fi
                    if timeout 5s "$binary_path" < "$hang_file" >/dev/null 2>&1; then
                        echo "    ✓ Completed within timeout"
                    else
                        echo "    ⚠ Still hangs or crashes"
                    fi
                fi
                echo ""
            fi
        done
    else
        echo "No hangs found"
    fi

    # Show fuzzer stats if available
    local stats_file="$tool_results/fuzzer_stats"
    if [ -f "$stats_file" ]; then
        echo "Fuzzer statistics:"
        echo "  Total execs: $(grep "execs_done" "$stats_file" | cut -d: -f2 | tr -d ' ')"
        echo "  Crashes: $(grep "unique_crashes" "$stats_file" | cut -d: -f2 | tr -d ' ')"
        echo "  Hangs: $(grep "unique_hangs" "$stats_file" | cut -d: -f2 | tr -d ' ')"
        echo "  Coverage: $(grep "bitmap_cvg" "$stats_file" | cut -d: -f2 | tr -d ' ')%"
    fi
}

# Analyze all tools that have results
for tool_dir in "$RESULTS_DIR"/*; do
    if [ -d "$tool_dir" ]; then
        tool_name=$(basename "$tool_dir")
        analyze_tool_crashes "$tool_name"
    fi
done

echo ""
echo "=== Summary ==="
echo "To minimize a crash case:"
echo "  afl-tmin -i path/to/crash -o minimized_crash -- /path/to/tool @@"
echo ""
echo "To get more detailed crash info:"
echo "  gdb --args /path/to/tool"
echo "  (gdb) run < path/to/crash"
echo "  (gdb) bt"
