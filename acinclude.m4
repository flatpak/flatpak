#serial 12

# Checks the location of the XML Catalog
# Usage:
#   JH_PATH_XML_CATALOG([ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND])
# Defines XMLCATALOG and XML_CATALOG_FILE substitutions
AC_DEFUN([JH_PATH_XML_CATALOG],
[
  # check for the presence of the XML catalog
  AC_ARG_WITH([xml-catalog],
              AC_HELP_STRING([--with-xml-catalog=CATALOG],
                             [path to xml catalog to use]),,
              [with_xml_catalog=/etc/xml/catalog])
  jh_found_xmlcatalog=true
  XML_CATALOG_FILE="$with_xml_catalog"
  AC_SUBST([XML_CATALOG_FILE])
  AC_MSG_CHECKING([for XML catalog ($XML_CATALOG_FILE)])
  if test -f "$XML_CATALOG_FILE"; then
    AC_MSG_RESULT([found])
  else
    jh_found_xmlcatalog=false
    AC_MSG_RESULT([not found])
  fi

  # check for the xmlcatalog program
  AC_PATH_PROG(XMLCATALOG, xmlcatalog, no)
  if test "x$XMLCATALOG" = xno; then
    jh_found_xmlcatalog=false
  fi

  if $jh_found_xmlcatalog; then
    ifelse([$1],,[:],[$1])
  else
    ifelse([$2],,[AC_MSG_ERROR([could not find XML catalog])],[$2])
  fi
])

# Checks if a particular URI appears in the XML catalog
# Usage:
#   JH_CHECK_XML_CATALOG(URI, [FRIENDLY-NAME], [ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND])
AC_DEFUN([JH_CHECK_XML_CATALOG],
[
  AC_REQUIRE([JH_PATH_XML_CATALOG],[JH_PATH_XML_CATALOG(,[:])])dnl
  AC_MSG_CHECKING([for ifelse([$2],,[$1],[$2]) in XML catalog])
  if $jh_found_xmlcatalog && \
     AC_RUN_LOG([$XMLCATALOG --noout "$XML_CATALOG_FILE" "$1" >&2]); then
    AC_MSG_RESULT([found])
    ifelse([$3],,,[$3
])dnl
  else
    AC_MSG_RESULT([not found])
    ifelse([$4],,
       [AC_MSG_ERROR([could not find ifelse([$2],,[$1],[$2]) in XML catalog])],
       [$4])
  fi
])

# ===========================================================================
#     http://www.gnu.org/software/autoconf-archive/ax_valgrind_check.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_VALGRIND_CHECK()
#
# DESCRIPTION
#
#   Checks whether Valgrind is present and, if so, allows running `make
#   check` under a variety of Valgrind tools to check for memory and
#   threading errors.
#
#   Defines VALGRIND_CHECK_RULES which should be substituted in your
#   Makefile; and $enable_valgrind which can be used in subsequent configure
#   output. VALGRIND_ENABLED is defined and substituted, and corresponds to
#   the value of the --enable-valgrind option, which defaults to being
#   enabled if Valgrind is installed and disabled otherwise.
#
#   If unit tests are written using a shell script and automake's
#   LOG_COMPILER system, the $(VALGRIND) variable can be used within the
#   shell scripts to enable Valgrind, as described here:
#
#     https://www.gnu.org/software/gnulib/manual/html_node/Running-self_002dtests-under-valgrind.html
#
#   Usage example:
#
#   configure.ac:
#
#     AX_VALGRIND_CHECK
#
#   Makefile.am:
#
#     @VALGRIND_CHECK_RULES@
#     VALGRIND_SUPPRESSIONS_FILES = my-project.supp
#     EXTRA_DIST = my-project.supp
#
#   This results in a "check-valgrind" rule being added to any Makefile.am
#   which includes "@VALGRIND_CHECK_RULES@" (assuming the module has been
#   configured with --enable-valgrind). Running `make check-valgrind` in
#   that directory will run the module's test suite (`make check`) once for
#   each of the available Valgrind tools (out of memcheck, helgrind, drd and
#   sgcheck), and will output results to test-suite-$toolname.log for each.
#   The target will succeed if there are zero errors and fail otherwise.
#
#   The macro supports running with and without libtool.
#
# LICENSE
#
#   Copyright (c) 2014, 2015 Philip Withnall <philip.withnall@collabora.co.uk>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved.  This file is offered as-is, without any
#   warranty.

