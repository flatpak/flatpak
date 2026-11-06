#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_bwrap
skip_revokefs_without_fuse
setup_repo

if [ "${USE_COLLECTIONS_IN_SERVER-}" = yes ]; then
    CPU_COLLECTION_ID=org.test.Collection.test
else
    CPU_COLLECTION_ID=""
fi

REQUIRED_CPU_FEATURES="flatpak-test-missing-feature;" GPGARGS="${FL_GPGARGS}" \
    $(dirname $0)/make-test-app.sh repos/test org.test.CpuFeatures master "${CPU_COLLECTION_ID}" > /dev/null
update_repo

if ${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures &> install-error-log; then
    assert_not_reached "Should not install with missing CPU features"
fi
assert_file_has_content install-error-log "requires CPU feature flatpak-test-missing-feature"
assert_not_has_dir "$FL_DIR/app/org.test.CpuFeatures/current/active"

# The first requirement check must persist a complete, sorted text snapshot.
# There is one file for the ABI of the test binary, shared across invocations.
CPU_CACHE_FILES=( "$XDG_CACHE_HOME/flatpak"/cpu-features-*.txt )
assert_streq "${#CPU_CACHE_FILES[@]}" "1"
CPU_CACHE=${CPU_CACHE_FILES[0]}
assert_has_file "$CPU_CACHE"
LC_ALL=C sort -c "$CPU_CACHE"


${FLATPAK} build-bundle repos/test --repo-url="http://127.0.0.1:${port}/test" \
    --gpg-keys="${FL_GPG_HOMEDIR}/pubring.gpg" cpu-features.flatpak org.test.CpuFeatures master >&2
if ${FLATPAK} ${U} install -y ./cpu-features.flatpak &> install-error-log; then
    assert_not_reached "Should not install bundle with missing CPU features"
fi
assert_file_has_content install-error-log "requires CPU feature flatpak-test-missing-feature"

${FLATPAK} ${U} config --set check-cpu-features false
${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures >&2
${FLATPAK} ${U} config --unset check-cpu-features
CPU_OLD_COMMIT=$(${FLATPAK} ${U} info --show-commit org.test.CpuFeatures)

REQUIRED_CPU_FEATURES="cpuinfo:flatpak-test-future-capabilities:missing;" GPGARGS="${FL_GPGARGS}" \
    $(dirname $0)/make-test-app.sh repos/test org.test.CpuFeatures master "${CPU_COLLECTION_ID}" > /dev/null
update_repo
if ${FLATPAK} ${U} update -y org.test.CpuFeatures &> install-error-log; then
    assert_not_reached "Should not update with missing CPU features"
fi
assert_file_has_content install-error-log "requires CPU feature cpuinfo:flatpak-test-future-capabilities:missing"
assert_streq "$CPU_OLD_COMMIT" "$(${FLATPAK} ${U} info --show-commit org.test.CpuFeatures)"

${FLATPAK} ${U} config --set check-cpu-features false
${FLATPAK} ${U} update -y org.test.CpuFeatures >&2
${FLATPAK} ${U} uninstall -y org.test.CpuFeatures >&2
${FLATPAK} ${U} install -y ./cpu-features.flatpak >&2
${FLATPAK} ${U} config --unset check-cpu-features
${FLATPAK} ${U} uninstall -y --all >&2

ok "handles CPU feature requirements and override"

for FEATURE in 'cpuinfo:flatpak-test-future-capabilities=missing' 'cpuinfo:flatpak-test-future-register#0' 'hwcap:999' 'hwcap2:999' 'hwcap3:999' 'hwcap4:999'; do
    REQUIRED_CPU_FEATURES="$FEATURE;" GPGARGS="${FL_GPGARGS}" \
        $(dirname $0)/make-test-app.sh repos/test org.test.CpuFeatures master "${CPU_COLLECTION_ID}" > /dev/null
    update_repo
    if ${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures &> install-error-log; then
        assert_not_reached "Should not install with unsupported CPU requirement $FEATURE"
    fi
    assert_file_has_content install-error-log "requires CPU feature $FEATURE"
done

ok "qualified CPU-info and all HWCAP requirement forms are enforced"

# Substitute a capability absent from the real CPU. Separate install processes
# (and the deployment helper) must honor the file as authoritative indefinitely.
printf '%s\n' 'cpuinfo:flatpak test cached field=value with spaces' 'cpuinfo:flatpak test cached register#3' flatpak-test-cached-feature > cpu-cache-expected
cp cpu-cache-expected "$CPU_CACHE"
touch -t 200001010000 "$CPU_CACHE"
REQUIRED_CPU_FEATURES="flatpak-test-cached-feature;cpuinfo:flatpak test cached field=value with spaces;cpuinfo:flatpak test cached register#3;" GPGARGS="${FL_GPGARGS}" \
    $(dirname $0)/make-test-app.sh repos/test org.test.CpuFeatures master "${CPU_COLLECTION_ID}" > /dev/null
update_repo
${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures >&2
cmp cpu-cache-expected "$CPU_CACHE"
${FLATPAK} ${U} uninstall -y org.test.CpuFeatures >&2

# An empty cache is a valid persistent empty set, not a request to regenerate.
: > "$CPU_CACHE"
if ${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures &> install-error-log; then
    assert_not_reached "An empty cache should not provide a CPU feature"
fi
assert_file_has_content install-error-log "requires CPU feature flatpak-test-cached-feature"
assert_streq "$(wc -c < "$CPU_CACHE" | tr -d ' ')" "0"

# Deletion is the only explicit refresh operation: detection must then recreate
# a real snapshot, which cannot contain the invented cached capability.
rm "$CPU_CACHE"
if ${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures &> install-error-log; then
    assert_not_reached "Deleting the CPU cache should trigger fresh discovery"
fi
assert_file_has_content install-error-log "requires CPU feature flatpak-test-cached-feature"
assert_has_file "$CPU_CACHE"
assert_not_file_has_content "$CPU_CACHE" "^flatpak-test-cached-feature$"
LC_ALL=C sort -c "$CPU_CACHE"

ok "CPU features persist across processes without invalidation until the cache is deleted"

# Cache persistence must not turn a writable-directory failure into a new
# installation requirement. A directory at the CPU cache filename blocks its
# replacement, while unrelated system-installation caches remain writable.
mkdir -p "cpu-cache-unwritable/flatpak/${CPU_CACHE##*/}"
if XDG_CACHE_HOME="$PWD/cpu-cache-unwritable" ${FLATPAK} ${U} install -y test-repo org.test.CpuFeatures &> install-error-log; then
    assert_not_reached "Live detection should still reject the invented feature"
fi
assert_file_has_content install-error-log "requires CPU feature flatpak-test-cached-feature"

ok "unwritable CPU feature caches fall back to live checks"

done_testing
