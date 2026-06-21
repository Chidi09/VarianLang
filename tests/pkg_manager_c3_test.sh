#!/bin/bash
set -e

# Setup directories
TEST_DIR="/tmp/varian_pkg_c3_test"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"

DEP_B_DIR="$TEST_DIR/dep_b"
DEP_A_DIR="$TEST_DIR/dep_a"
APP_DIR="$TEST_DIR/app"

# Helper function to compute tarball sha256
get_git_tarball_sha256() {
    local dir="$1"
    local tag="$2"
    local name="$3"
    local clone_tmp="/tmp/c3_tmp_clone"
    local tar_tmp="/tmp/c3_tmp_tarball.tar.gz"
    
    rm -rf "$clone_tmp" "$tar_tmp"
    git clone --branch "$tag" "$dir" "$clone_tmp" 2>/dev/null
    rm -rf "$clone_tmp/.git"
    tar -czf "$tar_tmp" -C "$clone_tmp" .
    sha256sum "$tar_tmp" | cut -d' ' -f1
    rm -rf "$clone_tmp" "$tar_tmp"
}

echo "=== Setting up mock Git repository for dep_b ==="
mkdir -p "$DEP_B_DIR"
cd "$DEP_B_DIR"
git init
git config user.name "Test"
git config user.email "test@example.com"
echo '[package]' > constellation.toml
echo 'name = "dep_b"' >> constellation.toml
echo 'version = "0.1.0"' >> constellation.toml
echo 'fn hello_b() { return "hello from dep_b v0.1.0" }' > b.vn
git add .
git commit -m "initial commit for dep_b"
git tag v0.1.0

# Compute sha256 for dep_b
sha_dep_b=$(get_git_tarball_sha256 "$DEP_B_DIR" "v0.1.0" "dep_b")

echo "=== Setting up mock Git repository for dep_a ==="
mkdir -p "$DEP_A_DIR"
cd "$DEP_A_DIR"
git init
git config user.name "Test"
git config user.email "test@example.com"
echo '[package]' > constellation.toml
echo 'name = "dep_a"' >> constellation.toml
echo 'version = "1.0.0"' >> constellation.toml
echo 'fn hello_a() { return "hello from dep_a v1.0.0" }' > a.vn
git add .
git commit -m "version 1.0.0"
git tag v1.0.0

sha_dep_a_100=$(get_git_tarball_sha256 "$DEP_A_DIR" "v1.0.0" "dep_a")

# Version 1.2.0
echo 'fn hello_a_120() { return "hello from dep_a v1.2.0" }' >> a.vn
git add .
git commit -m "version 1.2.0"
git tag v1.2.0

sha_dep_a_120=$(get_git_tarball_sha256 "$DEP_A_DIR" "v1.2.0" "dep_a")

# Version 2.0.0 (incompatible under ^1.0.0)
echo 'fn hello_a_200() { return "hello from dep_a v2.0.0" }' >> a.vn
git add .
git commit -m "version 2.0.0"
git tag v2.0.0

sha_dep_a_200=$(get_git_tarball_sha256 "$DEP_A_DIR" "v2.0.0" "dep_a")

echo "=== Creating mock index.json ==="
INDEX_FILE="$TEST_DIR/index.json"
cat <<EOF > "$INDEX_FILE"
{
  "dep_a": {
    "versions": {
      "1.0.0": {
        "git": "$DEP_A_DIR",
        "tag": "v1.0.0",
        "sha256": "$sha_dep_a_100"
      },
      "1.2.0": {
        "git": "$DEP_A_DIR",
        "tag": "v1.2.0",
        "sha256": "$sha_dep_a_120"
      },
      "2.0.0": {
        "git": "$DEP_A_DIR",
        "tag": "v2.0.0",
        "sha256": "$sha_dep_a_200"
      }
    }
  },
  "dep_b": {
    "versions": {
      "0.1.0": {
        "git": "$DEP_B_DIR",
        "tag": "v0.1.0",
        "sha256": "$sha_dep_b"
      }
    }
  }
}
EOF

export CONSTELLATION_INDEX_URL="$INDEX_FILE"

echo "=== Setting up main App directory ==="
mkdir -p "$APP_DIR"
cd "$APP_DIR"
echo '[package]' > constellation.toml
echo 'name = "main_app"' >> constellation.toml
echo 'version = "0.1.0"' >> constellation.toml
echo '[deps]' >> constellation.toml
# Should resolve to 1.2.0 (compatible with 1.x but not 2.x)
echo 'dep_a = "^1.0.0"' >> constellation.toml

# Copy our built 'vn' binary
cp /root/dev/VarianLang/vn "$APP_DIR/vn"

echo "=== Running 'vn search dep' ==="
./vn search dep

echo "=== Running 'vn install' ==="
./vn install

echo "=== Verifying best matching version was resolved and installed ==="
if [ ! -d "vn_modules/dep_a" ]; then
    echo "ERROR: dep_a not installed!"
    exit 1
fi

if ! grep -q "hello_a_120" vn_modules/dep_a/a.vn; then
    echo "ERROR: installed version is not v1.2.0!"
    exit 1
fi
if grep -q "hello_a_200" vn_modules/dep_a/a.vn; then
    echo "ERROR: installed version is v2.0.0 (major compatibility violation)!"
    exit 1
fi
echo "OK: Resolved to compatible version 1.2.0 correctly."

echo "=== Verifying lockfile structure contains resolved fields ==="
if [ ! -f "constellation.lock" ]; then
    echo "ERROR: constellation.lock not generated!"
    exit 1
fi
grep -q "version = \"v1.2.0\"" constellation.lock
grep -q "sha256 = \"$sha_dep_a_120\"" constellation.lock
echo "OK: lockfile matches resolved tag and integrity hash."

echo "=== ALL C3 INTEGRATION TESTS PASSED ==="
rm -rf "$TEST_DIR"