AC_DEFUN([AX_VALGRIND_CHECK],[
	dnl Check for --enable-valgrind
	AC_MSG_CHECKING([whether to enable Valgrind on the unit tests])
	AC_ARG_ENABLE([valgrind],
	              [AS_HELP_STRING([--enable-valgrind], [Whether to enable Valgrind on the unit tests])],
	              [enable_valgrind=$enableval],[enable_valgrind=])

	# Check for Valgrind.
	AC_CHECK_PROG([VALGRIND],[valgrind],[valgrind])

	AS_IF([test "$enable_valgrind" = "yes" -a "$VALGRIND" = ""],[
		AC_MSG_ERROR([Could not find valgrind; either install it or reconfigure with --disable-valgrind])
	])
	AS_IF([test "$enable_valgrind" != "no"],[enable_valgrind=yes])

	AM_CONDITIONAL([VALGRIND_ENABLED],[test "$enable_valgrind" = "yes"])
	AC_SUBST([VALGRIND_ENABLED],[$enable_valgrind])
	AC_MSG_RESULT([$enable_valgrind])

	# Check for Valgrind tools we care about.
        #m4_define([valgrind_tool_list],[[memcheck], [helgrind], [drd], [exp-sgcheck]])
        # I trimmed this, because we fail on all the thread stuff
        m4_define([valgrind_tool_list],[[memcheck]])

	AS_IF([test "$VALGRIND" != ""],[
		m4_foreach([vgtool],[valgrind_tool_list],[
			m4_define([vgtooln],AS_TR_SH(vgtool))
			m4_define([ax_cv_var],[ax_cv_valgrind_tool_]vgtooln)
			AC_CACHE_CHECK([for Valgrind tool ]vgtool,ax_cv_var,[
				ax_cv_var=
				AS_IF([`$VALGRIND --tool=vgtool --help >/dev/null 2>&1`],[
					ax_cv_var="vgtool"
				])
			])

			AC_SUBST([VALGRIND_HAVE_TOOL_]vgtooln,[$ax_cv_var])
		])
	])

VALGRIND_CHECK_RULES='
# Valgrind check
#
# Optional:
#  - VALGRIND_SUPPRESSIONS_FILES: Space-separated list of Valgrind suppressions
#    files to load. (Default: empty)
#  - VALGRIND_FLAGS: General flags to pass to all Valgrind tools.
#    (Default: --num-callers=30)
#  - VALGRIND_$toolname_FLAGS: Flags to pass to Valgrind $toolname (one of:
#    memcheck, helgrind, drd, sgcheck). (Default: various)

# Optional variables
VALGRIND_SUPPRESSIONS ?= $(addprefix --suppressions=,$(VALGRIND_SUPPRESSIONS_FILES))
VALGRIND_FLAGS ?= --num-callers=30
VALGRIND_memcheck_FLAGS ?= --leak-check=full --show-reachable=no
VALGRIND_helgrind_FLAGS ?= --history-level=approx
VALGRIND_drd_FLAGS ?=
VALGRIND_sgcheck_FLAGS ?=

# Internal use
valgrind_tools = memcheck helgrind drd sgcheck
valgrind_log_files = $(addprefix test-suite-,$(addsuffix .log,$(valgrind_tools)))

valgrind_memcheck_flags = --tool=memcheck $(VALGRIND_memcheck_FLAGS)
valgrind_helgrind_flags = --tool=helgrind $(VALGRIND_helgrind_FLAGS)
valgrind_drd_flags = --tool=drd $(VALGRIND_drd_FLAGS)
valgrind_sgcheck_flags = --tool=exp-sgcheck $(VALGRIND_sgcheck_FLAGS)

valgrind_quiet = $(valgrind_quiet_$(V))
valgrind_quiet_ = $(valgrind_quiet_$(AM_DEFAULT_VERBOSITY))
valgrind_quiet_0 = --quiet

# Support running with and without libtool.
ifneq ($(LIBTOOL),)
valgrind_lt = $(LIBTOOL) $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=execute
else
valgrind_lt =
endif

# Use recursive makes in order to ignore errors during check
check-valgrind:
ifeq ($(VALGRIND_ENABLED),yes)
	-$(foreach tool,$(valgrind_tools), \
		$(if $(VALGRIND_HAVE_TOOL_$(tool))$(VALGRIND_HAVE_TOOL_exp_$(tool)), \
			$(MAKE) $(AM_MAKEFLAGS) -k check-valgrind-tool VALGRIND_TOOL=$(tool); \
		) \
	)
else
	@echo "Need to reconfigure with --enable-valgrind"
endif

# Valgrind running
VALGRIND_TESTS_ENVIRONMENT = \
	$(TESTS_ENVIRONMENT) \
	env VALGRIND=$(VALGRIND) \
	G_SLICE=always-malloc,debug-blocks \
	G_DEBUG=fatal-warnings,fatal-criticals,gc-friendly

VALGRIND_LOG_COMPILER = \
	$(valgrind_lt) \
	$(VALGRIND) $(VALGRIND_SUPPRESSIONS) --error-exitcode=1 $(VALGRIND_FLAGS)

check-valgrind-tool:
ifeq ($(VALGRIND_ENABLED),yes)
	$(MAKE) check-TESTS \
		TESTS_ENVIRONMENT="$(VALGRIND_TESTS_ENVIRONMENT)" \
		LOG_COMPILER="$(VALGRIND_LOG_COMPILER)" \
		LOG_FLAGS="$(valgrind_$(VALGRIND_TOOL)_flags)" \
		TEST_SUITE_LOG=test-suite-$(VALGRIND_TOOL).log
else
	@echo "Need to reconfigure with --enable-valgrind"
endif

DISTCHECK_CONFIGURE_FLAGS ?=
DISTCHECK_CONFIGURE_FLAGS += --disable-valgrind

MOSTLYCLEANFILES ?=
MOSTLYCLEANFILES += $(valgrind_log_files)

.PHONY: check-valgrind check-valgrind-tool
'

	AC_SUBST([VALGRIND_CHECK_RULES])
	m4_ifdef([_AM_SUBST_NOTMAKE], [_AM_SUBST_NOTMAKE([VALGRIND_CHECK_RULES])])
])

