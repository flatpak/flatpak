function __fish_complete_flatpak
    set current_cmd (commandline -p)
    set current_position (commandline -C)
    set current_token (commandline -ct)
    echo "CMD \"$current_cmd\" POS \"$current_position\" TOK \"$current_token\"" >> /tmp/fish-flatpak-debug.txt
    command flatpak complete "$current_cmd" "$current_position" "$current_token" | while read fp_sugg
        echo "SUG \"$fp_sugg\"" >> /tmp/fish-flatpak-debug.txt
        set sugg (string trim -- "$fp_sugg")
        switch "$sugg"
            case __FLATPAK_FILE
                __fish_complete_path "$current_token"
            case __FLATPAK_BUNDLE_FILE
                __fish_complete_suffix "$current_token" '.flatpak'
            case __FLATPAK_BUNDLE_OR_REF_FILE
                __fish_complete_suffix "$current_token" '.flatpak'
                __fish_complete_suffix "$current_token" '.flatpakref'
            case __FLATPAK_DIR
                __fish_complete_directories "$current_token"
            case '*'
                # completing a value for option
                if string match -- "--*=" "$current_token"
                    echo "$current_token$sugg"
                else
                    echo "$sugg"
                end
        end
    end
    return
end

function __fish_flatpak_complete_files
    if __fish_seen_subcommand_from run build
        set pos_args 0
        for t in (commandline -co)
            if string match --invert -- "-*" "$t"
                set pos_args (math "$pos_args+1")
            end
        end
        if test $pos_args -gt 2
            return 0
        end
    end
    return 1
end

complete -c flatpak -f -n "not __fish_flatpak_complete_files" -a '(__fish_complete_flatpak)'
complete -c flatpak -n "__fish_flatpak_complete_files" -a '(__fish_complete_flatpak)'
