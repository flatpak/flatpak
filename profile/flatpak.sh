if command -v flatpak > /dev/null; then
    # set XDG_DATA_DIRS and PATH to include Flatpak installations

    installation_dirs=$(
         unset G_MESSAGES_DEBUG
         echo "${XDG_DATA_HOME:-"$HOME/.local/share"}/flatpak"
         GIO_USE_VFS=local flatpak --installations
    )

    new_share_dirs=$(echo "$installation_dirs" |
	(
            new_share_dirs=
            while read -r install_path
            do
                share_path=$install_path/exports/share
                case ":$XDG_DATA_DIRS:" in
                    (*":$share_path:"*) :;;
                    (*":$share_path/:"*) :;;
                    (*) new_share_dirs=${new_share_dirs:+${new_share_dirs}:}$share_path;;
                esac
            done
            echo "$new_share_dirs"
        )
    )

    export XDG_DATA_DIRS
    XDG_DATA_DIRS="${new_share_dirs:+${new_share_dirs}:}${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

    unset new_share_dirs

    new_bin_dirs=$(echo "$installation_dirs" |
	(
            new_bin_dirs=
            while read -r install_path
            do
                bin_path=$install_path/exports/bin
                case ":$PATH:" in
                    (*":$bin_path:"*) :;;
                    (*":$bin_path/:"*) :;;
                    (*) new_bin_dirs=${new_bin_dirs:+${new_bin_dirs}:}$bin_path;;
                esac
            done
            echo "$new_bin_dirs"
        )
    )

    export PATH
    PATH="${new_bin_dirs:+${new_bin_dirs}:}${PATH:-/usr/local/bin:/usr/bin}"

    unset new_bin_dirs
fi