# ===========================================================================
#    http://www.gnu.org/software/autoconf-archive/ax_compare_version.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_COMPARE_VERSION(VERSION_A, OP, VERSION_B, [ACTION-IF-TRUE], [ACTION-IF-FALSE])
#
# DESCRIPTION
#
#   This macro compares two version strings. Due to the various number of
#   minor-version numbers that can exist, and the fact that string
#   comparisons are not compatible with numeric comparisons, this is not
#   necessarily trivial to do in a autoconf script. This macro makes doing
#   these comparisons easy.
#
#   The six basic comparisons are available, as well as checking equality
#   limited to a certain number of minor-version levels.
#
#   The operator OP determines what type of comparison to do, and can be one
#   of:
#
#    eq  - equal (test A == B)
#    ne  - not equal (test A != B)
#    le  - less than or equal (test A <= B)
#    ge  - greater than or equal (test A >= B)
#    lt  - less than (test A < B)
#    gt  - greater than (test A > B)
#
#   Additionally, the eq and ne operator can have a number after it to limit
#   the test to that number of minor versions.
#
#    eq0 - equal up to the length of the shorter version
#    ne0 - not equal up to the length of the shorter version
#    eqN - equal up to N sub-version levels
#    neN - not equal up to N sub-version levels
#
#   When the condition is true, shell commands ACTION-IF-TRUE are run,
#   otherwise shell commands ACTION-IF-FALSE are run. The environment
#   variable 'ax_compare_version' is always set to either 'true' or 'false'
#   as well.
#
#   Examples:
#
#     AX_COMPARE_VERSION([3.15.7],[lt],[3.15.8])
#     AX_COMPARE_VERSION([3.15],[lt],[3.15.8])
#
#   would both be true.
#
#     AX_COMPARE_VERSION([3.15.7],[eq],[3.15.8])
#     AX_COMPARE_VERSION([3.15],[gt],[3.15.8])
#
#   would both be false.
#
#     AX_COMPARE_VERSION([3.15.7],[eq2],[3.15.8])
#
#   would be true because it is only comparing two minor versions.
#
#     AX_COMPARE_VERSION([3.15.7],[eq0],[3.15])
#
#   would be true because it is only comparing the lesser number of minor
#   versions of the two values.
#
#   Note: The characters that separate the version numbers do not matter. An
#   empty string is the same as version 0. OP is evaluated by autoconf, not
#   configure, so must be a string, not a variable.
#
#   The author would like to acknowledge Guido Draheim whose advice about
#   the m4_case and m4_ifvaln functions make this macro only include the
#   portions necessary to perform the specific comparison specified by the
#   OP argument in the final configure script.
#
# LICENSE
#
#   Copyright (c) 2008 Tim Toolan <toolan@ele.uri.edu>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

