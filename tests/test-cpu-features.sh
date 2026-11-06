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

done_testing
