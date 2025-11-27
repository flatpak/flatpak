Flatpak release checklist
=========================

* Update NEWS
* Check that version number in `meson.build` is correct
* Update release date in `NEWS`
* Commit the changes
* `ninja -C ${builddir} flatpak-update-po`
    * This will update `po/`; commit those changes too
* `git evtag sign $VERSION`
* `git push --atomic origin main $VERSION`
* Ensure the release.yml workflow succeeds

After the release:

* Update version number in `meson.build` to the next release version
* Start a new section in `NEWS`

After creating a stable branch:

* Update version number in `meson.build` to the next unstable release version
* Update the `NEWS` section header
