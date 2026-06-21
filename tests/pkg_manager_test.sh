#!/bin/bash
set -e

# Setup directories
TEST_DIR="/tmp/varian_pkg_test"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"

DEP_B_DIR="$TEST_DIR/dep_b"
DEP_A_DIR="$TEST_DIR/dep_a"
APP_DIR="$TEST_DIR/app"

echo "=== Setting up mock Git repository for dep_b ==="
mkdir -p "$DEP_B_DIR"
cd "$DEP_B_DIR"
git init
git config user.name "Test"
git config user.email "test@example.com"
echo '[package]' > constellation.toml
echo 'name = "dep_b"' >> constellation.toml
echo 'version = "1.0.0"' >> constellation.toml
echo 'fn hello_b() { return "hello from dep_b" }' > b.vn
git add .
git commit -m "initial commit for dep_b"
git tag v1.0.0

echo "=== Setting up mock Git repository for dep_a (depends on dep_b) ==="
mkdir -p "$DEP_A_DIR"
cd "$DEP_A_DIR"
git init
git config user.name "Test"
git config user.email "test@example.com"
echo '[package]' > constellation.toml
echo 'name = "dep_a"' >> constellation.toml
echo 'version = "2.0.0"' >> constellation.toml
echo '[deps]' >> constellation.toml
echo "dep_b = { git = \"$DEP_B_DIR\", tag = \"v1.0.0\" }" >> constellation.toml
echo 'fn hello_a() { return "hello from dep_a" }' > a.vn
git add .
git commit -m "initial commit for dep_a"
git tag v2.0.0

echo "=== Setting up main App directory ==="
mkdir -p "$APP_DIR"
cd "$APP_DIR"
echo '[package]' > constellation.toml
echo 'name = "main_app"' >> constellation.toml
echo 'version = "0.1.0"' >> constellation.toml
echo '[deps]' >> constellation.toml
echo "dep_a = { git = \"$DEP_A_DIR\", tag = \"v2.0.0\" }" >> constellation.toml

# Copy our built 'vn' binary
cp /root/dev/VarianLang/vn "$APP_DIR/vn"

echo "=== Running 'vn install' ==="
./vn install

echo "=== Verifying dependencies are resolved recursively ==="
if [ ! -d "vn_modules/dep_a" ]; then
    echo "ERROR: dep_a not installed!"
    exit 1
fi
if [ ! -d "vn_modules/dep_b" ]; then
    echo "ERROR: transitive dependency dep_b not installed!"
    exit 1
fi
echo "OK: Transitive dependencies successfully resolved and extracted."

echo "=== Verifying lockfile structure ==="
if [ ! -f "constellation.lock" ]; then
    echo "ERROR: constellation.lock not generated!"
    exit 1
fi

grep -q "name = \"dep_a\"" constellation.lock
grep -q "name = \"dep_b\"" constellation.lock
echo "OK: lockfile records both direct and transitive dependencies."

echo "=== Running 'vn remove dep_a' ==="
./vn remove dep_a

echo "=== Verifying pruning and removal ==="
if [ -d "vn_modules/dep_a" ] || [ -d "vn_modules/dep_b" ]; then
    echo "ERROR: dep_a or dep_b still present in vn_modules after remove!"
    exit 1
fi

if grep -q "dep_a" constellation.toml; then
    echo "ERROR: dep_a still in constellation.toml!"
    exit 1
fi

# Check that lockfile is now empty or has no entries
if grep -q "dep_a" constellation.lock || grep -q "dep_b" constellation.lock; then
    echo "ERROR: lockfile still references dep_a/dep_b!"
    exit 1
fi
echo "OK: 'vn remove' pruned dependencies and updated lockfile."

# Add dep_a back
echo "=== Restoring dep_a to test update ==="
echo '[package]' > constellation.toml
echo 'name = "main_app"' >> constellation.toml
echo 'version = "0.1.0"' >> constellation.toml
echo '[deps]' >> constellation.toml
echo "dep_a = { git = \"$DEP_A_DIR\", tag = \"v2.0.0\" }" >> constellation.toml

./vn install

# Update dep_a git tag v2.0.0 to new commit
cd "$DEP_A_DIR"
echo 'fn hello_a_v2() { return "hello updated" }' >> a.vn
git add .
git commit -m "update code"
git tag -f v2.0.0

cd "$APP_DIR"
echo "=== Running 'vn update' ==="
./vn update

# Check that updated code is fetched
if ! grep -q "hello_a_v2" vn_modules/dep_a/a.vn; then
    echo "ERROR: 'vn update' did not fetch the updated code!"
    exit 1
fi
echo "OK: 'vn update' successfully re-resolved latest git commit."

echo "=== ALL C2 INTEGRATION TESTS PASSED ==="
rm -rf "$TEST_DIR"
