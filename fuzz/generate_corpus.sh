#!/bin/bash
# Generate seed corpus for fuzzing patchutils from existing test cases

set -e

CORPUS_DIR="$(dirname "$0")/corpus"
TESTS_DIR="$(dirname "$0")/../tests"
STRESS_RESULTS_DIR="$(dirname "$0")/../stress-test-results"

echo "Generating seed corpus for patchutils fuzzing..."

# Clean and create corpus directory
rm -rf "$CORPUS_DIR"
mkdir -p "$CORPUS_DIR"

# Counter for naming files
counter=0

# Function to add a file to corpus with a unique name
add_to_corpus() {
    local file="$1"
    local name="$2"
    if [ -f "$file" ] && [ -s "$file" ]; then
        cp "$file" "$CORPUS_DIR/seed_$(printf "%04d" $counter)_${name}"
        counter=$((counter + 1))
        echo "Added: $name"
    fi
}

echo "1. Collecting patch files from test cases..."

# Collect .patch and .diff files from tests
find "$TESTS_DIR" -name "*.patch" -o -name "*.diff" | while read -r file; do
    basename_file=$(basename "$file")
    test_name=$(basename "$(dirname "$file")")
    add_to_corpus "$file" "${test_name}_${basename_file}"
done

echo "2. Collecting patches from stress test results..."

# Collect diff files from stress testing if available
if [ -d "$STRESS_RESULTS_DIR" ]; then
    find "$STRESS_RESULTS_DIR" -name "*.diff" | head -20 | while read -r file; do
        basename_file=$(basename "$file")
        add_to_corpus "$file" "stress_${basename_file}"
    done
fi

echo "3. Generating git diffs from repository history..."

# Generate some git diffs from the repository itself
cd "$(dirname "$0")/.."
git log --oneline -50 | while read -r commit rest; do
    if git show --format="" "$commit" > "/tmp/git_diff_$commit.patch" 2>/dev/null; then
        if [ -s "/tmp/git_diff_$commit.patch" ]; then
            add_to_corpus "/tmp/git_diff_$commit.patch" "git_${commit}.patch"
        fi
        rm -f "/tmp/git_diff_$commit.patch"
    fi
done

echo "4. Creating minimal test cases..."

# Create some minimal valid patches
cat > "$CORPUS_DIR/seed_minimal_unified.patch" << 'EOF'
--- a/test.txt	2024-01-01 00:00:00.000000000 +0000
+++ b/test.txt	2024-01-01 00:00:01.000000000 +0000
@@ -1,3 +1,3 @@
 line1
-line2
+modified line2
 line3
EOF

cat > "$CORPUS_DIR/seed_minimal_context.patch" << 'EOF'
*** a/test.txt	2024-01-01 00:00:00.000000000 +0000
--- b/test.txt	2024-01-01 00:00:01.000000000 +0000
***************
*** 1,3 ****
  line1
! line2
  line3
--- 1,3 ----
  line1
! modified line2
  line3
EOF

cat > "$CORPUS_DIR/seed_minimal_git.patch" << 'EOF'
diff --git a/test.txt b/test.txt
index 1234567..abcdefg 100644
--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,3 @@
 line1
-line2
+modified line2
 line3
EOF

# Create some edge cases
cat > "$CORPUS_DIR/seed_empty.patch" << 'EOF'
EOF

cat > "$CORPUS_DIR/seed_binary.patch" << 'EOF'
diff --git a/binary.bin b/binary.bin
index 1234567..abcdefg 100644
GIT binary patch
delta 10
Rcmcc61;2c{000000000000

EOF

cat > "$CORPUS_DIR/seed_new_file.patch" << 'EOF'
diff --git a/newfile.txt b/newfile.txt
new file mode 100644
index 0000000..1234567
--- /dev/null
+++ b/newfile.txt
@@ -0,0 +1,3 @@
+new line 1
+new line 2
+new line 3
EOF

cat > "$CORPUS_DIR/seed_deleted_file.patch" << 'EOF'
diff --git a/oldfile.txt b/oldfile.txt
deleted file mode 100644
index 1234567..0000000
--- a/oldfile.txt
+++ /dev/null
@@ -1,3 +0,0 @@
-old line 1
-old line 2
-old line 3
EOF

echo "5. Creating malformed/edge case patches for robustness testing..."

# Some malformed patches to test error handling
cat > "$CORPUS_DIR/seed_malformed_header.patch" << 'EOF'
---
+++ b/test.txt
@@ -1,3 +1,3 @@
 line1
-line2
+modified line2
 line3
EOF

cat > "$CORPUS_DIR/seed_malformed_hunk.patch" << 'EOF'
--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,999 @@
 line1
-line2
+modified line2
 line3
EOF

# Count final corpus
corpus_count=$(ls -1 "$CORPUS_DIR" | wc -l)
echo "Generated $corpus_count seed files in $CORPUS_DIR"
echo "Corpus generation complete!"
