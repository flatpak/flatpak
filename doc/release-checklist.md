Flatpak release checklist
=========================

* Update NEWS
* Check that version number in `meson.build` is correct
* Update release date in `NEWS`
* Commit the changes
* `ninja -C ${builddir} flatpak-update-po`
    * This will update `po/`; commit those changes too
* `meson dist -C ${builddir}`
* Do any final smoke-testing, e.g. update a package, install and test it
* `git evtag sign $VERSION`
* `git push --atomic origin main $VERSION`
* https://github.com/flatpak/flatpak/releases/new
    * Fill in the new version's tag in the "Tag version" box
    * Title: `$VERSION`
    * Copy the `NEWS` text into the description
    * Check "This is a pre-release" for 1.odd.x development releases
    * Upload the tarball that you built with `meson dist`
    * Get the `sha256sum` of the tarball and append it to the description
    * `Publish release`

After the release:

* Update version number in `meson.build` to the next release version
* Start a new section in `NEWS`

After creating a stable branch:

* Update version number in `meson.build` to the next unstable release version
* Update the `NEWS` section header
