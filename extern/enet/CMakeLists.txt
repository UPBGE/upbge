# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is: all of this file.
#
# Contributor(s): Jorge Bernal
#
# ***** END GPL LICENSE BLOCK *****

set(INC
        include
)

set(INC_SYS

)

set(SRC
        src/callbacks.c
        src/compress.c
        src/host.c
        src/list.c
        src/packet.c
        src/peer.c
        src/protocol.c
        src/unix.c
        src/win32.c

        include/enet/callbacks.h
        include/enet/enet.h
        include/enet/list.h
        include/enet/protocol.h
        include/enet/time.h
        include/enet/types.h
        include/enet/unix.h
        include/enet/utility.h
        include/enet/win32.h
)

set(LIB
)

#ENET_TODO add right definitions
if(WIN32)
    add_definitions(-DWIN32)
else(WIN32)
    add_definitions(-DHAS_SOCKLEN_T)
endif()

blender_add_lib(extern_enet "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
