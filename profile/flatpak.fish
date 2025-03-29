if type -q flatpak
    # Set XDG_DATA_DIRS and PATH to include Flatpak installations

    set -x --path XDG_DATA_DIRS $XDG_DATA_DIRS
    set -x --path PATH $PATH

    set -q XDG_DATA_DIRS[1]; or set XDG_DATA_DIRS /usr/local/share /usr/share
    set -q XDG_DATA_HOME; or set -l XDG_DATA_HOME $HOME/.local/share
    set -q PATH[1]; or set PATH /usr/local/bin /usr/bin

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

    for dir in {$installations[-1..1]}/exports/bin
        if not contains $dir $PATH
            set -p PATH $dir
        end
    end
end
