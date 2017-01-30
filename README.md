
<p align="center">
  <img src="https://github.com/flatpak/flatpak/blob/master/flatpak.png?raw=true" alt="Flatpak icon"/>
</p>

Flatpak is a system for building, distributing and running sandboxed
desktop applications on Linux.

See http://flatpak.org/ for more information.

Read documentation for the flatpak [commandline tools](http://flatpak.github.io/flatpak/flatpak-docs.html) and for the libflatpak [library API](http://flatpak.github.io/flatpak/reference/html/index.html).

If you're going to build Flatpak from this source repo, the recommended way to build the subfolder projects (currently they are `bubblewrap` and `libglnx`) is just to build Flatpak itself.  The Flatpak build will automatically build the subfolder projects, whereas building them separately first can cause problems.  

You can download the source for Flatpak and for the subfolder projects in one go by using:  
`git clone --recursive` (followed by the Flatpak source repo's Git clone URL).  
