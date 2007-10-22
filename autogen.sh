#!/bin/sh
# SVN package rebuild script
# $Id$

# *************************************************************************
# *  Copyright © 2006 Rémi Denis-Courmont.                                *
# *  This program is free software: you can redistribute and/or modify    *
# *  it under the terms of the GNU General Public License as published by *
# *  the Free Software Foundation, version 3 of the license.              *
# *                                                                       *
# *  This program is distributed in the hope that it will be useful,      *
# *  but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# *  GNU General Public License for more details.                         *
# *                                                                       *
# *  You should have received a copy of the GNU General Public License    *
# *  along with this program. If not, see <http://www.gnu.org/licenses/>. *
# *************************************************************************

if test ! -f doc/rdisc6.8 ; then
	echo "You must run this script from your ndisc6 SVN directory."
	exit 1
fi

echo "Creating admin directory ..."
test -d admin || mkdir admin || exit 1

echo "Running \`autopoint' ..."
autopoint -f || {
echo "Error: gettext is probably not on your system, or it does not work."
echo "You need GNU gettext version 0.12.1 or higher."
exit 1
}

unlink po/Makevars.template

gettext_h=""
for d in /usr /usr/local /opt/gettext /usr/pkg $HOME ; do
	if test -f $d/share/gettext/gettext.h ; then
		test -z "$gettext_h" && ln -sf $d/share/gettext/gettext.h \
					include/gettext.h
		gettext_h=ok
	fi
done

echo "Generating \`aclocal.m4' with aclocal ..."
aclocal -I m4 || {
echo "Error: autoconf is probably not on your system, or it does not work."
echo "You need GNU autoconf 2.59c or higher, as well as GNU gettext 0.12.1."
exit 1
}
echo "Generating \`config.h.in' with autoheader ..."
autoheader || exit 1
echo "Generating \`Makefile.in' with automake ..."
automake -Wall --add-missing || {
echo "Error: automake is probably not on your system, or it is too old."
echo "You need GNU automake 1.7 higher to rebuild this package."
exit 1
}
echo "Generating \`configure' script with autoconf ..."
autoconf || exit 1
echo "Done."

test -z $gettext_h && {
echo "Error: can't find <gettext.h> convenience C header."
echo "Please put a link to it by hand as include/gettext.h"
}

echo ""
echo "Type \`./configure' to configure the package for your system"
echo "(type \`./configure -- help' for help)."
echo "Then you can use the usual \`make', \`make install', etc."

