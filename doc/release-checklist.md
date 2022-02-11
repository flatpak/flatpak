Flatpak release checklist
=========================

* Update NEWS
* Update version number in `configure.ac` and release date in `NEWS`
* Commit the changes
* `make distcheck`
    * This will update `po/`; commit those changes too
* Do any final smoke-testing, e.g. update a package, install and test it
* `git evtag sign $VERSION`
* `git push --atomic origin main $VERSION`
* https://github.com/flatpak/flatpak/releases/new
    * Fill in the new version's tag in the "Tag version" box
    * Title: `Release $VERSION`
    * Copy the `NEWS` text into the description
    * Check "This is a pre-release" for 1.odd.x development releases
    * Upload the tarball that you built with `make distcheck`
    * Get the `sha256sum` of the tarball and append it to the description
    * `Publish release`
* Send an announcement to the mailing list
