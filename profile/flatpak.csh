set _flatpak=`where flatpak | head -n 1`
if ( ${%_flatpak} > 0 ) then
    if ( ! ${?XDG_DATA_HOME} ) setenv XDG_DATA_HOME "$HOME/.local/share"
    if ( ${%XDG_DATA_HOME} == 0 ) setenv XDG_DATA_HOME "$HOME/.local/share"
    if ( ! ${?XDG_DATA_DIRS} ) setenv XDG_DATA_DIRS /usr/local/share:/usr/share
    if ( ${%XDG_DATA_DIRS} == 0 ) setenv XDG_DATA_DIRS /usr/local/share:/usr/share
    set _new_dirs=""
    foreach _line (`(unset G_MESSAGES_DEBUG; echo "${XDG_DATA_HOME}"/flatpak; setenv GIO_USE_VFS local; flatpak --installations)`)
        set _line=${_line}/exports/share
	if ( ":${XDG_DATA_DIRS}:" =~ *:${_line}:* ) continue
	if ( ":${XDG_DATA_DIRS}:" =~ *:${_line}/:* ) continue
	if ( ${%_new_dirs} > 0 ) set _new_dirs="${_new_dirs}:"
	set _new_dirs="${_new_dirs}${_line}"
    end
    if ( ${%_new_dirs} > 0 ) then
	set _new_dirs="${_new_dirs}:"
	setenv XDG_DATA_DIRS "${_new_dirs}${XDG_DATA_DIRS}"
    endif
endif
unset _flatpak _line _new_dirs
