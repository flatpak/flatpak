if type -q flatpak
    # Set XDG_DATA_DIRS to include Flatpak installations

    set -x --path XDG_DATA_DIRS $XDG_DATA_DIRS

    set -q XDG_DATA_DIRS[1]; or set XDG_DATA_DIRS /usr/local/share /usr/share
    set -q XDG_DATA_HOME; or set -l XDG_DATA_HOME $HOME/.local/share

    set -l installations $XDG_DATA_HOME/flatpak
    begin
        set -le G_MESSAGES_DEBUG
        set -lx GIO_USE_VFS local
        set installations $installations (flatpak --installations)
    end

    set XDG_DATA_DIRS {$installations}/exports/share $XDG_DATA_DIRS
end
