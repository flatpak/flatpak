if type -q flatpak
    # Fast-path: skip spawning `flatpak --installations` when the canonical
    # user installation's exports/share is already present in $XDG_DATA_DIRS.
    set -x --path XDG_DATA_DIRS $XDG_DATA_DIRS

    set -l __flatpak_xdg_data_home $HOME/.local/share
    set -q XDG_DATA_HOME; and set __flatpak_xdg_data_home $XDG_DATA_HOME

    if not contains -- "$__flatpak_xdg_data_home/flatpak/exports/share" $XDG_DATA_DIRS
        set -q XDG_DATA_DIRS[1]; or set XDG_DATA_DIRS /usr/local/share /usr/share
        set -q XDG_DATA_HOME; or set -l XDG_DATA_HOME $HOME/.local/share

        set -l installations $XDG_DATA_HOME/flatpak
        begin
            set -le G_MESSAGES_DEBUG
            set -lx GIO_USE_VFS local
            set installations $installations (flatpak --installations)
        end

        for dir in {$installations[-1..1]}/exports/share
            if not contains $dir $XDG_DATA_DIRS
                set -p XDG_DATA_DIRS $dir
            end
        end
    end
end