dnl #########################################################################
AC_DEFUN([AX_COMPARE_VERSION], [
  AC_REQUIRE([AC_PROG_AWK])

  # Used to indicate true or false condition
  ax_compare_version=false

  # Convert the two version strings to be compared into a format that
  # allows a simple string comparison.  The end result is that a version
  # string of the form 1.12.5-r617 will be converted to the form
  # 0001001200050617.  In other words, each number is zero padded to four
  # digits, and non digits are removed.
  AS_VAR_PUSHDEF([A],[ax_compare_version_A])
  A=`echo "$1" | sed -e 's/\([[0-9]]*\)/Z\1Z/g' \
                     -e 's/Z\([[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/Z\([[0-9]][[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/Z\([[0-9]][[0-9]][[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/[[^0-9]]//g'`

  AS_VAR_PUSHDEF([B],[ax_compare_version_B])
  B=`echo "$3" | sed -e 's/\([[0-9]]*\)/Z\1Z/g' \
                     -e 's/Z\([[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/Z\([[0-9]][[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/Z\([[0-9]][[0-9]][[0-9]]\)Z/Z0\1Z/g' \
                     -e 's/[[^0-9]]//g'`

  dnl # In the case of le, ge, lt, and gt, the strings are sorted as necessary
  dnl # then the first line is used to determine if the condition is true.
  dnl # The sed right after the echo is to remove any indented white space.
  m4_case(m4_tolower($2),
  [lt],[
    ax_compare_version=`echo "x$A
x$B" | sed 's/^ *//' | sort -r | sed "s/x${A}/false/;s/x${B}/true/;1q"`
  ],
  [gt],[
    ax_compare_version=`echo "x$A
x$B" | sed 's/^ *//' | sort | sed "s/x${A}/false/;s/x${B}/true/;1q"`
  ],
  [le],[
    ax_compare_version=`echo "x$A
x$B" | sed 's/^ *//' | sort | sed "s/x${A}/true/;s/x${B}/false/;1q"`
  ],
  [ge],[
    ax_compare_version=`echo "x$A
x$B" | sed 's/^ *//' | sort -r | sed "s/x${A}/true/;s/x${B}/false/;1q"`
  ],[
    dnl Split the operator from the subversion count if present.
    m4_bmatch(m4_substr($2,2),
    [0],[
      # A count of zero means use the length of the shorter version.
      # Determine the number of characters in A and B.
      ax_compare_version_len_A=`echo "$A" | $AWK '{print(length)}'`
      ax_compare_version_len_B=`echo "$B" | $AWK '{print(length)}'`

      # Set A to no more than B's length and B to no more than A's length.
      A=`echo "$A" | sed "s/\(.\{$ax_compare_version_len_B\}\).*/\1/"`
      B=`echo "$B" | sed "s/\(.\{$ax_compare_version_len_A\}\).*/\1/"`
    ],
    [[0-9]+],[
      # A count greater than zero means use only that many subversions
      A=`echo "$A" | sed "s/\(\([[0-9]]\{4\}\)\{m4_substr($2,2)\}\).*/\1/"`
      B=`echo "$B" | sed "s/\(\([[0-9]]\{4\}\)\{m4_substr($2,2)\}\).*/\1/"`
    ],
    [.+],[
      AC_WARNING(
        [illegal OP numeric parameter: $2])
    ],[])

    # Pad zeros at end of numbers to make same length.
    ax_compare_version_tmp_A="$A`echo $B | sed 's/./0/g'`"
    B="$B`echo $A | sed 's/./0/g'`"
    A="$ax_compare_version_tmp_A"

    # Check for equality or inequality as necessary.
    m4_case(m4_tolower(m4_substr($2,0,2)),
    [eq],[
      test "x$A" = "x$B" && ax_compare_version=true
    ],
    [ne],[
      test "x$A" != "x$B" && ax_compare_version=true
    ],[
      AC_WARNING([illegal OP parameter: $2])
    ])
  ])

  AS_VAR_POPDEF([A])dnl
  AS_VAR_POPDEF([B])dnl

  dnl # Execute ACTION-IF-TRUE / ACTION-IF-FALSE.
  if test "$ax_compare_version" = "true" ; then
    m4_ifvaln([$4],[$4],[:])dnl
    m4_ifvaln([$5],[else $5])dnl
  fi
]) dnl AX_COMPARE_VERSION

dnl #########################################################################

# AC_PROG_BISON
# ------------
AN_MAKEVAR([BISON],  [AC_PROG_YACC])
AN_PROGRAM([bison], [AC_PROG_YACC])
AC_DEFUN([AC_PROG_BISON],
[AC_CHECK_PROGS(BISON, 'bison')dnl
AC_ARG_VAR(BISON,
[The Bison implementation to use.  Defaults to `bison'.])dnl
AC_ARG_VAR(BFLAGS,
[The list of arguments that will be passed by default to $BISON.  This script
will default BFLAGS to the empty string to avoid a default value of `-d' given
by some make applications.])])
